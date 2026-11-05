// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';

const directory = mkdtempSync(join(tmpdir(), 'dynarmic-a32-memory-'));
let checks = 0;
try {
    execFileSync(resolve(process.argv[2]), [directory]);
    for (const width of [1, 2, 4, 8]) for (let flags = 0; flags < 4; ++flags) for (let crossing = 0; crossing < 4; ++crossing) {
        const absolute = Boolean(flags & 1), tags = flags & 2 ? 3 : 0;
        const source = crossing & 1 ? 0x1fff : 0x1001;
        const destination = crossing & 2 ? 0x3fff : 0x3001;
        const memory = new WebAssembly.Memory({initial: 2});
        const heap = new Uint8Array(memory.buffer), view = new DataView(memory.buffer);
        const pages = new Map([[1, 0x4000], [2, 0x8000], [3, 0xa000], [4, 0xc000]]);
        const update = page => view.setUint32(0x2000 + page * 4,
            (absolute ? pages.get(page) - page * 4096 : pages.get(page)) | tags, true);
        for (const page of pages.keys()) update(page);
        const byte = address => {
            const pointer = pages.get(address >>> 12);
            if (pointer === undefined) throw Error('unmapped reference page');
            return pointer + (address & 4095);
        };
        let reads = [], writes = [], protectedWrite = false;
        const read = (_state, _context, address) => {
            reads.push(address);
            let value = 0n;
            for (let i = 0; i < width; ++i) value |= BigInt(heap[byte(address + i)]) << BigInt(i * 8);
            return width === 8 ? value : Number(value);
        };
        const write = (_state, _context, address, value) => {
            writes.push(address);
            if (protectedWrite) throw Error('write protected');
            for (let i = 0; i < width; ++i) heap[byte(address + i)] = Number(BigInt(value) >> BigInt(i * 8) & 255n);
        };
        const module = new WebAssembly.Module(readFileSync(join(directory, `memory_${width}_${flags}_${crossing}.wasm`)));
        const instance = new WebAssembly.Instance(module, {e: {
            m: memory, [`read${width * 8}`]: read, [`write${width * 8}`]: write,
        }});
        const run = (forceSlow = false) => {
            const expected = Array.from({length: width}, (_, i) => (i * 43 + checks + 129) & 255);
            heap.fill(0xcd, 0x5000, 0x6000); heap.fill(0xcd, 0xb000, 0xc000);
            expected.forEach((value, i) => { heap[byte(source + i)] = value; heap[byte(destination + i)] = 0; });
            reads = []; writes = [];
            assert.equal(instance.exports.f(0, 0x1000), 0);
            assert.deepEqual(Array.from({length: width}, (_, i) => heap[byte(destination + i)]), expected);
            assert.deepEqual(reads, forceSlow || (source & 4095) + width > 4096 ? [source] : []);
            assert.deepEqual(writes, forceSlow || (destination & 4095) + width > 4096 ? [destination] : []);
            assert(heap.subarray(0xb000, 0xc000).every(value => value === 0xcd));
            ++checks;
        };
        run();
        // Reuse the same compiled function after changing both live mappings.
        pages.set(1, 0x6000); pages.set(3, 0xe000); update(1); update(3);
        run();
        // Tagged null entries route MMIO/observers/protection through callbacks.
        view.setUint32(0x2004, tags, true); view.setUint32(0x200c, tags, true);
        run(true);
        protectedWrite = true;
        assert.throws(() => instance.exports.f(0, 0x1000), /write protected/);
        protectedWrite = false;
        if (width > 1 && crossing & 1) {
            pages.delete(2);
            assert.throws(() => instance.exports.f(0, 0x1000), /unmapped reference page/);
        } else if (width > 1 && crossing & 2) {
            pages.delete(4);
            assert.throws(() => instance.exports.f(0, 0x1000), /unmapped reference page/);
        }
    }
    console.log(`PASS A32 constant memory: ${checks} byte-reference cases, remapping, observers and faults`);
} finally {
    rmSync(directory, {recursive: true, force: true});
}
