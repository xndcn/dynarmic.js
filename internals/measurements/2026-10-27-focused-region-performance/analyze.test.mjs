import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {fixture, output} from '../../../tests/bench/coremark_fixtures.mjs';
import {parseCoremark} from '../../../tests/bench/coremark_protocol.mjs';
import {CANDIDATE_FOCUS, FOCUSED_METHODOLOGY, RAW_SCHEMA, analyzeFocusedCoremark} from './analyze.mjs';

const CASE = 'a64-thunk';
const clone = value => structuredClone(value);

function rawFixture() {
  const baselineReport = fixture(false, 10_000_000);
  const candidateReport = fixture(false, 10_000_000);
  candidateReport.build.a64RegionFocus = CANDIDATE_FOCUS;
  candidateReport.artifact.wasmSha256 = 'c'.repeat(64);
  const baselineSamples = baselineReport.cases.find(item => item.name === CASE).samples;
  const candidateSamples = candidateReport.cases.find(item => item.name === CASE).samples;
  return {
    schema: RAW_SCHEMA,
    timestamp: baselineReport.timestamp,
    node: baselineReport.node,
    v8: baselineReport.v8,
    runtime: clone(baselineReport.runtime),
    host: clone(baselineReport.host),
    git: {baseline: clone(baselineReport.git), candidate: clone(candidateReport.git)},
    build: {baseline: clone(baselineReport.build), candidate: clone(candidateReport.build)},
    configuration: {
      baseline: {regionDiagnostics: false, focusTestExceptions: false, examples: true},
      candidate: {regionDiagnostics: false, focusTestExceptions: false, examples: true},
    },
    artifact: {baseline: clone(baselineReport.artifact), candidate: clone(candidateReport.artifact)},
    workload: clone(baselineReport.workload),
    workloadSha256: baselineReport.workloadSha256,
    methodology: clone(FOCUSED_METHODOLOGY),
    architecture: 'a64', memoryMode: '0', declaredPairs: 10,
    native: clone(baselineReport.native),
    pairs: Array.from({length: 10}, (_, pair) => ({
      pair, order: pair % 2 ? ['candidate', 'baseline'] : ['baseline', 'candidate'],
      baseline: {...clone(baselineSamples[pair % 5]), process: pair},
      candidate: {...clone(candidateSamples[pair % 5]), process: pair},
    })),
  };
}

function setTicks(sample, ticks) {
  sample.stdout = output(ticks);
  sample.metrics = parseCoremark(sample.stdout, 4000);
  sample.runMs = ticks / 1000 + 50;
}

test('focused CoreMark analysis computes preregistered speedups and pair behavior', () => {
  const raw = rawFixture();
  raw.pairs.forEach((pair, index) => {
    setTicks(pair.baseline, 11_000_000 + index * 100_000);
    setTicks(pair.candidate, 10_000_000 + index * 100_000);
  });
  const result = analyzeFocusedCoremark(raw);
  assert.equal(result.schema, 'dynarmic.js/focused-coremark-comparison-v1');
  assert.equal(result.durationQualified, true);
  assert.equal(result.pass, true);
  assert.equal(result.headline.kernel.speedup, 11_450 / 10_450);
  assert.equal(result.headline.kernel.latencyRatio, 10_450 / 11_450);
  assert.equal(result.headline.run.speedup, 11_500 / 10_500);
  assert.equal(result.paired.kernel.outcomes.candidateFaster, 10);
  assert.equal(result.paired.kernel.outcomes.candidateSlower, 0);
  assert.equal(result.paired.kernel.baselineFirst.ratios.length, 5);
  assert.equal(result.paired.kernel.candidateFirst.ratios.length, 5);
  assert.equal(result.baseline.kernelMinMs, 11_000);
  assert.equal(result.baseline.kernelMaxMs, 11_900);
  assert.equal(result.baseline.kernelMadMs, 250);
  assert.equal(result.baseline.kernelRelativeMad, 250 / 11_450);
});

