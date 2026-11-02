// Focused warm execution diagnostic for the frozen vector IR modules.
// Usage: node bench.mjs <module-directory> [iterations]
import { createHash } from 'node:crypto';
import { readFileSync, readdirSync } from 'node:fs';
import { cpus } from 'node:os';
import { join } from 'node:path';
import { performance } from 'node:perf_hooks';

const directory = process.argv[2];
const iterations = Number(process.argv[3] ?? 4000000);
const warmup = 100000;
const samples = [];
for (const filename of readdirSync(directory).sort().filter(x => /^(?:qr?dmul[hl]|shl_[su])/.test(x))) {
    const memory = new WebAssembly.Memory({ initial: 1 });
    const view = new DataView(memory.buffer);
    const bytes = readFileSync(join(directory, filename));
    const { instance } = await WebAssembly.instantiate(bytes, { e: { m: memory } });
    const fn = instance.exports.f;
    view.setBigUint64(0x100 + 96 + 16, 0x71862163fedcb591n, true);
    view.setBigUint64(0x100 + 96 + 24, 0x83232298cafeb0ban, true);
    view.setBigUint64(0x100 + 96 + 32, 0xff017e830184fd02n, true);
    view.setBigUint64(0x100 + 96 + 40, 0x0784f621017f0001n, true);
    for (let n = 0; n < warmup; ++n) fn(0x100, 0x500);
    const begin = performance.now();
    for (let n = 0; n < iterations; ++n) fn(0x100, 0x500);
    const ms = performance.now() - begin;
    samples.push({
        filename,
        ms,
        low: view.getBigUint64(0x100 + 96 + 48, true).toString(16),
        high: view.getBigUint64(0x100 + 96 + 56, true).toString(16),
        fpsr: view.getUint32(0x110, true),
        moduleBytes: bytes.length,
        moduleSHA256: createHash('sha256').update(bytes).digest('hex'),
    });
}
console.log(JSON.stringify({
    node: process.version,
    v8: process.versions.v8,
    cpu: cpus()[0].model,
    iterations,
    warmup,
    samples,
    totalMs: samples.reduce((sum, sample) => sum + sample.ms, 0),
}));
