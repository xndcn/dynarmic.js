#!/usr/bin/env node
// One fresh Node process per smoke/diagnostic run; browser measurements use
// compare.mjs, which runs both emulators in the same browser version.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {createRequire} from 'node:module';
const require = createRequire(import.meta.url);
const {runV86} = require('./runtime.js');
const argv = process.argv.slice(2), options = {
  runtime: 'build-v86-quickjs/runtime/package/build', guest: 'build-v86-quickjs/guest',
  dist: 'build-em/examples/browser/dist', case: 'Richards/Richards', timeout: '600000',
};
for (let i = 0; i < argv.length; i += 2) {
  const key = argv[i].replace(/^--/, '');
  assert(['runtime','guest','dist','case','timeout','source','report'].includes(key) && argv[i + 1], 'Invalid option ' + argv[i]);
  options[key] = argv[i + 1];
}
const timeoutMs = Number(options.timeout);
assert(Number.isSafeInteger(timeoutMs) && timeoutMs >= 1000 && timeoutMs <= 3600000);
const read = (directory, filename) => fs.readFileSync(path.join(directory, filename));
const {V86} = await import(pathToFileURL(path.resolve(options.runtime, 'libv86.mjs')));
const elf = read(options.guest, 'quickjs-x86.elf');
const source = options.source ? fs.readFileSync(options.source, 'utf8') :
  `${read(options.dist, 'official-v8.js')}\n${read(options.dist, 'official-v8-adapter.js')}\nJSON.stringify(OfficialV8Bench.run(${JSON.stringify(options.case)}, {}, benchNow));\n`;
const result = await runV86({V86, wasm: read(options.runtime, 'v86.wasm'),
  elf: elf.buffer.slice(elf.byteOffset, elf.byteOffset + elf.byteLength), source, timeoutMs});
const value = JSON.parse(result.text.trim());
if (!options.source) {
  assert.equal(value.caseId, options.case); assert.equal(value.status, 'pass');
  assert.equal(value.mode, 'upstream'); assert.equal(value.warmupMs, 1000);
  assert.equal(value.minimumRuns, 32); assert(value.runs[0] >= 32);
  assert(value.elapsedMs[0] >= 1000); assert(result.stats.compiledSlots > 0, 'v86 JIT must compile code');
}
const report = {schema: 'dynarmic.js/v86-quickjs-node-v1', node: process.version,
  ...result, result: value, verified: true};
if (options.report) fs.writeFileSync(options.report, JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
console.log(JSON.stringify(report, null, 2));
