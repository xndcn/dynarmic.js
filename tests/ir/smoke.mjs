// Node runner for the WASM backend smoke test (module from smoke_ir).
// Instantiates the emitted block module with a shared memory, places an
// A32JitState and ExecutionContext in it, runs the block, checks results.

import { readFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { strict as assert } from 'node:assert';

const bytes = readFileSync(process.argv[2]);

const memory = new WebAssembly.Memory({ initial: 1 });
const u32 = new Uint32Array(memory.buffer);

const STATE = 0x100; // A32JitState lives here
const ECTX = 0x300;  // ExecutionContext { halt_reason u32, check_bit u32, cycles_remaining i64, cycles_to_run i64 }

const { instance } = await WebAssembly.instantiate(bytes, {
    e: { m: memory },
});

const halt = instance.exports.f(STATE, ECTX);

const cpsr_nzcv = u32[(STATE + 0) >> 2];
const r0 = u32[(STATE + 20) >> 2];
const r1 = u32[(STATE + 24) >> 2];
const r2 = u32[(STATE + 28) >> 2];

console.log(`halt=${halt} r0=${r0} r1=${r1} r2=${r2} cpsr_nzcv=0x${cpsr_nzcv.toString(16)}`);

let fail = 0;
const check = (name, got, want) => {
    if (got !== want) {
        console.error(`FAIL ${name}: got ${got}, want ${want}`);
        fail = 1;
    }
};
check('halt', halt, 0);
check('r0', r0, 5);
check('r1', r1, 8);
check('r2', r2, 128);
check('cpsr_nzcv', cpsr_nzcv, 0); // 128 is non-negative, non-zero

if (!fail) console.log('PASS');
if (fail) process.exit(fail);

// A32 cycle-counted region boundaries: two blocks may continue in one call,
// but every slow condition must materialize the next location and cached regs.
{
    const boundaryPath = `${dirname(process.argv[2])}/a32_boundary.wasm`;
    const boundaryMemory = new WebAssembly.Memory({ initial: 2 });
    const { instance: boundary } = await WebAssembly.instantiate(readFileSync(boundaryPath), {
        e: { m: boundaryMemory },
    });
    const dv = new DataView(boundaryMemory.buffer);
    const slots = new Uint8Array(boundaryMemory.buffer, 0x600, 0x10000);
    const STATE = 0x100, ECTX = 0x300, BOUNDARY = 0x500;
    const run = ({ cycles = 2n, halt = 0, flags = 0, hooks = 0, hookPC = null, poisonSlots = false } = {}) => {
        dv.setUint32(STATE + 20, 0, true); // r0
        dv.setUint32(STATE + 24, 0, true); // r1
        dv.setUint32(STATE + 80, 0x1000, true); // r15
        dv.setUint32(STATE + 84, 0, true);
        dv.setUint32(ECTX, halt, true);
        dv.setBigInt64(ECTX + 24, cycles, true);
        dv.setUint32(BOUNDARY, flags, true);
        dv.setUint32(BOUNDARY + 4, hooks, true);
        dv.setUint32(BOUNDARY + 8, poisonSlots ? 0x20000 : 0x600, true);
        slots.fill(0);
        if (hookPC !== null) slots[(hookPC >>> 1) & 0xffff] = 1 << 2;
        const result = boundary.exports.f(STATE, ECTX);
        return {
            result,
            r0: dv.getUint32(STATE + 20, true),
            r1: dv.getUint32(STATE + 24, true),
            pc: dv.getUint32(STATE + 80, true),
            remaining: dv.getBigInt64(ECTX + 24, true),
        };
    };
    assert.deepEqual(run(), { result: 0, r0: 11, r1: 21, pc: 0x1008, remaining: 0n });
    assert.deepEqual(run({cycles: (1n << 40n)}), { result: 0, r0: 11, r1: 21, pc: 0x1008, remaining: (1n << 40n) - 2n });
    assert.deepEqual(run({cycles: -3n}), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: -4n });
    assert.deepEqual(run({ cycles: 1n }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 0n });
    assert.deepEqual(run({ halt: 1 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n });
    assert.deepEqual(run({ flags: 1 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n });
    assert.deepEqual(run({ hooks: 1, hookPC: 0x1004 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n });
    assert.deepEqual(run({ hooks: 1, hookPC: 0x1000 }), { result: 0, r0: 11, r1: 21, pc: 0x1008, remaining: 0n });
    assert.deepEqual(run({ hooks: 1 << 2 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n });
    assert.deepEqual(run({ hooks: 0, hookPC: 0x1004 }), { result: 0, r0: 11, r1: 21, pc: 0x1008, remaining: 0n });
    assert.deepEqual(run({ poisonSlots: true }), { result: 0, r0: 11, r1: 21, pc: 0x1008, remaining: 0n });
    assert.deepEqual(run({ hooks: 2, poisonSlots: true }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n });
    console.log('a32_boundary.wasm OK');
}

// Emit-time hook specialization removes only provably unobservable hook loads.
// The no-hooks case deliberately places hook_slots beyond linear memory.
{
    const directory = dirname(process.argv[2]);
    for (const test of [
        { name: 'specialized', hooks: 0, hookPC: null, poisonSlots: true },
        { name: 'specialized_local', hooks: 1, hookPC: 0x1004 },
        { name: 'specialized_global_code', hooks: 1 << 1, hookPC: null },
        { name: 'specialized_global_block', hooks: 1 << 2, hookPC: null },
    ]) {
        const memory = new WebAssembly.Memory({ initial: 2 });
        const { instance } = await WebAssembly.instantiate(
            readFileSync(`${directory}/a32_boundary_${test.name}.wasm`), { e: { m: memory } });
        const dv = new DataView(memory.buffer);
        const slots = test.poisonSlots ? null : new Uint8Array(memory.buffer, 0x600, 0x10000);
        const STATE = 0x100, ECTX = 0x300, BOUNDARY = 0x500;
        const run = ({ cycles = 2n, halt = 0, flags = 0, hooked = false } = {}) => {
            dv.setUint32(STATE + 20, 0, true);
            dv.setUint32(STATE + 24, 0, true);
            dv.setUint32(STATE + 80, 0x1000, true);
            dv.setUint32(STATE + 84, 0, true);
            dv.setUint32(ECTX, halt, true);
            dv.setBigInt64(ECTX + 24, cycles, true);
            dv.setUint32(BOUNDARY, flags, true);
            dv.setUint32(BOUNDARY + 4, hooked ? test.hooks : 0, true);
            dv.setUint32(BOUNDARY + 8, test.poisonSlots ? 0x20000 : 0x600, true);
            slots?.fill(0);
            if (hooked && test.hookPC !== null) slots[(test.hookPC >>> 1) & 0xffff] = 1 << 1;
            const result = instance.exports.f(STATE, ECTX);
            return {
                result,
                r0: dv.getUint32(STATE + 20, true),
                r1: dv.getUint32(STATE + 24, true),
                pc: dv.getUint32(STATE + 80, true),
                remaining: dv.getBigInt64(ECTX + 24, true),
            };
        };
        const stopped = { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n };
        assert.deepEqual(run(), { result: 0, r0: 11, r1: 21, pc: 0x1008, remaining: 0n });
        if (test.name === 'specialized') {
            assert.deepEqual(run({ cycles: 1n }), { ...stopped, remaining: 0n });
            assert.deepEqual(run({ halt: 1 }), stopped);
            assert.deepEqual(run({ flags: 1 }), stopped);
        } else {
            assert.deepEqual(run({ hooked: true }), stopped, `${test.name} must retain its matching hook guard`);
        }
    }
    console.log('specialized boundary guards OK');
}

// Cross-module static chaining: linked edges tail-call through the shared
// table, but every board-observable condition still exits before the target.
{
    const directory = dirname(process.argv[2]);
    const chainMemory = new WebAssembly.Memory({ initial: 2 });
    const table = new WebAssembly.Table({ initial: 2, element: 'anyfunc' });
    const imports = { e: { m: chainMemory, t: table } };
    const { instance: first } = await WebAssembly.instantiate(readFileSync(`${directory}/a32_chain_first.wasm`), imports);
    const { instance: target } = await WebAssembly.instantiate(readFileSync(`${directory}/a32_chain_target.wasm`), imports);
    table.set(1, target.exports.f);
    const dv = new DataView(chainMemory.buffer);
    const slots = new Uint8Array(chainMemory.buffer, 0x600, 0x10000);
    const STATE = 0x100, ECTX = 0x300, BOUNDARY = 0x500, LINK_SLOT = 0x10700;
    const run = ({ cycles = 2n, linked = true, halt = 0, flags = 0, hooks = 0, hookPC = null } = {}) => {
        dv.setUint32(STATE + 20, 0, true);
        dv.setUint32(STATE + 24, 0, true);
        dv.setUint32(STATE + 80, 0x1000, true);
        dv.setUint32(STATE + 84, 0, true);
        dv.setUint32(ECTX, halt, true);
        dv.setUint32(ECTX + 8, 0, true);
        dv.setBigInt64(ECTX + 24, cycles, true);
        dv.setUint32(BOUNDARY, flags, true);
        dv.setUint32(BOUNDARY + 4, hooks, true);
        dv.setUint32(BOUNDARY + 8, 0x600, true);
        dv.setUint32(LINK_SLOT, linked ? 1 : 0, true);
        slots.fill(0);
        if (hookPC !== null) slots[(hookPC >>> 1) & 0xffff] = 1 << 2;
        const result = first.exports.f(STATE, ECTX);
        return {
            result,
            r0: dv.getUint32(STATE + 20, true),
            r1: dv.getUint32(STATE + 24, true),
            pc: dv.getUint32(STATE + 80, true),
            remaining: dv.getBigInt64(ECTX + 24, true),
            linkSlot: dv.getUint32(ECTX + 8, true),
        };
    };
    assert.deepEqual(run(), { result: 0, r0: 11, r1: 21, pc: 0x1004, remaining: 0n, linkSlot: 0 });
    assert.deepEqual(run({ linked: false }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n, linkSlot: LINK_SLOT });
    assert.deepEqual(run({ cycles: 1n }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 0n, linkSlot: 0 });
    assert.deepEqual(run({ halt: 1 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n, linkSlot: 0 });
    assert.deepEqual(run({ flags: 1 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n, linkSlot: 0 });
    assert.deepEqual(run({ hooks: 1, hookPC: 0x1004 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n, linkSlot: 0 });
    assert.deepEqual(run({ hooks: 1, hookPC: 0x1000 }), { result: 0, r0: 11, r1: 21, pc: 0x1004, remaining: 0n, linkSlot: 0 });
    assert.deepEqual(run({ hooks: 1 << 2 }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n, linkSlot: 0 });
    console.log('a32_chain_first.wasm OK');
}

// Static chaining uses the same specialized guard without reading hook state.
{
    const directory = dirname(process.argv[2]);
    const memory = new WebAssembly.Memory({ initial: 2 });
    const table = new WebAssembly.Table({ initial: 2, element: 'anyfunc' });
    const imports = { e: { m: memory, t: table } };
    const { instance: first } = await WebAssembly.instantiate(
        readFileSync(`${directory}/a32_chain_specialized_first.wasm`), imports);
    const { instance: target } = await WebAssembly.instantiate(
        readFileSync(`${directory}/a32_chain_specialized_target.wasm`), imports);
    table.set(1, target.exports.f);
    const dv = new DataView(memory.buffer);
    const STATE = 0x100, ECTX = 0x300, BOUNDARY = 0x500, LINK_SLOT = 0x10700;
    const run = ({ cycles = 2n, halt = 0, flags = 0 } = {}) => {
        dv.setUint32(STATE + 20, 0, true);
        dv.setUint32(STATE + 24, 0, true);
        dv.setUint32(STATE + 80, 0x1000, true);
        dv.setUint32(STATE + 84, 0, true);
        dv.setUint32(ECTX, halt, true);
        dv.setUint32(ECTX + 8, 0, true);
        dv.setBigInt64(ECTX + 24, cycles, true);
        dv.setUint32(BOUNDARY, flags, true);
        dv.setUint32(BOUNDARY + 4, 0, true);
        dv.setUint32(BOUNDARY + 8, 0x20000, true);
        dv.setUint32(LINK_SLOT, 1, true);
        const result = first.exports.f(STATE, ECTX);
        return {
            result,
            r0: dv.getUint32(STATE + 20, true),
            r1: dv.getUint32(STATE + 24, true),
            pc: dv.getUint32(STATE + 80, true),
            remaining: dv.getBigInt64(ECTX + 24, true),
        };
    };
    assert.deepEqual(run(), { result: 0, r0: 11, r1: 21, pc: 0x1004, remaining: 0n });
    assert.deepEqual(run({ cycles: 1n }), { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 0n });
    console.log('specialized static chaining OK');
}

// Board returns and indirect branches share the guarded dynamic dispatch.
{
    const directory = dirname(process.argv[2]);
    const memory = new WebAssembly.Memory({ initial: 2 });
    const table = new WebAssembly.Table({ initial: 2, element: 'anyfunc' });
    const imports = { e: { m: memory, t: table } };
    const { instance: target } = await WebAssembly.instantiate(
        readFileSync(`${directory}/a32_chain_target.wasm`), imports);
    table.set(1, target.exports.f);
    const STATE = 0x100, ECTX = 0x300, BOUNDARY = 0x500;
    const dv = new DataView(memory.buffer);

    const { instance: rsb } = await WebAssembly.instantiate(
        readFileSync(`${directory}/a32_boundary_rsb.wasm`), imports);
    const rsbOnly = new WebAssembly.Module(readFileSync(`${directory}/a32_boundary_rsb_only.wasm`));
    assert.equal(WebAssembly.Module.imports(rsbOnly).some((entry) => entry.module === 'e' && entry.name === 't'), true);

    const { instance: indirect } = await WebAssembly.instantiate(
        readFileSync(`${directory}/a32_boundary_fast_dispatch.wasm`), imports);
    const slots = new Uint8Array(memory.buffer, 0x600, 0x10000);
    const slot = 0x10800 + ((((0x1004 >>> 2) ^ (0x1004 >>> 14)) & 0xfff) * 16);
    const run = ({ cycles = 2n, halt = 0, flags = 0, hooks = 0, hookPC = null,
                   hookMask = 1 << 1, descriptor = 0x1004n, index = 1, source = indirect } = {}) => {
        dv.setUint32(STATE + 20, 0, true);
        dv.setUint32(STATE + 24, 0, true);
        dv.setUint32(STATE + 80, 0x1000, true);
        dv.setUint32(STATE + 84, 0, true);
        dv.setUint32(ECTX, halt, true);
        dv.setUint32(ECTX + 8, 0, true);
        dv.setUint32(ECTX + 16, 0, true);
        dv.setBigInt64(ECTX + 24, cycles, true);
        dv.setUint32(BOUNDARY, flags, true);
        dv.setUint32(BOUNDARY + 4, hooks, true);
        dv.setUint32(BOUNDARY + 8, 0x600, true);
        slots.fill(0);
        if (hookPC !== null) slots[(hookPC >>> 1) & 0xffff] = hookMask;
        dv.setBigUint64(slot, descriptor, true);
        dv.setUint32(slot + 8, index, true);
        const result = source.exports.f(STATE, ECTX);
        return {
            result,
            r0: dv.getUint32(STATE + 20, true),
            r1: dv.getUint32(STATE + 24, true),
            pc: dv.getUint32(STATE + 80, true),
            remaining: dv.getBigInt64(ECTX + 24, true),
            fastDispatch: dv.getUint32(ECTX + 16, true),
        };
    };
    const hit = { result: 0, r0: 11, r1: 21, pc: 0x1004, remaining: 0n, fastDispatch: 0 };
    const slow = { result: 0, r0: 11, r1: 0, pc: 0x1004, remaining: 1n, fastDispatch: 0 };
    for (const source of [indirect, rsb]) {
        const runSource = (options = {}) => run({ ...options, source });
        assert.deepEqual(runSource(), hit);
        assert.deepEqual(runSource({ cycles: 1n }), { ...slow, remaining: 0n });
        assert.deepEqual(runSource({ cycles: 1n, index: 0 }), { ...slow, remaining: 0n });
        assert.deepEqual(runSource({ halt: 1 }), slow);
        assert.deepEqual(runSource({ flags: 1 }), slow);
        assert.deepEqual(runSource({ hooks: 1 << 1 }), slow);
        assert.deepEqual(runSource({ hooks: 1 << 2 }), slow);
        assert.deepEqual(runSource({ hooks: 1, hookPC: 0x1004 }), slow);
        assert.deepEqual(runSource({ hooks: 1, hookPC: 0x1004, hookMask: 1 << 2 }), slow);
        assert.deepEqual(runSource({ hooks: 1, hookPC: 0x1000 }), hit, 'a source-only hook must not stop the dynamic edge');
        assert.deepEqual(runSource({ index: 0 }), { ...slow, fastDispatch: slot });
        assert.deepEqual(runSource({ descriptor: 0x8000000000001004n }), { ...slow, fastDispatch: slot });
    }

    // With a compile-time empty hook snapshot the dynamic guard must not touch
    // hook_slots; place it one byte beyond linear memory to prove that.
    const specializedMemory = new WebAssembly.Memory({ initial: 2 });
    const specializedTable = new WebAssembly.Table({ initial: 2, element: 'anyfunc' });
    const specializedImports = { e: { m: specializedMemory, t: specializedTable } };
    const { instance: specializedTarget } = await WebAssembly.instantiate(
        readFileSync(`${directory}/a32_chain_target.wasm`), specializedImports);
    specializedTable.set(1, specializedTarget.exports.f);
    const { instance: specialized } = await WebAssembly.instantiate(
        readFileSync(`${directory}/a32_boundary_fast_dispatch_specialized.wasm`), specializedImports);
    const sd = new DataView(specializedMemory.buffer);
    sd.setUint32(STATE + 80, 0x1000, true);
    sd.setBigInt64(ECTX + 24, 2n, true);
    sd.setUint32(BOUNDARY + 8, 0x20000, true);
    sd.setBigUint64(slot, 0x1004n, true);
    sd.setUint32(slot + 8, 1, true);
    assert.equal(specialized.exports.f(STATE, ECTX), 0);
    assert.equal(sd.getUint32(STATE + 20, true), 11);
    assert.equal(sd.getUint32(STATE + 24, true), 21);
    assert.equal(sd.getUint32(ECTX + 16, true), 0);
    console.log('boundary indirect guards OK');
}

// A64 fallback multi-entry: the native test emits this sibling module from an
// irreducible region with one genuine return site and one ordinary block.
{
    const fallbackPath = `${dirname(process.argv[2])}/a64_fallback.wasm`;
    const fallbackBytes = readFileSync(fallbackPath);
    const fallbackMemory = new WebAssembly.Memory({ initial: 1 });
    const { instance: fallback } = await WebAssembly.instantiate(fallbackBytes, {
        e: { m: fallbackMemory },
    });
    assert.deepEqual(Object.keys(fallback.exports), ['f']);
    assert.equal(fallback.exports.f.length, 2);

    const bytes = new Uint8Array(fallbackMemory.buffer);
    const dv = new DataView(fallbackMemory.buffer);
    const A64_STATE = 0x100;
    const A64_ECTX = 0x400;
    const PC = A64_STATE + 16;
    const X0 = A64_STATE + 32;
    const X1 = A64_STATE + 40;
    const X2 = A64_STATE + 48;
    const SENTINEL = 0xa5a5a5a5a5a5a5a5n;

    const run = (pc) => {
        bytes.fill(0xa5);
        dv.setUint32(A64_STATE, 1 << 30, true); // Z=1: every test path returns
        dv.setBigUint64(PC, pc, true);
        dv.setUint32(A64_ECTX + 8, 0x12345678, true);  // link_slot_addr
        dv.setUint32(A64_ECTX + 12, 3, true);         // rsb_top
        dv.setUint32(A64_ECTX + 16, 0x23456789, true); // fast_dispatch_entry
        const halt = fallback.exports.f(A64_STATE, A64_ECTX);
        return {
            halt,
            x0: dv.getBigUint64(X0, true),
            x1: dv.getBigUint64(X1, true),
            x2: dv.getBigUint64(X2, true),
            linkSlot: dv.getUint32(A64_ECTX + 8, true),
            rsbTop: dv.getUint32(A64_ECTX + 12, true),
            fastDispatch: dv.getUint32(A64_ECTX + 16, true),
        };
    };

    // High state-PC bits are ignored for the original entry. Entry executes
    // block 0 and then block 1; direct return-site entry must skip block 0.
    const original = run(0xab00000000001000n);
    assert.deepEqual(original, {
        halt: 0, x0: 11n, x1: 21n, x2: SENTINEL,
        linkSlot: 0x12345678, rsbTop: 3, fastDispatch: 0x23456789,
    });

    // The candidate has descriptor bit 55 set (so PC() sign-extends) and the
    // incoming state has additional high bits set. Matching must use raw PCs
    // under the 56-bit mask, not sign-extended LocationDescriptor::PC().
    const returnSite = run(0xab80000000001004n);
    assert.deepEqual(returnSite, {
        halt: 0, x0: SENTINEL, x1: 21n, x2: SENTINEL,
        linkSlot: 0x12345678, rsbTop: 3, fastDispatch: 0x23456789,
    });

    // Fail closed before register-cache setup or any block/RSB/link activity:
    // every byte, not just the architectural state, must remain unchanged.
    // The ordinary selected block is not a candidate either.
    const failClosed = (pc) => {
        bytes.fill(0xa5);
        dv.setUint32(A64_STATE, 1 << 30, true);
        dv.setBigUint64(PC, pc, true);
        const before = bytes.slice();
        assert.equal(fallback.exports.f(A64_STATE, A64_ECTX), 0);
        assert.deepEqual(bytes, before);
    };
    failClosed(0x1234000000002000n);
    failClosed(0xab00000000001008n);
    console.log('a64_fallback.wasm OK');
}

for (let count=0; count<=3; count++) {
  const module = new WebAssembly.Module(readFileSync(`${dirname(process.argv[2])}/host_call_${count}.wasm`));
  const memory=new WebAssembly.Memory({initial:1});
  const targetBytes=[0,97,115,109,1,0,0,0,1,4+count,1,96,count,...Array(count).fill(126),0,3,2,1,0,7,5,1,1,102,0,0,10,4,1,2,0,11];
  const target=new WebAssembly.Instance(new WebAssembly.Module(Uint8Array.from(targetBytes))).exports.f;
  const table=new WebAssembly.Table({initial:2,element:'anyfunc'});table.set(1,target);
  const instance=new WebAssembly.Instance(module,{e:{m:memory,t:table}});
  assert.equal(instance.exports.f(0,256),0);
}
console.log('host calls 0..3 arguments OK');

// Independent byte-addressed reference: page 1 and page 2 are deliberately
// noncontiguous. Every crossing, including a missing second page, must invoke
// the callback; unaligned accesses wholly inside a page must not invoke it.
for (const width of [1, 2, 4, 8]) for (const absolute of [false, true]) {
    const memory = new WebAssembly.Memory({initial: 2});
    const heap = new Uint8Array(memory.buffer), dv = new DataView(memory.buffer);
    const pages = new Map([[1, 0x4000], [2, 0x8000], [3, 0xa000], [4, 0xc000]]);
    for (const [page, pointer] of pages) dv.setUint32(0x1000 + page * 4, absolute ? pointer - page * 4096 : pointer, true);
    let reads = 0, writes = 0;
    const byte = address => {
        const pointer = pages.get(address >>> 12);
        if (pointer === undefined) throw Error('unmapped reference page');
        return pointer + (address & 4095);
    };
    const read = (_state, _ctx, address) => {
        reads++;
        let value = 0n;
        for (let i=0;i<width;i++) value |= BigInt(heap[byte((address+i)>>>0)]) << BigInt(i*8);
        return width === 8 ? value : Number(value);
    };
    const write = (_state, _ctx, address, value) => {
        writes++;
        for (let i=0;i<width;i++) heap[byte((address+i)>>>0)] = Number((BigInt(value>>>0) >> BigInt(i*8)) & 255n);
    };
    const write64 = (_state, _ctx, address, value) => {
        writes++;
        for (let i=0;i<width;i++) heap[byte((address+i)>>>0)] = Number((value >> BigInt(i*8)) & 255n);
    };
    const {instance} = await WebAssembly.instantiate(readFileSync(`${dirname(process.argv[2])}/a32_page_${width}${absolute?'_absolute':''}.wasm`),
        {e:{m:memory,[`read${width*8}`]:read,[`write${width*8}`]:width===8?write64:write}});
    for (const offset of [0, 1, 3, 4096-width, 4097-width, 4095]) {
        const source=0x1000+offset, destination=0x3000+offset;
        const expected=Array.from({length:width},(_,i)=>(i*43+129)&255);
        expected.forEach((value,i)=>heap[byte(source+i)]=value);
        for(let i=0;i<width;i++) heap[byte(destination+i)]=0;
        dv.setUint32(20,source,true);dv.setUint32(24,destination,true);
        reads=writes=0;
        assert.equal(instance.exports.f(0,256),0);
        assert.deepEqual(Array.from({length:width},(_,i)=>heap[byte(destination+i)]),expected);
        assert.equal(reads,(offset&4095)+width>4096?1:0);
        assert.equal(writes,(offset&4095)+width>4096?1:0);
    }
    // A protected/MMIO first page (null fast entry) still takes the callback.
    dv.setUint32(0x1004,0,true);dv.setUint32(0x100c,0,true);
    dv.setUint32(20,0x1001,true);dv.setUint32(24,0x3001,true);
    reads=writes=0; instance.exports.f(0,256);
    assert.equal(reads,1);assert.equal(writes,1);
    if(width>1) {
        dv.setUint32(0x1004,absolute?0x3000:0x4000,true);
        pages.delete(2);
        dv.setUint32(20,0x1fff,true);
        assert.throws(()=>instance.exports.f(0,256),/unmapped reference page/);
    }
}
console.log('A32 page-contained unaligned loads/stores OK');
