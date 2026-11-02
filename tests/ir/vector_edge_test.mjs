// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
// Execute emitted SIMD with arbitrary-precision arithmetic as the oracle.
// Usage: node vector_edge_test.mjs /path/to/vector_edge_test
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const directory = mkdtempSync(join(tmpdir(), 'dynarmic-vector-edge-'));
const STATE = 0x100, ECTX = 0x500, QC = 1 << 27;
let cases = 0, failures = 0, moduleBytes = 0;
let seed = 0x7a63d291;
const random32 = () => {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    return BigInt(seed >>> 0);
};
const signed = (w, x) => BigInt.asIntN(w, x);
const unsigned = (w, x) => BigInt.asUintN(w, x);
function clamp(x, width, sign) {
    const hi = (1n << BigInt(width - (sign ? 1 : 0))) - 1n;
    const lo = sign ? -(1n << BigInt(width - 1)) : 0n;
    const value = x < lo ? lo : x > hi ? hi : x;
    return [unsigned(width, value), value !== x];
}
function reference(op, width, a, b) {
    if (op === 'eq') return [a === b ? (1n << 128n) - 1n : 0n, false];
    if (op === 'sext') return [unsigned(128, signed(64, a)), false];
    if (/^[su]q(add|sub)$/.test(op)) {
        const sign = op[0] === 's';
        const x = sign ? signed(width, a) : a;
        const y = sign ? signed(width, b) : b;
        return clamp(op.endsWith('sub') ? x - y : x + y, width, sign);
    }
    if (op === 'sqabs' || op === 'sqneg') {
        const x = signed(width, a);
        return clamp(op === 'sqabs' ? (x < 0n ? -x : x) : -x, width, true);
    }
    const sign = op.endsWith('_s');
    const x = sign ? signed(width, a) : a;
    if (op.includes('shl')) {
        const amount = signed(8, b);
        if (amount < 0n) {
            const rounding = op.startsWith('r');
            const bias = rounding ? 1n << (-amount - 1n) : 0n;
            return [unsigned(width, (x + bias) >> -amount), false];
        }
        const shifted = x << amount;
        return op.startsWith('q') ? clamp(shifted, width, sign) : [unsigned(width, shifted), false];
    }
    if (op === 'suqadd') return clamp(a + signed(width, b), width, true);
    if (op === 'usqadd') return clamp(signed(width, a) + b, width, false);
    let product = 2n * signed(width, a) * signed(width, b);
    if (op === 'qdmull') return clamp(product, 2 * width, true);
    if (op === 'qrdmulh') product += 1n << BigInt(width - 1);
    return clamp(product >> BigInt(width), width, true);
}

