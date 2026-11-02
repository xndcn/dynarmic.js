import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {cases} from './protocol.mjs';
import {report as wasmReport, samples} from './fixtures.mjs';
import {nativeMethodology, pairOrder, parseNativeOutput, summarizeNativePairs, validateNativeReport} from './native_protocol.mjs';
import {parseArgs} from './native_run.mjs';

const output = samples => samples.map(s => `BENCHMARK ${JSON.stringify(s)}`).join('\n') + '\nPASS\n';
function observation(native = false, ms = 1) {
  const observations = samples().map(({process, ...sample}) => ({...sample, ms, minstrPerSec: 1 / ms,
    compile: native ? null : sample.compile}));
  return {samples: observations, stdout: output(observations), stderr: '', exitCode: 0, signal: null, wallMs: 10};
}
function report(runs = 5) {
  const base = wasmReport();
  return {...base, schema: 'dynarmic.js/native-comparison-v1', methodology: nativeMethodology, runs,
    runtime: {...base.runtime, cpuAffinity: '0-3'},
    build: {...base.build, regionDiagnostics: false, testExceptions: false},
    qualification: runs >= 5 ? 'comparison' : 'smoke-only', collectorSha256: 'f'.repeat(64),
    nativeBuild: {backend: 'upstream-native', ...base.git, upstreamDirty: false, type: 'Release', architecture: 'x86_64', compilerId: 'GNU', compilerVersion: 'test',
      systemProcessor: 'x86_64', cxxFlags: '', configCxxFlags: '-O3', linkFlags: '', configLinkFlags: '',
      options: {DYNARMIC_TESTS: 'OFF', DYNARMIC_USE_BUNDLED_EXTERNALS: 'ON', DYNARMIC_FRONTENDS: 'A32;A64', DYNARMIC_IGNORE_ASSERTS: 'OFF'}},
    cases: cases.map((spec, index) => {
      const pairs = Array.from({length: runs}, (_, round) => ({round, order: pairOrder(round, index),
        native: observation(true), wasm: observation(false, 2)}));
      return {name: spec.name, architecture: spec.architecture, memoryMode: spec.memoryMode, workloadSha256: 'c'.repeat(64),
        artifacts: {nativeSha256: 'a'.repeat(64), jsSha256: 'b'.repeat(64), wasmSha256: 'c'.repeat(64)},
        pairs, summary: summarizeNativePairs(pairs)};
    })};
}

test('native comparison retains every cold/warm observation and paired ratio direction', () => {
  const r = validateNativeReport(report(), true);
  const s = r.cases[0].summary;
  assert.equal(s.native.warmMedianMs, 1); assert.equal(s.wasm.warmMedianMs, 2);
  assert.equal(s.nativeOverWasmThroughput.warmMedian, 2);
  assert.equal(s.nativeOverWasmThroughput.warmMad, 0);
  assert.equal(s.nativeOverWasmThroughput.coldMedian, 2);
  assert.deepEqual(s.wasm.warmIndexMedianMs, Array(8).fill(2));
});
test('ratios use paired process medians rather than a ratio of unpaired totals', () => {
  const pairs = [1, 10, 100].map((n, i) => ({native: observation(true, n), wasm: observation(false, [2, 100, 100][i])}));
  const summary = summarizeNativePairs(pairs);
  assert.equal(summary.nativeOverWasmThroughput.warmMedian, 2);
  assert.equal(summary.nativeOverWasmThroughput.warmMad, 1);
  assert.deepEqual(summary.nativeOverWasmThroughput.warmPairs, [2, 10, 1]);
});
test('smoke reports validate but cannot qualify a performance comparison', () => {
  validateNativeReport(report(1));
  assert.throws(() => validateNativeReport(report(1), true), /at least five/);
});
for (const [name, change] of [
  ['missing PASS', s => s.replace('PASS', '')],
  ['guest failure', s => s + 'WRONG RESULT'],
  ['truncated samples', s => s.split('\n').slice(1).join('\n')],
  ['out of order', s => s.replace('"runIndex":1', '"runIndex":2')],
  ['zero duration', s => s.replace('"ms":1', '"ms":0')],
  ['fabricated compilation', s => s.replace('"compile":null', '"compile":{}')],
  ['inconsistent throughput', s => s.replace('"minstrPerSec":1', '"minstrPerSec":2')],
]) test(`native parser rejects ${name}`, () => assert.throws(() => parseNativeOutput(change(observation(true).stdout))));

