// Validates wasm_binary_test output against Node's WebAssembly engine (V8).
// usage: node wasm_binary_test.mjs <dir with basic/imports/numeric.wasm>
import { readFileSync } from 'node:fs';
import { strict as assert } from 'node:assert';

const dir = process.argv[2] ?? '.';

function load(name, imports = {}) {
  const bytes = readFileSync(`${dir}/${name}`);
  return new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);
}

// basic.wasm
{
  const inst = load('basic.wasm');
  const { add, store_load, mem } = inst.exports;
  assert.equal(add(3, 4), 7);
  assert.equal(add(-1, 1), 0);
  assert.equal(store_load(0xDEADBEEF | 0), 0xDEADBEEF | 0);
  assert.equal(new Uint32Array(mem.buffer)[0], 0xDEADBEEF);
  console.log('basic.wasm OK');
}

// imports.wasm: imported memory/table/host func shared with the instance
{
  const memory = new WebAssembly.Memory({ initial: 1 });
  const table = new WebAssembly.Table({ initial: 1, maximum: 16, element: 'anyfunc' });
  const host_double = (x) => x * 2;
  const inst = load('imports.wasm', { env: { memory, table, host_double } });
  new Uint32Array(memory.buffer)[0] = 5;
  assert.equal(inst.exports.use_host(10), 25); // 10*2 + 5
  assert.equal(inst.exports.dispatch(21), 42); // mul2 via call_indirect
  console.log('imports.wasm OK');
}

// numeric.wasm
{
  const inst = load('numeric.wasm');
  // -1 + -64 + 64 + (0x7FFFFFFF + INT32_MIN) + 1 + 1  == -1 + -64 + 64 - 1 + 1 + 1 = 0
  assert.equal(inst.exports.run(), 0);
  console.log('numeric.wasm OK');
}

console.log('ALL PASS');
