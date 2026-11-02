import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import http from 'node:http';
import {once} from 'node:events';
import {createEmulator} from 'dynarmic.js';
import {exercise} from './cases.mjs';

const checks = await exercise(createEmulator);
const wasmURL = new URL(import.meta.resolve('dynarmic.js/runtime.wasm'));
const wasmBinary = await fs.readFile(wasmURL);
async function checkInitialization(options) {
  const cpu = await createEmulator({...options, memorySize: 65536});
  try {
    cpu.writeMemory(0x1000, Uint8Array.of(42, 0, 0xa0, 0xe3, 0, 0, 0, 0xef));
    cpu.setPC(0x1000);
    assert.equal(cpu.run().svc, 0);
    assert.equal(cpu.getRegister(0), 42);
  }
  finally { cpu.dispose(); }
}
const padded = new Uint8Array(wasmBinary.byteLength + 16);
padded.set(wasmBinary, 8);
for (const options of [
  {wasmURL}, {wasmURL: wasmURL.href}, {wasmBinary},
  {wasmBinary: Uint8Array.from(wasmBinary).buffer},
  {wasmBinary: padded.subarray(8, padded.byteLength - 8)},
]) await checkInitialization(options);
checks.push('Node file URL and supplied Buffer, ArrayBuffer and offset Uint8Array');

const server = http.createServer((request, response) => {
  if (request.url !== '/assets/runtime.wasm') { response.writeHead(404).end('Not found'); return; }
  response.writeHead(200, {'content-type': 'application/wasm'}).end(wasmBinary);
});
try {
  const listening = once(server, 'listening');
  server.listen(0, '127.0.0.1');
  await listening;
  const origin = `http://127.0.0.1:${server.address().port}`;
  await checkInitialization({wasmURL: `${origin}/assets/runtime.wasm`});
  await assert.rejects(createEmulator({wasmURL: new URL(`${origin}/missing.wasm`)}), /HTTP 404/);
  await checkInitialization({wasmURL: new URL(`${origin}/assets/runtime.wasm`)});
} finally {
  await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
}
checks.push('Custom HTTP asset URL, 404 rejection and successful retry');

await assert.rejects(createEmulator({wasmBinary: Uint8Array.of(0, 1, 2)}));
await checkInitialization({});
checks.push('Invalid Wasm initialization rejects without breaking subsequent instances');
for (const options of [
  null, [], {architecture: 'x86'}, {architecture: null}, {memorySize: 1}, {memorySize: -1},
  {memorySize: null}, {memorySize: 65537}, {memorySize: 2 ** 32},
  {wasmURL: './relative.wasm'}, {wasmURL: 'ftp://example.test/runtime.wasm'},
  {wasmURL, wasmBinary}, {wasmBinary: new DataView(new ArrayBuffer(4))},
])
  await assert.rejects(createEmulator(options));
checks.push('Invalid architecture, memory, URL and mutually exclusive loader options reject');
const large = await createEmulator({memorySize: 96 * 1024 * 1024});
try {
  large.writeMemory(large.memorySize - 1, Uint8Array.of(42));
  assert.equal(large.readMemory(large.memorySize - 1, 1)[0], 42);
  large.writeMemory(0x1000, Uint8Array.of(42, 0, 0xa0, 0xe3, 0, 0, 0, 0xef));
  large.setPC(0x1000); assert.equal(large.run().svc, 0); assert.equal(large.getRegister(0), 42);
} finally { large.dispose(); }
checks.push('RAM allocation grows Wasm memory without stale host views');
console.log(JSON.stringify({status: 'pass', environment: process.version, checks}, null, 2));