test('focused CoreMark analysis retains slowdowns and does not use compile as speedup', () => {
  const raw = rawFixture();
  for (const pair of raw.pairs) {
    setTicks(pair.candidate, 11_000_000);
    pair.candidate.compile.translateMs = 0;
  }
  const result = analyzeFocusedCoremark(raw);
  assert.equal(result.pass, false);
  assert.equal(result.headline.kernel.pass, false);
  assert(result.headline.kernel.speedup < 1);
  assert(result.headline.run.speedup < 1);
  assert.equal(result.secondary.compile.pass, true);
  assert.equal(result.paired.run.outcomes.candidateSlower, 10);
});

for (const [name, mutate, pattern] of [
  ['wrong raw schema', raw => { raw.schema = 'other'; }, /schema/],
  ['wrong architecture', raw => { raw.architecture = 'a32'; }, /architecture/],
  ['numeric memory mode', raw => { raw.memoryMode = 0; }, /memoryMode/],
  ['wrong candidate focus', raw => { raw.build.candidate.a64RegionFocus = ''; }, /candidate focus/],
  ['nonempty baseline focus', raw => { raw.build.baseline.a64RegionFocus = CANDIDATE_FOCUS; }, /baseline focus/],
  ['non-Release build', raw => { raw.build.candidate.type = 'Debug'; }, /Release/],
  ['profiling enabled', raw => { raw.build.candidate.profile = true; }, /profiling/],
  ['diagnostics enabled', raw => { raw.configuration.baseline.regionDiagnostics = true; }, /diagnostics/],
  ['test exceptions enabled', raw => { raw.configuration.candidate.focusTestExceptions = true; }, /exceptions/],
  ['examples disabled', raw => { raw.configuration.candidate.examples = false; }, /examples/],
  ['extra build difference', raw => { raw.build.candidate.configCxxFlags = '-O2'; }, /normalized build/],
  ['dirty source', raw => { raw.git.candidate.dirty = true; }, /clean/],
  ['different source', raw => { raw.git.candidate.revision = '0'.repeat(40); }, /source identity/],
  ['changed workload identity', raw => { raw.workloadSha256 = '0'.repeat(64); }, /workload identity/],
  ['changed method', raw => { raw.methodology = {}; }, /methodology/],
  ['nonempty NODE_OPTIONS', raw => { raw.runtime.nodeOptions = '--trace-opt'; }, /NODE_OPTIONS/],
  ['missing CPU affinity', raw => { delete raw.runtime.cpuAffinity; }, /CPU affinity/],
  ['different JS artifacts', raw => { raw.artifact.candidate.jsSha256 = '0'.repeat(64); }, /JS artifacts/],
  ['native compiler flags mismatch', raw => {
    raw.native.stdout = output(10_000_000).replaceAll('-O2', '-O3');
    raw.native.metrics = parseCoremark(raw.native.stdout, 4000);
  }, /native compiler flags/],
  ['missing pair', raw => { raw.pairs.pop(); }, /pair count/],
  ['wrong declared pair count', raw => { raw.declaredPairs = 5; }, /pair count/],
  ['wrong schedule', raw => { raw.pairs[0].order.reverse(); }, /order/],
  ['short guest sample', raw => { setTicks(raw.pairs[0].candidate, 9_999_999); }, /10 seconds/],
  ['abnormal halt', raw => { raw.pairs[0].baseline.haltReason = 0; }, /halt/],
  ['CRC mismatch', raw => {
    const sample = raw.pairs[0].candidate;
    sample.stdout = output(10_000_000, '1234');
    sample.metrics = parseCoremark(sample.stdout, 4000);
  }, /CRC/],
  ['kernel exceeds Run', raw => { raw.pairs[0].candidate.runMs = 1; }, /kernel time exceeds Run/],
  ['profile payload present', raw => { raw.pairs[0].candidate.profile = {}; }, /profile must be null/],
  ['region diagnostics present', raw => { raw.pairs[0].candidate.regionDiagnostics = {}; }, /diagnostic payload/],
  ['tampered parsed metrics', raw => { raw.pairs[0].candidate.metrics.ticks++; }, /parsed output/],
  ['invalid compile time', raw => { raw.pairs[0].candidate.compile.translateMs = -1; }, /compile/],
  ['identical artifacts', raw => { raw.artifact.candidate.wasmSha256 = raw.artifact.baseline.wasmSha256; }, /Wasm artifacts/],
]) test(`focused CoreMark analysis rejects ${name}`, () => {
  const raw = rawFixture();
  mutate(raw);
  assert.throws(() => analyzeFocusedCoremark(raw), pattern);
});

