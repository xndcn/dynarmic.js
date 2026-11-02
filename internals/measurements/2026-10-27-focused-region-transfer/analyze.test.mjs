import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {gzipSync} from 'node:zlib';
import {analyzeReports} from './analyze.mjs';
import {analyzeRegionTransferDiagnostics, checkRunTransferConservation} from '../../../tests/bench/region_transfer_diagnostics.mjs';
import {BASELINE_FOCUS, CANDIDATE_FOCUS, RETURNS, SOURCE, TARGET, makeReports, refreshReport,
  u64} from './test_fixtures.mjs';

const analyze = (baseline, candidate) => analyzeReports(baseline, candidate);
function rejection(mutate, pattern, refresh = false) {
  const {baseline, candidate} = makeReports();
  mutate(baseline, candidate);
  const left = refresh ? refreshReport(baseline) : baseline;
  const right = refresh ? refreshReport(candidate) : candidate;
  assert.throws(() => analyze(left, right), pattern);
}

test('valid focused pair produces whole-Run mechanism evidence and no performance fields', () => {
  const {baseline, candidate} = makeReports();
  const before = structuredClone(baseline);
  const result = analyze(baseline, candidate);
  assert.deepEqual(baseline, before);
  assert.equal(result.schema, 'dynarmic.js/focused-region-transfer-analysis-v1');
  assert.match(result.scope, /one-Run mechanism accounting/);
  assert.match(result.scope, /no performance qualification/);
  assert.deepEqual(result.focus, {baseline: BASELINE_FOCUS, candidate: CANDIDATE_FOCUS});
  assert.equal(result.provenance.memoryMode, '0');
  assert.equal(result.provenance.memoryModeLabel, 'callbacks/thunks');
  assert.equal(result.runs.baseline.generations.length, 3);
  assert.equal(result.runs.candidate.transferRegions.length, 3);
  assert.equal(result.runs.baseline.transferConservation.conserved, true);
  assert.equal(result.runs.candidate.transferConservation.conserved, true);
  assert.equal(result.wholeRunTotals.delta.transferCount, '1');
  assert.equal(result.newlyExternalizedDirectEdges.length, 1);
  assert.doesNotMatch(JSON.stringify(result), /runMs|ticks|throughput|performanceVerdict/);
});

test('repeated analysis is deterministic byte for byte', () => {
  const {baseline, candidate} = makeReports();
  const first = analyze(baseline, candidate);
  const second = analyze(baseline, candidate);
  assert.deepEqual(second, first);
  assert.equal(JSON.stringify(second), JSON.stringify(first));
});

test('runs retain complete raw generations and transfer regions exactly', () => {
  const {baseline, candidate} = makeReports();
  const result = analyze(baseline, candidate);
  for (const [side, report] of Object.entries({baseline, candidate})) {
    assert.deepEqual(result.runs[side].generations, report.regionDiagnostics.regions);
    assert.deepEqual(result.runs[side].transferRegions, report.regionTransferDiagnostics.regions);
  }
});

test('legitimate timestamps, Run times and native/guest timing output differences are pair-valid', () => {
  const {baseline, candidate} = makeReports();
  const retime = (stdout, ticks) => stdout.replace('Total ticks      : 1000000', `Total ticks      : ${ticks}`)
    .replace('Total time (secs): 1.000000', `Total time (secs): ${(ticks / 1e6).toFixed(6)}`)
    .replace('Iterations/Sec   : 200000.000000',
      `Iterations/Sec   : ${(200000 / (ticks / 1e6)).toFixed(6)}`);
  baseline.timestamp = '2026-10-26T00:00:00.000Z';
  candidate.timestamp = '2026-10-28T00:00:00.000Z';
  baseline.sample.runMs = 2200;
  candidate.sample.runMs = 2100;
  baseline.native.stdout = retime(baseline.native.stdout, 1100000);
  baseline.sample.stdout = retime(baseline.sample.stdout, 1200000);
  candidate.native.stdout = retime(candidate.native.stdout, 1300000);
  candidate.sample.stdout = retime(candidate.sample.stdout, 1400000);
  const result = analyze(refreshReport(baseline), refreshReport(candidate));
  assert.equal(result.schema, 'dynarmic.js/focused-region-transfer-analysis-v1');
});

test('compile-time focus may change JS but Wasm identity must differ', () => {
  const {baseline, candidate} = makeReports();
  candidate.artifact.jsSha256 = baseline.artifact.jsSha256;
  assert.equal(analyze(baseline, candidate).schema, 'dynarmic.js/focused-region-transfer-analysis-v1');
  candidate.artifact.wasmSha256 = baseline.artifact.wasmSha256;
  assert.throws(() => analyze(baseline, candidate), /candidate Wasm hash must differ/);
});

