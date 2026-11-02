// Test Worker protocol and failure handling without requiring browser automation or a Wasm build.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const workerSource = readFileSync(new URL('worker.js', import.meta.url), 'utf8');

function workerHarness({load = async arm => arm, wasm = WebAssembly} = {}) {
  const replies = [];
  const ram = 0x1000;
  const size = 0x20000;
  let runs = 0;
  const arm = {
    HEAPU8: new Uint8Array(0x40000).fill(0xa5),
    _arm_ram: () => ram, _arm_ram_size: () => size,
    _arm_console_clear() {}, _arm_init: () => 0,
    _arm_run: () => { runs++; return 0x01000000; },
    _arm_marks: () => 0, _arm_mark_count: () => 0,
    _arm_console: () => 0, _arm_console_len: () => 0,
  };
  const self = {postMessage: (reply) => replies.push(reply)};
  vm.runInNewContext(workerSource, {
    self, importScripts() {}, createArmJit: () => load(arm),
    performance, ArrayBuffer, Uint8Array, Float64Array, TextDecoder, WebAssembly: wasm,
  });
  return {arm, replies, self, ram, size, runs: () => runs};
}

test('Worker rejects oversized or malformed requests before changing guest memory', async () => {
  const h = workerHarness();
  for (const data of [null, {}, {bin: new Uint8Array(4), mode: 0, arch: 0},
    {bin: new ArrayBuffer(0), mode: 0, arch: 0},
    {bin: new ArrayBuffer(h.size - 0x10000 + 1), mode: 0, arch: 0},
    {bin: new ArrayBuffer(4), mode: -1, arch: 0},
    {bin: new ArrayBuffer(4), mode: 3, arch: 0},
    {bin: new ArrayBuffer(4), mode: '2', arch: 0},
    {bin: new ArrayBuffer(4), mode: NaN, arch: 0},
    {bin: new ArrayBuffer(4), mode: 0, arch: 2},
    {bin: new ArrayBuffer(4), mode: 0, arch: '0'}]) {
    const before = h.arm.HEAPU8.slice();
    await h.self.onmessage({data});
    assert.equal(h.replies.at(-1).ok, false);
    assert.deepEqual(h.arm.HEAPU8, before);
  }
  assert.equal(h.runs(), 0);
});

test('Worker loads a maximum-size binary entirely within guest RAM', async () => {
  const h = workerHarness();
  const bin = new Uint8Array(h.size - 0x10000).fill(0x5a).buffer;
  await h.self.onmessage({data: {bin, mode: 2, arch: 1}});
  assert.equal(h.replies[0].ok, true);
  assert.equal(h.runs(), 1);
  assert.equal(h.arm.HEAPU8[h.ram + 0x10000], 0x5a);
  assert.equal(h.arm.HEAPU8[h.ram + h.size - 1], 0x5a);
  assert.equal(h.arm.HEAPU8[h.ram + h.size], 0xa5);
  assert.equal(h.arm.HEAPU8[h.ram - 1], 0xa5);
});

test('Worker retries a failed module load and reuses a later successful instance', async () => {
  let loads = 0;
  const h = workerHarness({load: async arm => {
    if (++loads === 1) throw new Error('module fetch failed');
    return arm;
  }});
  const data = {bin: new ArrayBuffer(4), mode: 0, arch: 0};
  const before = h.arm.HEAPU8.slice();
  await h.self.onmessage({data});
  assert.equal(h.replies[0].ok, false);
  assert.match(h.replies[0].error, /module fetch failed/);
  assert.equal(h.runs(), 0);
  assert.deepEqual(h.arm.HEAPU8, before);

  await h.self.onmessage({data});
  await h.self.onmessage({data});
  assert.equal(h.replies[1].ok, true);
  assert.equal(h.replies[2].ok, true);
  assert.equal(h.runs(), 2);
  assert.equal(loads, 2);
});