test('focused CoreMark no-regression gates retain their exact boundaries', () => {
  const passingKernel = rawFixture();
  passingKernel.pairs.forEach(pair => setTicks(pair.candidate, 10_500_000));
  assert.equal(analyzeFocusedCoremark(passingKernel).headline.kernel.pass, true);
  const failingKernel = rawFixture();
  failingKernel.pairs.forEach(pair => setTicks(pair.candidate, 10_526_316));
  assert.equal(analyzeFocusedCoremark(failingKernel).headline.kernel.pass, false);

  const passingRun = rawFixture();
  passingRun.pairs.forEach(pair => { pair.candidate.runMs = 10_050 * 1.15; });
  assert.equal(analyzeFocusedCoremark(passingRun).headline.run.pass, true);
  const failingRun = rawFixture();
  failingRun.pairs.forEach(pair => { pair.candidate.runMs = 10_050 * 1.15 + 0.01; });
  assert.equal(analyzeFocusedCoremark(failingRun).headline.run.pass, false);

  const passingCompile = rawFixture();
  passingCompile.pairs.forEach(pair => { pair.candidate.compile.translateMs = 0.115; });
  assert.equal(analyzeFocusedCoremark(passingCompile).secondary.compile.pass, true);
  const failingCompile = rawFixture();
  failingCompile.pairs.forEach(pair => { pair.candidate.compile.translateMs = 0.116; });
  const failingCompileResult = analyzeFocusedCoremark(failingCompile);
  assert.equal(failingCompileResult.secondary.compile.pass, false);
  assert.equal(failingCompileResult.pass, false);

  const passingBytes = rawFixture();
  passingBytes.pairs.forEach(pair => { pair.candidate.compile.bytes = 110; });
  assert.equal(analyzeFocusedCoremark(passingBytes).secondary.emittedBytes.pass, true);
  const failingBytes = rawFixture();
  failingBytes.pairs.forEach(pair => { pair.candidate.compile.bytes = 111; });
  const failingBytesResult = analyzeFocusedCoremark(failingBytes);
  assert.equal(failingBytesResult.secondary.emittedBytes.pass, false);
  assert.equal(failingBytesResult.pass, false);
});

test('focused CoreMark analyzer CLI preserves a valid budget failure', t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'focused-coremark-failure-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const input = path.join(directory, 'raw.json'), outputFile = path.join(directory, 'comparison.json');
  const raw = rawFixture();
  raw.pairs.forEach(pair => { pair.candidate.runMs *= 2; });
  fs.writeFileSync(input, JSON.stringify(raw));
  const cli = fileURLToPath(new URL('./analyze.mjs', import.meta.url));
  const result = spawnSync(process.execPath, [cli, input, outputFile], {encoding: 'utf8'});
  assert.equal(result.status, 1, result.stderr);
  assert.equal(JSON.parse(fs.readFileSync(outputFile, 'utf8')).pass, false);
});

test('focused CoreMark analyzer CLI is non-overwriting', t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'focused-coremark-analysis-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const input = path.join(directory, 'raw.json'), outputFile = path.join(directory, 'comparison.json');
  fs.writeFileSync(input, JSON.stringify(rawFixture()));
  const cli = fileURLToPath(new URL('./analyze.mjs', import.meta.url));
  const first = spawnSync(process.execPath, [cli, input, outputFile], {encoding: 'utf8'});
  assert.equal(first.status, 0, first.stderr);
  assert.match(first.stdout, /kernelSpeedup=1\.000000/);
  const retained = fs.readFileSync(outputFile, 'utf8');
  const second = spawnSync(process.execPath, [cli, input, outputFile], {encoding: 'utf8'});
  assert.equal(second.status, 2);
  assert.equal(fs.readFileSync(outputFile, 'utf8'), retained);
});