const focusRejections = [
  ['missing baseline focus', (a) => { delete a.build.a64RegionFocus; }, /exact report schema|own focus provenance/],
  ['wrong baseline focus', (a) => { a.build.a64RegionFocus = CANDIDATE_FOCUS; }, /own focus provenance/],
  ['wrong candidate focus', (a, b) => { b.build.a64RegionFocus = ''; }, /own focus provenance/],
  ['numeric memory mode', (a) => { a.memoryMode = 0; }, /memoryMode/],
  ['flat-window mode', (a) => { a.memoryMode = '2'; }, /memoryMode/],
  ['dirty source', (a) => { a.git.dirty = true; }, /clean git/],
  ['non-Release build', (a) => { a.build.type = 'Debug'; }, /Release/],
  ['profiling disabled', (a) => { a.build.profile = false; }, /profile build/],
  ['architecture', (a) => { a.architecture = 'a32'; }, /architecture/],
  ['bad artifact hash', (a) => { a.artifact.wasmSha256 = 'xyz'; }, /artifact hash/],
];
for (const [name, mutate, pattern] of focusRejections) {
  test(`single-report validation rejects ${name}`, () => rejection(mutate, pattern));
}

const pairRejections = [
  ['source', (a, b) => { b.git.revision = '4'.repeat(40); }, /source identity/],
  ['upstream', (a, b) => { b.git.upstream = '4'.repeat(40); }, /upstream identity/],
  ['workload manifest', (a, b) => { b.workload.revision = '4'.repeat(40); }, /workload identity/],
  ['workload hash', (a, b) => { b.workloadSha256 = '4'.repeat(64); }, /workload identity/],
  ['build besides focus', (a, b) => { b.build.configCxxFlags = '-O2'; }, /normalized build identity/],
  ['mode', (a, b) => { b.memoryMode = '1'; }, /memoryMode/],
  ['host', (a, b) => { b.host.cpu = 'other'; }, /host identity/],
  ['Node', (a, b) => { b.node = 'v98.0.0'; }, /Node identity/],
  ['V8', (a, b) => { b.v8 = '98.0.0'; }, /V8 identity/],
  ['nonempty nodeOptions', (a) => { a.runtime.nodeOptions = '--trace-wasm'; }, /nodeOptions/],
  ['affinity', (a, b) => { b.runtime.cpuAffinity = '1'; }, /runtime identity/],
];
for (const [name, mutate, pattern] of pairRejections) {
  test(`pair validation rejects ${name} mismatch`, () => rejection(mutate, pattern));
}

const diagnosticRejections = [
  ['missing region diagnostics', (a) => { delete a.regionDiagnostics; }, /exact report schema|region diagnostics/],
  ['v1 region diagnostics', (a) => { a.regionDiagnostics.schema = 'dynarmic.js/region-diagnostics-v1'; }, /region-diagnostics-v2/],
  ['missing transfer diagnostics', (a) => { delete a.regionTransferDiagnostics; }, /exact report schema|transfer/],
  ['wrong transfer diagnostics', (a) => { a.regionTransferDiagnostics.schema = 'wrong'; }, /schema/],
  ['missing profile', (a) => { a.sample.profile = null; }, /profile/],
  ['wrong profile schema', (a) => { a.sample.profile.schema = 'wrong'; }, /profile-v1/],
  ['multiple Runs', (a) => { a.sample.profile.runCount = 2; }, /one Run/],
  ['Step contamination', (a) => { a.sample.profile.stepCount = 1; }, /zero Steps/],
  ['negative profile counter', (a) => { a.sample.profile.dispatchCount = -1; }, /profile.dispatchCount/],
  ['unsafe profile counter', (a) => { a.sample.profile.dispatchCount = Number.MAX_SAFE_INTEGER + 1; }, /profile.dispatchCount/],
  ['abnormal halt', (a) => { a.sample.haltReason = 0; }, /normal halt/],
  ['stale guest CRC', (a) => { a.sample.metrics.crc.crcfinal = '0000'; }, /guest metrics/],
  ['stdout CRC', (a) => { a.sample.stdout = a.sample.stdout.replace('0x4983', '0xffff'); }, /guest metrics|CRC/],
  ['native CRC mismatch', (a) => { a.native.metrics.crc.crcfinal = '0000'; }, /native metrics/],
  ['raw history removal', (a) => { a.regionDiagnostics.regions.pop(); }, /region|fresh derived report/],
  ['raw history misalignment', (a) => { a.regionTransferDiagnostics.regions[0].callerEntryDescriptor = TARGET; }, /caller descriptor|fresh derived report/],
  ['stale duplication', (a) => { a.duplication.totals.moduleBytes += 1; }, /fresh derived report/],
  ['stale transfer analysis', (a) => { a.transferAnalysis.totals.transferCount = u64(0); }, /fresh derived report/],
  ['stale conservation', (a) => { a.transferConservation.conserved = false; }, /fresh derived report/],
  ['fabricated conservation', (a) => {
    a.transferConservation = {...a.transferConservation, conserved: true, expectedEntryCount: u64(0)};
  }, /fresh derived report/],
];
for (const [name, mutate, pattern] of diagnosticRejections) {
  test(`diagnostic validation rejects ${name}`, () => rejection(mutate, pattern));
}