try {
    const layout = JSON.parse(execFileSync(process.argv[2], [directory], { encoding: 'utf8' }));
    const expectedModules = ['eq128.wasm', 'sext64.wasm'];
    for (const width of [8, 16, 32, 64]) {
        for (const operation of ['sqadd', 'sqsub', 'uqadd', 'uqsub', 'sqabs', 'sqneg',
            'shl_u', 'shl_s', 'rshl_u', 'rshl_s', 'qshl_u', 'qshl_s', 'suqadd', 'usqadd']) {
            expectedModules.push(`${operation}${width}.wasm`);
        }
        if (width === 16 || width === 32) {
            for (const operation of ['qdmull', 'qdmulh', 'qrdmulh']) expectedModules.push(`${operation}${width}.wasm`);
        }
    }
    const filenames = readdirSync(directory).sort();
    assert.deepEqual(filenames, expectedModules.sort(), 'generator must emit the complete vector test matrix');
    for (const filename of filenames) {
        const [, op, widthString] = /^(.*?)(\d+)\.wasm$/.exec(filename);
        const width = Number(widthString), lanes = 128 / width;
        const bytes = readFileSync(join(directory, filename));
        moduleBytes += bytes.length;
        if (!WebAssembly.validate(bytes)) {
            ++failures;
            console.error(`${filename}: generated module fails WebAssembly validation`);
            continue;
        }
        const memory = new WebAssembly.Memory({ initial: 1 });
        const { instance } = await WebAssembly.instantiate(bytes, { e: { m: memory } });
        const view = new DataView(memory.buffer);
        const ram = new Uint8Array(memory.buffer);
        const setLane = (offset, bits, value) => {
            for (let byte = 0; byte < bits / 8; ++byte) ram[offset + byte] = Number((value >> BigInt(8 * byte)) & 255n);
        };
        const getLane = (offset, bits) => {
            let value = 0n;
            for (let byte = 0; byte < bits / 8; ++byte) value |= BigInt(ram[offset + byte]) << BigInt(8 * byte);
            return value;
        };
        const run = (a, b, initial = 0x91) => {
            for (let lane = 0; lane < lanes; ++lane) {
                setLane(STATE + layout.ext + 16 + lane * width / 8, width, a[lane]);
                setLane(STATE + layout.ext + 32 + lane * width / 8, width, b[lane]);
            }
            view.setUint32(STATE + layout.fpsr, initial, true);
            assert.equal(instance.exports.f(STATE, ECTX), 0);
            const outWidth = op === 'qdmull' || op === 'sext' ? 2 * width : width;
            let expectedFlags = initial;
            const expected = [], actual = [];
            for (let lane = 0; lane < 128 / outWidth; ++lane) {
                const [value, saturated] = reference(op, width, a[lane], b[lane]);
                expected.push(value);
                actual.push(getLane(STATE + layout.ext + 48 + lane * outWidth / 8, outWidth));
                if (saturated) expectedFlags |= QC;
            }
            const actualFlags = view.getUint32(STATE + layout.fpsr, true);
            ++cases;
            if (actual.some((x, i) => x !== expected[i]) || actualFlags !== expectedFlags) {
                if (++failures <= 8) console.error({ filename, a, b, expected, actual, expectedFlags, actualFlags });
            }
        };
        const high = 1n << BigInt(width - 1), max = 2n * high - 1n;
        const values = [0n, 1n, 2n, 3n, high - 2n, high - 1n, high, high + 1n, max - 1n, max];
        if (op.includes('shl')) {
            // Every signed low-byte shift amount; unrelated upper bytes must
            // not change shift semantics. Every lane sees distinct inputs.
            for (let amount = 0; amount < 256; ++amount) {
                for (let i = 0; i < values.length; ++i) {
                    const a = Array.from({ length: lanes }, (_, lane) => values[(i + lane) % values.length]);
                    const b = Array.from({ length: lanes }, (_, lane) => unsigned(width, (random32() << 8n) | BigInt((amount + lane) & 255)));
                    run(a, b, amount & 1 ? QC | 0x91 : 0x91);
                }
            }
        } else {
            for (const a of values) for (const b of values) {
                // Isolate each lane's saturation and each operation's QC;
                // another saturating operation must never mask a missing
                // update. Unused upper inputs of widening multiply must not
                // set QC either.
                for (let lane = 0; lane < lanes; ++lane) {
                    const av = Array(lanes).fill(1n), bv = Array(lanes).fill(1n);
                    av[lane] = a; bv[lane] = b;
                    run(av, bv);
                }
                run(Array(lanes).fill(a), Array(lanes).fill(b), QC | 0x91);
            }
        }
        for (let i = 0; i < 256; ++i) {
            const random64 = () => (random32() << 32n) | random32();
            const randomLane = () => unsigned(width, width === 128 ? (random64() << 64n) | random64() : random64());
            run(Array.from({ length: lanes }, randomLane), Array.from({ length: lanes }, randomLane));
        }
    }
    console.log(JSON.stringify({ cases, failures, moduleBytes, seed: '0x7a63d291' }));
    assert.equal(failures, 0, 'vector results and sticky FPSR.QC must match the BigInt reference');
} finally {
    rmSync(directory, { recursive: true, force: true });
}
