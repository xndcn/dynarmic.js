// Dispatcher runner for block_dump output: loads one emitted block module per
// guest PC and runs the classic dynarmic dispatch loop in JS (no host thunks,
// so only blocks without memory/SVC callbacks can run).
//
// usage: node dispatch.mjs <pc_hex>=<file.wasm>... <entry_pc_hex> <lr_hex>
//                          [--expect name=value,...]
//
// Expectation names: halt, steps, r0..r15 (decimal or 0x hex), n, z, c, v.
// Exits non-zero if any expectation fails.

import { readFileSync } from 'node:fs';

const argv = process.argv.slice(2);
const expectAt = argv.indexOf('--expect');
const expect = expectAt >= 0 ? argv.splice(expectAt, 2)[1].split(',') : [];
const entry = parseInt(argv[argv.length - 2], 16);
const lr = parseInt(argv[argv.length - 1], 16);
const specs = argv.slice(0, -2);

const memory = new WebAssembly.Memory({ initial: 1 });
const u32 = new Uint32Array(memory.buffer);
// Blocks with RSB / link terminals import the function table. It stays empty:
// every chain or RSB lookup misses and the block returns here instead.
const table = new WebAssembly.Table({ initial: 1, element: 'anyfunc' });

const STATE = 0x100; // A32JitState
const ECTX = 0x300;  // ExecutionContext
const reg = (i) => u32[(STATE + 20 + i * 4) >> 2];

const blocks = new Map();
for (const spec of specs) {
    const [pcHex, file] = spec.split('=');
    const { instance } = await WebAssembly.instantiate(readFileSync(file), { e: { m: memory, t: table } });
    blocks.set(parseInt(pcHex, 16), instance.exports.f);
}

// r14 = lr, r15 = entry, cpsr_nzcv = 0
u32[(STATE + 20 + 14 * 4) >> 2] = lr;
u32[(STATE + 20 + 15 * 4) >> 2] = entry;

let steps = 0;
let halt = 0;
while (halt === 0 && steps < 1000) {
    const fn = blocks.get(reg(15));
    if (!fn) break;
    halt = fn(STATE, ECTX);
    steps++;
}

const nzcv = u32[STATE >> 2];
const regs = Array.from({ length: 16 }, (_, i) => `r${i}=0x${reg(i).toString(16)}`).join(' ');
console.log(`steps=${steps} halt=${halt} nzcv=0x${(nzcv >>> 28).toString(16)}\n${regs}`);

const actual = { halt, steps, n: (nzcv >>> 31) & 1, z: (nzcv >>> 30) & 1, c: (nzcv >>> 29) & 1, v: (nzcv >>> 28) & 1 };
for (let i = 0; i < 16; i++) actual[`r${i}`] = reg(i);

let fail = 0;
for (const e of expect) {
    const [name, value] = e.split('=');
    const want = Number(value.startsWith('0x') ? parseInt(value, 16) : value) >>> 0;
    if (!(name in actual)) {
        console.error(`unknown expectation ${name}`);
        fail = 1;
    } else if (actual[name] >>> 0 !== want) {
        console.error(`FAIL ${name}: got ${actual[name]}, want ${want}`);
        fail = 1;
    }
}
if (expect.length && !fail) console.log('PASS');
process.exit(fail);
