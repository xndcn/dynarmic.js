// Recompute diagnostic summaries and audit the rejected region-size experiment.
// Run from the repository root: node <this-file> <extracted-evidence-dir>
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const root = process.argv[2];
assert(root && process.argv.length === 3, 'usage: verify.mjs <extracted-evidence-dir>');
const {summarizeCpuProfile} = await import(pathToFileURL(path.resolve('tests/bench/cpu_profile.mjs')));
const {parseCoremark} = await import(pathToFileURL(path.resolve('tests/bench/coremark_protocol.mjs')));
const readJson = name => JSON.parse(fs.readFileSync(path.join(root, name), 'utf8'));
const sha256 = data => createHash('sha256').update(data).digest('hex');
const observations = readJson('observations.json');
assert.equal(observations.schema, 'dynarmic.js/region-size-observations-v1');
assert.deepEqual(observations.limits, {maxBlocks: 128, maxInstructions: 2000, maxGuestDistance: 4096});
assert.match(observations.outcome, /^REJECTED/);
assert(observations.emittedBytesRatio > 1.10);

const workload = readJson('workload-manifest.json');
const nativeText = fs.readFileSync(path.join(root, 'native.txt'), 'utf8');
const native = parseCoremark(nativeText, workload.iterations);
function checkSample(prefix) {
  const text = fs.readFileSync(path.join(root, `${prefix}-sample.txt`), 'utf8');
  const lines = text.trim().split('\n').filter(line => line.startsWith('COREMARK_SAMPLE '));
  assert.equal(lines.length, 1, `${prefix}: sample count`);
  const sample = JSON.parse(lines[0].slice(16));
  assert.equal(sample.haltReason, 0x01000000, `${prefix}: halt`);
  const metrics = parseCoremark(sample.stdout, workload.iterations);
  assert.deepEqual(metrics.crc, native.crc, `${prefix}: native/guest CRC`);
  assert(Number.isFinite(sample.runMs) && sample.runMs > 0);
  return {sample, metrics};
}
const baselineSample = checkSample('baseline-a64-flat');
const candidateSample = checkSample('candidate-a64-flat');
checkSample('candidate-concurrent');

for (const prefix of ['baseline-a64-flat', 'candidate-a64-flat', 'candidate-concurrent']) {
  const raw = fs.readFileSync(path.join(root, `${prefix}.cpuprofile`));
  const profile = JSON.parse(raw);
  for (const [suffix, pattern] of [['all', null], ['guest', '^a64_']]) {
    const saved = readJson(`${prefix}-${suffix}.json`);
    assert.equal(saved.input.sha256, sha256(raw), `${prefix}: ${suffix} input hash`);
    const {input, ...expected} = saved;
    assert.deepEqual(summarizeCpuProfile(profile, pattern), expected, `${prefix}: ${suffix} summary`);
  }
}

function observedSample(sample) {
  return {haltReason: sample.haltReason, runMs: sample.runMs,
    kernelSeconds: Number(sample.stdout.match(/Total time \(secs\): ([^\n]+)/)[1]), compile: sample.compile};
}
assert.deepEqual(observations.baseline, observedSample(baselineSample.sample));
assert.deepEqual(observations.candidate, observedSample(candidateSample.sample));
assert.equal(observations.emittedBytesRatio, candidateSample.sample.compile.bytes / baselineSample.sample.compile.bytes);
assert.equal(observations.diagnosticRunRatio, candidateSample.sample.runMs / baselineSample.sample.runMs);
assert.equal(observations.diagnosticKernelRatio,
  observations.candidate.kernelSeconds / observations.baseline.kernelSeconds);

const modules = {
  baseline: readJson('baseline-modules.json'),
  candidate: readJson('candidate-modules.json'),
};
assert.equal(modules.baseline.length, baselineSample.sample.compile.blocks);
assert.equal(modules.candidate.length, candidateSample.sample.compile.blocks);
const entries = new Set(observations.regions.map(region => region.entry));
assert.equal(entries.size, observations.regions.length);
for (const region of observations.regions) {
  for (const side of ['baseline', 'candidate']) {
    const expected = region[side];
    const actual = modules[side].find(row => row.entry === region.entry) || null;
    assert.deepEqual(actual, expected, `${side}: ${region.entry} module summary`);
    if (expected === null) continue;
    const stem = `${side}-${expected.file.replace('_', '-').replace(/\.wat$/, '')}`;
    const wat = fs.readFileSync(path.join(root, `${stem}.wat`), 'utf8');
    const wasm = fs.readFileSync(path.join(root, `${stem}.wasm`));
    assert(wat.includes(`(func $${region.entry} `), `${stem}: entry identity`);
    assert.equal(wasm.length, expected.bytes, `${stem}: binary size`);
    assert.equal(Buffer.byteLength(wat), expected.watBytes, `${stem}: WAT size`);
    assert.equal((wat.match(/return_call_indirect/g) || []).length, expected.externalTransfers,
      `${stem}: static external transfer sites`);
    const code = wat.replace(/\(;.*?;\)/gs, '');
    const tables = [...code.matchAll(/\(br_table\s+([^)]+)\)/gs)]
      .map(match => match[1].trim().split(/\s+/).length);
    assert.equal(tables.length, expected.dispatchSites, `${stem}: dispatch sites`);
    assert.equal(tables.length ? Math.max(...tables) : 0, expected.dispatchTargets,
      `${stem}: dispatch labels`);
  }
}

const environment = readJson('environment.json');
assert.equal(environment.baseRevision, observations.baseRevision);
assert.equal(environment.builds.length, 2);
assert.deepEqual(environment.builds[0], environment.builds[1]);
assert.match(environment.provenanceLimitations, /not independent attestation/);
const patch = fs.readFileSync(path.join(root, 'source.patch'), 'utf8');
assert.match(patch, /^-    size_t max_blocks = 64;$/m);
assert.match(patch, /^\+    size_t max_blocks = 128;$/m);
assert(!patch.includes('max_instructions = 4000'));
assert.match(fs.readFileSync(path.join(root, 'native-tests.txt'), 'utf8'),
  /100% tests passed, 0 tests failed out of 2/);

const inventory = fs.readFileSync(path.join(root, 'SHA256SUMS'), 'utf8').trim().split('\n');
for (const line of inventory) {
  const match = line.match(/^([0-9a-f]{64})  (.+)$/);
  assert(match, `invalid inventory line: ${line}`);
  const data = fs.readFileSync(path.join(root, match[2]));
  assert.equal(sha256(data), match[1], `inventory hash: ${match[2]}`);
}
console.log(`region-size evidence PASS profiles=3 files=${inventory.length}`);
