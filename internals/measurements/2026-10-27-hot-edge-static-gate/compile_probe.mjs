#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {performance} from 'node:perf_hooks';

const inputs = process.argv.slice(2);
assert(inputs.length > 0, 'usage: compile_probe.mjs <module.wasm> [...]');

for (const input of inputs) {
  const bytes = fs.readFileSync(input);
  const name = path.basename(input, '.wasm');
  console.log(`${name} validate=${WebAssembly.validate(bytes)}`);
  const samples = [];
  for (let trial = 0; trial < 20; ++trial) {
    const start = performance.now();
    await WebAssembly.compile(bytes);
    samples.push(performance.now() - start);
  }
  const sorted = [...samples].sort((a, b) => a - b);
  console.log(JSON.stringify({
    name,
    v8CompileMsUpperMedianOf20: sorted[10],
    v8CompileMsAll: samples,
  }));
}