test('Worker rejects concurrent requests during module loading without replacing the running guest', async () => {
  let release, loads = 0;
  const h = workerHarness({load: arm => {
    loads++;
    return new Promise(resolve => { release = () => resolve(arm); });
  }});
  const first = h.self.onmessage({data: {bin: new Uint8Array([0x5a]).buffer, mode: 0, arch: 0}});
  const before = h.arm.HEAPU8.slice();
  await h.self.onmessage({data: {bin: new Uint8Array([0xa5]).buffer, mode: 2, arch: 1}});
  assert.equal(h.replies.length, 1);
  assert.equal(h.replies[0].ok, false);
  assert.equal(h.replies[0].arch, 1);
  assert.match(h.replies[0].error, /busy/);
  assert.deepEqual(h.arm.HEAPU8, before);
  assert.equal(h.runs(), 0);

  release();
  await first;
  assert.equal(h.replies[1].ok, true);
  assert.equal(h.replies[1].arch, 0);
  assert.equal(h.arm.HEAPU8[h.ram + 0x10000], 0x5a);

  await h.self.onmessage({data: {bin: new ArrayBuffer(4), mode: 2, arch: 1}});
  assert.equal(h.replies[2].ok, true);
  assert.equal(h.runs(), 2);
  assert.equal(loads, 1);
});

test('Worker identifies missing Wasm features before loading the module', async () => {
  for (const missing of ['WebAssembly', 'tail calls', 'SIMD']) {
    let probes = 0, loads = 0;
    const wasm = missing === 'WebAssembly' ? undefined : {
      validate(bytes) {
        assert.equal(WebAssembly.validate(bytes), true, 'feature probe is valid in the test runtime');
        return ++probes !== (missing === 'tail calls' ? 1 : 2);
      },
    };
    // null represents WebAssembly being unavailable without triggering the default parameter.
    const h = workerHarness({wasm: wasm ?? null, load: async arm => { loads++; return arm; }});
    await h.self.onmessage({data: {bin: new ArrayBuffer(4), mode: 0, arch: 0}});
    assert.equal(h.replies[0].ok, false);
    assert.match(h.replies[0].error, new RegExp(missing));
    assert.equal(loads, 0);
    assert.equal(h.runs(), 0);
  }
});

test('Worker rejects empty input without loading Wasm and permits a valid retry', async () => {
  let loads = 0;
  const h = workerHarness({load: async arm => { loads++; return arm; }});
  await h.self.onmessage({data: {bin: new ArrayBuffer(0), mode: 0, arch: 0}});
  assert.equal(h.replies[0].ok, false);
  assert.equal(loads, 0);
  await h.self.onmessage({data: {bin: new ArrayBuffer(4), mode: 0, arch: 0}});
  assert.equal(h.replies[1].ok, true);
  assert.equal(loads, 1);
});

test('Worker releases the request lock after initialization errors and guest faults', async () => {
  const h = workerHarness();
  const data = {bin: new ArrayBuffer(4), mode: 0, arch: 0};
  h.arm._arm_init = () => -1;
  await h.self.onmessage({data});
  assert.equal(h.replies[0].ok, false);
  assert.match(h.replies[0].error, /initialization failed/);
  h.arm._arm_init = () => 0;
  const run = h.arm._arm_run;
  h.arm._arm_run = () => 0x02000000;
  await h.self.onmessage({data});
  assert.equal(h.replies[1].ok, false);
  assert.match(h.replies[1].error, /0x02000000/);
  h.arm._arm_run = run;
  await h.self.onmessage({data});
  assert.equal(h.replies[2].ok, true);
});

test('Worker reports a Wasm runtime trap as an ordinary failure reply', async () => {
  const h = workerHarness();
  h.arm._arm_run = () => { throw new WebAssembly.RuntimeError('memory access out of bounds'); };
  await h.self.onmessage({data: {bin: new ArrayBuffer(4), mode: 0, arch: 1}});
  assert.equal(h.replies.length, 1);
  assert.equal(h.replies[0].ok, false);
  assert.equal(h.replies[0].arch, 1);
  assert.match(h.replies[0].error, /RuntimeError: memory access out of bounds/);
});

test('Worker reports compile counters only from the active JIT instance', async () => {
  const h = workerHarness();
  h.arm.dynarmicWasmStats = {
    activeInstance: 2,
    blocks: 100,
    instances: {
      '1': {compile: {blocks: 100}},
      '2': {compile: {blocks: 4, bytes: 256, metadata: {ignored: true}}},
    },
  };
  await h.self.onmessage({data: {bin: new ArrayBuffer(4), mode: 0, arch: 1}});
  assert.equal(h.replies[0].ok, true);
  assert.deepEqual({...h.replies[0].stats}, {blocks: 4, bytes: 256});
});