test('independently recomputed non-conserved raw history fails even with refreshed serialization', () => {
  const {baseline, candidate} = makeReports();
  baseline.regionTransferDiagnostics.regions[0].entryCount = u64(6);
  baseline.transferAnalysis = analyzeRegionTransferDiagnostics(baseline.regionTransferDiagnostics);
  baseline.transferConservation = checkRunTransferConservation(baseline.transferAnalysis,
    baseline.sample.profile.dispatchCount);
  assert.equal(baseline.transferConservation.conserved, false);
  assert.throws(() => analyze(baseline, candidate), /conservation/);
});

test('focus mechanism requires the exact baseline edge, candidate membership and all return descriptors', () => {
  rejection((a) => {
    a.regionTransferDiagnostics.regions[0].exits = a.regionTransferDiagnostics.regions[0].exits.filter(row =>
      row.exitBlockDescriptor !== SOURCE);
    a.regionTransferDiagnostics.regions[0].entryCount = u64(0);
  }, /baseline focused external direct edge/, true);
  rejection((a, b) => {
    b.regionDiagnostics.regions[0].blocks = b.regionDiagnostics.regions[0].blocks.filter(row =>
      row.descriptor !== TARGET);
  }, /exactly one candidate focused generation/, true);
  rejection((a, b) => {
    b.regionDiagnostics.regions[0].blocks = b.regionDiagnostics.regions[0].blocks.filter(row =>
      row.descriptor !== RETURNS[0]);
    b.regionTransferDiagnostics.regions[0].exits[1].exitBlockDescriptor = TARGET;
  }, /focused return descriptors/, true);
  rejection((a, b) => {
    const row = b.regionDiagnostics.regions[0].blocks.find(block => block.descriptor === RETURNS[1]);
    row.emitted = false;
    row.entryReachable = false;
  }, /focused return descriptors/, true);
  rejection((a, b) => {
    b.regionDiagnostics.regions[0].layout = 'fallback';
    const row = b.regionDiagnostics.regions[0].blocks.find(block => block.descriptor === RETURNS[1]);
    row.entryReachable = false;
  }, /focused return descriptors/, true);
  rejection((a, b) => {
    b.regionTransferDiagnostics.regions[0].exits.push({exitSite: 2, exitBlockDescriptor: SOURCE,
      kind: 'external-direct-call', staticTargetDescriptor: TARGET, count: u64(0),
      lastDynamicTargetDescriptor: u64(0), lastDynamicTargetPc: u64(0)});
  }, /candidate focused external direct exit/, true);
});

function cliDirectory(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'focused-transfer-analyze-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const {baseline, candidate} = makeReports();
  const plain = path.join(directory, 'baseline.json');
  const gzip = path.join(directory, 'candidate.json.gz');
  fs.writeFileSync(plain, JSON.stringify(baseline));
  fs.writeFileSync(gzip, gzipSync(JSON.stringify(candidate)));
  return {directory, plain, gzip, output: path.join(directory, 'analysis.json')};
}
const cli = fileURLToPath(new URL('./analyze.mjs', import.meta.url));
const runCli = args => spawnSync(process.execPath, [cli, ...args], {encoding: 'utf8'});

test('CLI accepts plain and gzip reports and refuses overwrite', t => {
  const paths = cliDirectory(t);
  const pass = runCli([paths.plain, paths.gzip, paths.output]);
  assert.equal(pass.status, 0, pass.stderr);
  assert.equal(JSON.parse(fs.readFileSync(paths.output)).schema,
    'dynarmic.js/focused-region-transfer-analysis-v1');
  const existing = fs.readFileSync(paths.output);
  const again = runCli([paths.plain, paths.gzip, paths.output]);
  assert.equal(again.status, 2);
  assert.match(again.stderr, /refusing to overwrite/);
  assert.deepEqual(fs.readFileSync(paths.output), existing);
});

test('CLI invalid input produces no output', t => {
  const paths = cliDirectory(t);
  fs.writeFileSync(paths.gzip, gzipSync('{}'));
  const result = runCli([paths.plain, paths.gzip, paths.output]);
  assert.equal(result.status, 2);
  assert(!fs.existsSync(paths.output));
});
