import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {browserCases, summarizeBrowser, validateBrowserReport} from './browser_protocol.mjs';

function report() {
  const samples = Array.from({length: 3}, () => ({total: 6, kernels: [1, 2, 3], correct: true}));
  const summary = {totalMinMs: 6, totalMedianMs: 6,
    kernelMinMs: {lcg: 1, sieve: 2, pi: 3}, kernelMedianMs: {lcg: 1, sieve: 2, pi: 3}};
  return {schema: 'dynarmic.js/browser-benchmark-v1', timestamp: '2026-10-22T00:00:00.000Z',
    node: 'v24', v8: 'test', source: {armJitSha256: 'a'.repeat(64), wasmSha256: 'b'.repeat(64),
      demoA32Sha256: 'c'.repeat(64), demoA64Sha256: 'd'.repeat(64)},
    kernels: {lcg: 'integer ALU loop', sieve: 'guest RAM memory loop', pi: 'FP64 floating-point loop'},
    cases: browserCases.flatMap(([name, architecture]) => ['thunk', 'flat-window'].map(memoryMode =>
      ({name, architecture, memoryMode, runs: 3, samples: structuredClone(samples), summary: structuredClone(summary)})))};
}

test('browser benchmark report validates', () => validateBrowserReport(report()));
for (const [name, change] of [
  ['wrong schema', r => { r.schema = 'old'; }],
  ['missing artifact hash', r => { r.source.armJitSha256 = ''; }],
  ['missing guest hash', r => { delete r.source.demoA64Sha256; }],
  ['wrong kernel set', r => { delete r.kernels.pi; }],
  ['incorrect result', r => { r.cases[0].samples[0].correct = false; }],
  ['missing sample', r => { r.cases[0].samples.pop(); }],
  ['negative timing', r => { r.cases[0].samples[0].kernels[0] = -1; }],
  ['missing memory mode', r => { r.cases.pop(); }],
  ['duplicate case', r => { r.cases[1] = r.cases[0]; }],
  ['wrong architecture', r => { r.cases[0].architecture = 'a64'; }],
  ['truthy non-boolean result', r => { r.cases[0].samples[0].correct = 'false'; }],
  ['forged total summary', r => { r.cases[0].summary.totalMedianMs = 1; }],
  ['forged kernel summary', r => { r.cases[0].summary.kernelMedianMs.pi = 1; }],
  ['kernel duration exceeds Run', r => { r.cases[0].samples[0].total = 5; r.cases[0].summary = summarizeBrowser(r.cases[0].samples); }],
]) test(`browser report rejects ${name}`, () => {
  const r = report(); change(r); assert.throws(() => validateBrowserReport(r));
});

// Synthetic module exercises the real CLI's failure/collection path, not JIT performance.
function fixture(t, {wrong = false, marks = [0, 1, 2, 3]} = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'browser-collector-'));
  t.after(() => fs.rmSync(dir, {recursive: true, force: true}));
  fs.mkdirSync(path.join(dir, 'dist'));
  const module = `module.exports = async () => {
    const heap = new Uint8Array(2 ** 21);
    return {HEAPU8: heap, _arm_ram: () => 1000, _arm_ram_size: () => 2 ** 20,
      _arm_console_clear() {}, _arm_init() {}, _arm_console: () => 100, _arm_console_len: () => 2,
      _arm_marks: () => 0, _arm_mark_count: () => ${marks.length},
      _arm_run() { new Float64Array(heap.buffer, 0, ${marks.length}).set(${JSON.stringify(marks)});
        heap.set(Buffer.from(${JSON.stringify(wrong ? 'no' : 'ok')}), 100);
        const until = performance.now() + 4; while (performance.now() < until) {} return 0x01000000; }
    };
  };`;
  fs.writeFileSync(path.join(dir, 'dist/arm_jit.js'), module);
  for (const file of ['arm_jit.wasm', 'demo.bin', 'demo_a64.bin']) fs.writeFileSync(path.join(dir, 'dist', file), 'synthetic');
  fs.writeFileSync(path.join(dir, 'dist/expected.txt'), 'ok');
  const output = path.join(dir, 'report.json');
  const run = (runs = '1', extra = []) => spawnSync(process.execPath,
    [new URL('../../examples/browser/bench.mjs', import.meta.url).pathname, dir, runs, '--json', output, ...extra],
    {encoding: 'utf8', timeout: 10000});
  return {dir, output, run};
}

test('browser collector validates complete report and refuses to overwrite evidence', t => {
  const f = fixture(t), result = f.run();
  assert.equal(result.status, 0, result.stderr);
  const text = fs.readFileSync(f.output, 'utf8');
  validateBrowserReport(JSON.parse(text));
  assert.notEqual(f.run().status, 0);
  assert.equal(fs.readFileSync(f.output, 'utf8'), text);
});
for (const [name, options] of [['wrong guest result', {wrong: true}], ['missing mark', {marks: [0, 1, 2]}],
  ['nonmonotonic marks', {marks: [0, 1, 1, 3]}]]) {
  test(`browser collector fails closed on ${name}`, t => {
    const f = fixture(t, options);
    assert.notEqual(f.run().status, 0);
    assert(!fs.existsSync(f.output));
  });
}
test('browser collector rejects missing guest and invalid CLI inputs', t => {
  const f = fixture(t);
  for (const count of ['0', '-1', '1.5', '101', 'NaN']) assert.notEqual(f.run(count).status, 0);
  assert.notEqual(f.run('1', ['unexpected']).status, 0);
  fs.unlinkSync(path.join(f.dir, 'dist/demo_a64.bin'));
  assert.notEqual(f.run().status, 0);
  assert(!fs.existsSync(f.output));
});
