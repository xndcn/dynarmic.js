// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';

const directory = mkdtempSync(join(tmpdir(), 'dynarmic-memory-edges-'));
let checks = 0;
try {
    const offsets = JSON.parse(execFileSync(resolve(process.argv[2]), [directory], {encoding: 'utf8'}));
    for (const width of [1, 2, 4, 8, 16]) for (let flags = 0; flags < 8; ++flags) {
        const absolute = Boolean(flags & 1), mirrored = Boolean(flags & 2), tags = flags & 4 ? 3 : 0;
        const memory = new WebAssembly.Memory({initial: 2});
        const heap = new Uint8Array(memory.buffer), view = new DataView(memory.buffer);
        const pages = new Map([[1, 0x4000], [2, 0x8000], [3, 0xa000], [4, 0xc000]]);
        const entry = page => (absolute ? pages.get(page) - page * 4096 : pages.get(page)) | tags;
        for (const page of pages.keys()) view.setUint32(0x2000 + page * 4, entry(page), true);
        // The reference callback maps high virtual addresses too. The emitter
        // may bypass it at those addresses only with table mirroring enabled.
        const byte = address => {
            const low = Number(address & 0xffffn);
            const pointer = pages.get(low >>> 12);
            if (pointer === undefined) throw Error('unmapped reference page');
            return pointer + (low & 4095);
        };
        let reads = [], writes = [];
        const read = (state, _context, address) => {
            address = BigInt.asUintN(64, address);
            reads.push(address);
            const bytes = Array.from({length: width}, (_, i) => heap[byte(address + BigInt(i))]);
            if (width === 16) {
                heap.set(bytes, state + offsets.scratch);
                return;
            }
            const value = bytes.reduce((result, value, i) => result | BigInt(value) << BigInt(8 * i), 0n);
            return width === 8 ? value : Number(value);
        };
        const write = (state, _context, address, value) => {
            address = BigInt.asUintN(64, address);
            writes.push(address);
            const bytes = width === 16 ? heap.slice(state + offsets.scratch, state + offsets.scratch + 16)
                : Array.from({length: width}, (_, i) => Number((BigInt(value) >> BigInt(i * 8)) & 255n));
            bytes.forEach((value, i) => { heap[byte(address + BigInt(i))] = value; });
        };
        const module = new WebAssembly.Module(readFileSync(join(directory, `memory_${width}_${flags}.wasm`)));
        const instance = new WebAssembly.Instance(module, {e: {
            m: memory, [`a64_read${width * 8}`]: read, [`a64_write${width * 8}`]: write,
        }});
        const addresses = [0, 1, 3, 4096 - width, 4097 - width, 4095];
        for (const high of [0n, 0x100000000n]) for (const sourceOffset of addresses) for (const destinationOffset of addresses) {
            const source = high + 0x1000n + BigInt(sourceOffset);
            const destination = high + 0x3000n + BigInt(destinationOffset);
            const expected = Array.from({length: width}, (_, i) => (i * 43 + 129) & 255);
            // Adjacent host pages are poison: a guest crossing must never use them.
            heap.fill(0xcd, 0x5000, 0x6000); heap.fill(0xcd, 0xb000, 0xc000);
            expected.forEach((value, i) => { heap[byte(source + BigInt(i))] = value; });
            for (let i = 0; i < width; ++i) heap[byte(destination + BigInt(i))] = 0;
            view.setBigUint64(offsets.reg, source, true);
            view.setBigUint64(offsets.reg + 8, destination, true);
            reads = []; writes = [];
            const label = `width=${width} flags=${flags} source=${source.toString(16)} destination=${destination.toString(16)}`;
            assert.equal(instance.exports.f(0, 0x1000), 0, label);
            assert.deepEqual(Array.from({length: width}, (_, i) => heap[byte(destination + BigInt(i))]), expected, label);
            const slow = offset => (!mirrored && high !== 0n) || (offset & 4095) + width > 4096;
            assert.deepEqual(reads, slow(sourceOffset) ? [source] : [], label);
            assert.deepEqual(writes, slow(destinationOffset) ? [destination] : [], label);
            assert(heap.subarray(0xb000, 0xc000).every(value => value === 0xcd), label);
            ++checks;
        }
        // A tagged null mapping must still call the callback.
        view.setUint32(0x2004, tags, true); view.setUint32(0x200c, tags, true);
        view.setBigUint64(offsets.reg, 0x1001n, true); view.setBigUint64(offsets.reg + 8, 0x3001n, true);
        reads = []; writes = [];
        instance.exports.f(0, 0x1000);
        assert.deepEqual(reads, [0x1001n]); assert.deepEqual(writes, [0x3001n]);
        if (width > 1) {
            view.setUint32(0x2004, entry(1), true); view.setUint32(0x200c, entry(3), true);
            pages.delete(2);
            view.setUint32(0x2008, 0, true);
            view.setBigUint64(offsets.reg, 0x1fffn, true);
            reads = []; writes = [];
            assert.throws(() => instance.exports.f(0, 0x1000), /unmapped reference page/);
            assert.deepEqual(reads, [0x1fffn]); assert.deepEqual(writes, []);
            pages.delete(4);
            view.setUint32(0x2010, 0, true);
            view.setBigUint64(offsets.reg, 0x1001n, true); view.setBigUint64(offsets.reg + 8, 0x3fffn, true);
            reads = []; writes = [];
            assert.throws(() => instance.exports.f(0, 0x1000), /unmapped reference page/);
            assert.deepEqual(reads, []); assert.deepEqual(writes, [0x3fffn]);
        }
    }
    console.log(`PASS A64 memory edges: ${checks} byte-reference cases plus null mappings and cross-page faults`);
} finally {
    rmSync(directory, {recursive: true, force: true});
}
