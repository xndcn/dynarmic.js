import {test} from 'node:test';
import assert from 'node:assert/strict';
import {median, parseHostOutput, validateReport, validateSample} from './protocol.mjs';
import {report, samples} from './fixtures.mjs';
const output = (observations = samples()) => observations.map(s => 'BENCHMARK ' + JSON.stringify(s)).join('\n') + '\nPASS\n';
test('median averages the middle pair', () => assert.equal(median([9, 1, 7, 3]), 5));
test('valid samples retain all observations', () => assert.equal(parseHostOutput(output()).length, 9));
test('run indices expose the warmup curve', () => {
  const parsed = parseHostOutput(output());
  assert.deepEqual(parsed.map(s => s.runIndex), [0, 1, 2, 3, 4, 5, 6, 7, 8]);
});
test('wrong guest result is rejected even with timings and PASS', () => assert.throws(() => parseHostOutput(output() + 'WRONG')));
test('missing completion marker is rejected', () => assert.throws(() => parseHostOutput(output().replace('PASS', ''))));
test('production and profile reports validate', () => { validateReport(report()); validateReport(report(true), true); });
for (const [name, change] of [
  ['negative timing', s => { s.ms = -1; }],
  ['inconsistent throughput', s => { s.minstrPerSec *= 2; }],
  ['fractional modules', s => { s.compile.blocks = 0.5; }],
  ['invalid run index', s => { s.runIndex = 9; }],
  ['truncated profile', s => { s.profile = {schema: 'dynarmic.js/profile-v1'}; }],
  ['cold without compilation', s => { s.compile.blocks = 0; }],
]) test(name, () => { const s = samples()[0]; change(s); assert.throws(() => validateSample(s)); });
for (const key of ['blocks', 'bytes', 'translateMs', 'optMs', 'emitMs', 'compileMs', 'instMs', 'tableMs']) {
  test(`warm ${key} invalidates experiment`, () => {
    const s = samples()[1]; s.compile[key] = 1; assert.throws(() => validateSample(s));
  });
}
for (const [name, change] of [
  ['unknown case', r => { r.cases[0].name = 'unknown'; }],
  ['missing architecture', r => { delete r.cases[0].architecture; }],
  ['wrong memory mode', r => { r.cases[0].memoryMode = 'flat-window'; }],
  ['missing artifact', r => { delete r.cases[0].artifact; }],
  ['bad workload hash', r => { r.cases[0].workloadSha256 = ''; }],
  ['missing CPU', r => { delete r.host.cpu; }],
  ['missing build identity', r => { delete r.build; }],
  ['wrong summary', r => { r.cases[0].summary.coldMedianMs = 20; }],
  ['missing process', r => { delete r.cases[0].samples[0].process; }],
  ['out of range process', r => { r.cases[0].samples[0].process = 5; }],
  ['duplicate process samples', r => { r.cases[0].samples[0].process = 1; }],
  ['misordered phases', r => { r.cases[0].samples.reverse(); }],
  ['mixed profile modes', r => { r.cases[0].samples[1] = samples(true)[1]; }],
  ['changed instruction count', r => { r.cases[0].samples[9].instructions *= 2; r.cases[0].samples[9].minstrPerSec *= 2; }],
]) test(name, () => { const r = report(); change(r); assert.throws(() => validateReport(r)); });
test('profile required fails closed', () => assert.throws(() => validateReport(report(), true)));
test('cache peak cannot under-report cold compilation', () => {
  const r = report(true);
  r.cases[0].samples[0].profile.peakCachedBytes = 1;
  assert.throws(() => validateReport(r));
});
test('warm cache peak must be a zero delta', () => {
  const r = report(true);
  r.cases[0].samples[1].profile.peakCachedBlocks = 1;
  assert.throws(() => validateReport(r));
});