for (const [name, change] of [
  ['profile build', r => { r.build.profile = true; }],
  ['missing CPU affinity', r => { delete r.runtime.cpuAffinity; }],
  ['non-string hash', r => { r.cases[0].artifacts.nativeSha256 = [r.cases[0].artifacts.nativeSha256]; }],
  ['non-string revision', r => { r.git.revision = [r.git.revision]; }],
  ['diagnostics build', r => { r.build.regionDiagnostics = true; }],
  ['test exceptions', r => { r.build.testExceptions = true; }],
  ['unsupported platform', r => { r.host.platform = 'win32'; }],
  ['wrong native architecture', r => { r.nativeBuild.architecture = 'arm64'; }],
  ['debug native', r => { r.nativeBuild.type = 'Debug'; }],
  ['different upstream', r => { r.nativeBuild.upstream = 'f'.repeat(40); }],
  ['modified upstream', r => { r.nativeBuild.upstreamDirty = true; }],
  ['missing native binary identity', r => { delete r.cases[0].artifacts.nativeSha256; }],
  ['incomplete suite', r => { r.cases.pop(); }],
  ['duplicate case', r => { r.cases[1] = r.cases[0]; }],
  ['incomplete pair', r => { delete r.cases[0].pairs[0].native; }],
  ['nonzero process status', r => { r.cases[0].pairs[0].native.exitCode = 1; }],
  ['terminated process', r => { r.cases[0].pairs[0].native.signal = 'SIGTERM'; }],
  ['raw output mismatch', r => { r.cases[0].pairs[0].native.samples[0].ms = 3; }],
  ['nonalternating order', r => { r.cases[0].pairs[1].order = ['native', 'wasm']; }],
  ['duplicate pair', r => { r.cases[0].pairs[1].round = 0; }],
  ['wrong ratio', r => { r.cases[0].summary.nativeOverWasmThroughput.warmMedian = 0.5; }],
  ['instruction mismatch', r => {
    const o = r.cases[0].pairs[0].native;
    o.samples.forEach(s => { s.instructions *= 2; s.minstrPerSec *= 2; }); o.stdout = output(o.samples);
  }],
]) test(`native report rejects ${name}`, () => { const r = report(); change(r); assert.throws(() => validateNativeReport(r)); });

test('CLI validates reports and distinguishes smoke qualification', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-native-validate-'));
  try {
    const file = path.join(dir, 'report.json'), cli = fileURLToPath(new URL('./native_validate.mjs', import.meta.url));
    fs.writeFileSync(file, JSON.stringify(report(1)));
    assert.equal(spawnSync(process.execPath, [cli, file]).status, 0);
    assert.notEqual(spawnSync(process.execPath, [cli, file, '--require-comparison']).status, 0);
    fs.writeFileSync(file, JSON.stringify(report()));
    assert.equal(spawnSync(process.execPath, [cli, file, '--require-comparison']).status, 0);
  } finally { fs.rmSync(dir, {recursive: true, force: true}); }
});
test('collector invocation rejects malformed and duplicate options', () => {
  assert.equal(parseArgs(['wasm/tests', 'native']).runs, 5);
  for (const args of [[], ['wasm'], ['--help', 'native'], ['w', 'n', '--runs'], ['w', 'n', '--runs', '0'],
    ['w', 'n', '--runs', '101'], ['w', 'n', '--runs', '1.5'], ['w', 'n', '--runs', '1', '--runs', '2'],
    ['w', 'n', '--unknown', 'x']]) assert.throws(() => parseArgs(args));
});
