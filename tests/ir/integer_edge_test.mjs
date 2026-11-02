// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
// Usage: node integer_edge_test.mjs <native-generator-or-module-directory> [--bench]
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { performance } from 'node:perf_hooks';

const source = resolve(process.argv[2]);
const generated = !statSync(source).isDirectory();
const dir = generated ? mkdtempSync(join(tmpdir(), 'dynarmic-integer-')) : source;
try {
  if (generated) {
    const run = spawnSync(source, [dir], { encoding: 'utf8' });
    assert.equal(run.status, 0, `generator failed: ${run.stderr}`);
  }
  const manifest = JSON.parse(readFileSync(join(dir, 'manifest.json'), 'utf8'));
  const memory = new WebAssembly.Memory({ initial: 1 });
  const dv = new DataView(memory.buffer);
  const state = 0x100, ectx = 0x800;
  const reg = i => state + manifest.regs + 4 * i;
  const qc = 1 << 27;
  const modules = [];
  for (const spec of manifest.modules) {
    const bytes = readFileSync(join(dir, `${spec.name}.wasm`));
    assert(WebAssembly.validate(bytes), spec.name);
    const { instance } = await WebAssembly.instantiate(bytes, { e: { m: memory } });
    modules.push({ ...spec, f: instance.exports.f });
  }
  // Arithmetic in Number is exact for these <= 33-bit intermediate values.
  // Each lane is extracted independently, avoiding the emitter's bit tricks.
  function reference(name, a, b) {
    if (name.startsWith('SignedSaturated')) {
      const full = (a | 0) + (name.includes('Sub') ? -(b | 0) : (b | 0));
      const value = Math.max(-0x80000000, Math.min(0x7fffffff, full));
      return { value: value >>> 0, overflow: value !== full };
    }
    const bits = name.endsWith('16') ? 16 : 8;
    const mask = 2 ** bits - 1;
    const signed = name.endsWith(`S${bits}`);
    const sat = name.includes('Saturated'), half = name.includes('Halving');
    const exchange = name.includes('AddSub') || name.includes('SubAdd');
    let value = 0, ge = 0;
    const lane = (word, i) => {
      const x = (word >>> (i * bits)) & mask;
      return signed && x >= 2 ** (bits - 1) ? x - 2 ** bits : x;
    };
    for (let i = 0; i < 32 / bits; ++i) {
      const sub = exchange ? (name.includes('AddSub') ? i === 0 : i === 1) : name.includes('Sub');
      const full = lane(a, i) + (sub ? -1 : 1) * lane(b, exchange ? i ^ 1 : i);
      const condition = signed || sub ? full >= 0 : full > mask;
      if (condition) ge = (ge | (mask << (i * bits))) >>> 0;
      let result = full;
      if (half) result = Math.floor(result / 2);
      if (sat) result = Math.max(signed ? -(2 ** (bits - 1)) : 0,
        Math.min(signed ? 2 ** (bits - 1) - 1 : mask, result));
      value = (value | ((result & mask) << (i * bits))) >>> 0;
    }
    return { value, ge };
  }
  const edges = [0, 1, 2, 0xffffffff, 0x7fffffff, 0x80000000, 0x80000001,
    0x7f807f80, 0x807f807f, 0x00ff00ff, 0xff00ff00, 0x7fff8000, 0x80007fff,
    0x0000ffff, 0xffff0000, 0x12345678, 0x89abcdef];
  let seed = 0x32c0ffee;
  function random() {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    return seed >>> 0;
  }
  let checks = 0;
  function check(test, a, b, initial = 0x00400015) {
    const expected = reference(test.name, a, b);
    dv.setUint32(reg(0), a, true); dv.setUint32(reg(1), b, true);
    dv.setUint32(state + manifest.fpsr, initial, true);
    dv.setUint32(state + manifest.q, 0, true);
    dv.setUint32(state + manifest.ge, 0x5a5a5a5a, true);
    assert.equal(test.f(state, ectx), 0);
    const context = `${test.name} a=${a.toString(16)} b=${b.toString(16)}`;
    assert.equal(dv.getUint32(reg(2), true), expected.value, `${context}: result`);
    const writesQC = test.name.startsWith('SignedSaturated') && !test.name.includes('WithFlag');
    assert.equal(dv.getUint32(state + manifest.fpsr, true),
      (initial | (writesQC && expected.overflow ? qc : 0)) >>> 0, `${context}: FPSR`);
    assert.equal(dv.getUint32(state + manifest.q, true),
      test.name.endsWith('_live') && expected.overflow ? qc : 0, `${context}: CPSR.Q`);
    const writesGE = /^Packed(Add|Sub)/.test(test.name);
    assert.equal(dv.getUint32(state + manifest.ge, true),
      writesGE ? expected.ge : 0x5a5a5a5a, `${context}: GE`);
    ++checks;
  }
  if (!process.argv.includes('--bench')) {
    for (const test of modules.filter(test => !test.scalar)) {
      for (const a of edges) for (const b of edges) {
        check(test, a, b);
        check(test, a, b, qc | 0x00400015); // sticky QC and unrelated FPSR bits
      }
      for (let i = 0; i < 4096; ++i) check(test, random(), random());
      if (test.name.endsWith('8')) {
        for (let a = 0; a < 256; ++a) for (let b = 0; b < 256; ++b) {
          // Exhaust every lane pair and vary neighboring lanes independently.
          const aa = (a | ((255 - a) << 8) | (((a + 97) & 255) << 16) | (((a + 53) & 255) << 24)) >>> 0;
          const bb = (b | (((b + 61) & 255) << 8) | ((255 - b) << 16) | (((b + 11) & 255) << 24)) >>> 0;
          check(test, aa, bb);
        }
      }
    }
    for (const test of modules.filter(test => test.scalar)) {
      const [name, flags] = test.name.split('_');
      const bits = test.bits, width = BigInt(bits), limit = 1n << width, mask = limit - 1n;
      const signed = x => BigInt.asIntN(bits, x);
      const values = [0n, 1n, 2n, mask, mask - 1n, limit / 2n, limit / 2n - 1n,
        limit / 2n + 1n, 0x12345678n, mask ^ 0x12345678n];
      const shift = name.includes('Shift') || name.startsWith('Rotate');
      function scalarCheck(a, b, carry) {
        let value, cout = 0, overflow = false, nzcv = 0;
        if (name.startsWith('Add') || name.startsWith('Sub')) {
          const sub = name.startsWith('Sub');
          const full = sub ? a - b + BigInt(carry) - 1n : a + b + BigInt(carry);
          const fullSigned = sub ? signed(a) - signed(b) + BigInt(carry) - 1n
            : signed(a) + signed(b) + BigInt(carry);
          value = BigInt.asUintN(bits, full);
          overflow = fullSigned < -(limit / 2n) || fullSigned >= limit / 2n;
          cout = sub ? +(full >= 0n) : +(full >= limit);
          nzcv = ((signed(value) < 0n ? 0x80000000 : 0) | (value === 0n ? 0x40000000 : 0)
            | (cout << 29) | (+overflow << 28)) >>> 0;
        } else if (shift) {
          const amount = b & 255n;
          if (amount === 0n) { value = a; cout = carry; }
          else if (name.startsWith('Rotate')) {
            const rotate = amount % width;
            value = ((a >> rotate) | (a << (width - rotate))) & mask;
            cout = Number(value >> (width - 1n));
          } else if (name.startsWith('LogicalShiftLeft')) {
            value = amount >= width ? 0n : (a << amount) & mask;
            cout = amount > width ? 0 : Number((a >> (width - amount)) & 1n);
          } else {
            const input = name.startsWith('Arithmetic') ? signed(a) : a;
            value = BigInt.asUintN(bits, input >> amount);
            cout = Number((input >> (amount - 1n)) & 1n);
          }
        } else if (name.includes('Div')) {
          const aa = name.startsWith('Signed') ? signed(a) : a;
          const bb = name.startsWith('Signed') ? signed(b) : b;
          value = BigInt.asUintN(bits, bb === 0n ? 0n : aa / bb);
        } else {
          const aa = name.startsWith('Signed') ? signed(a) : a;
          const bb = name.startsWith('Signed') ? signed(b) : b;
          value = BigInt.asUintN(bits, (aa * bb) >> width);
        }
        dv.setBigUint64(reg(0), a, true); dv.setBigUint64(reg(2), b, true);
        dv.setUint32(reg(4), carry ? 0x80000000 : 0, true);
        dv.setUint32(state, 0, true);
        dv.setUint32(state + manifest.q, 0, true);
        dv.setUint32(state + manifest.fpsr, 0x00400015, true);
        assert.equal(test.f(state, ectx), 0);
        const got = bits === 64 ? dv.getBigUint64(reg(6), true) : BigInt(dv.getUint32(reg(6), true));
        const context = `${test.name} a=${a.toString(16)} b=${b.toString(16)} c=${carry}`;
        assert.equal(got, value, `${context}: result`);
        if (flags === 'nzcv') assert.equal(dv.getUint32(state, true), nzcv, `${context}: NZCV`);
        const expectedQ = flags === 'overflow' ? +overflow : shift && bits === 32 ? cout : 0;
        assert.equal(dv.getUint32(state + manifest.q, true), expectedQ << 27, `${context}: carry/overflow`);
        assert.equal(dv.getUint32(state + manifest.fpsr, true), 0x00400015, `${context}: FPSR`);
        ++checks;
      }
      for (const a of values) for (const b of shift ? Array.from({ length: 256 }, (_, i) => BigInt(i)) : values) {
        scalarCheck(a, b, 0); scalarCheck(a, b, 1);
      }
      for (let i = 0; i < 4096; ++i) {
        const a = ((BigInt(random()) << 32n) | BigInt(random())) & mask;
        const b = ((BigInt(random()) << 32n) | BigInt(random())) & mask;
        scalarCheck(a, b, i & 1);
      }
    }
    console.log(`integer edge cases: ${checks} passed (seed=0x32c0ffee)`);
  } else {
    const samples = [];
    for (const spec of manifest.modules.slice(0, 8)) {
      const bytes = readFileSync(join(dir, `bench_${spec.name}.wasm`));
      const { instance } = await WebAssembly.instantiate(bytes, { e: { m: memory } });
      const f = instance.exports.f;
      dv.setUint32(reg(0), 0x7f802345, true); dv.setUint32(reg(1), 0xfe012468, true);
      let expected = 0x7f802345;
      for (let n = 0; n < 64; ++n) expected = reference(spec.name, expected, 0xfe012468).value;
      f(state, ectx);
      assert.equal(dv.getUint32(reg(2), true), expected);
      for (let n = 0; n < 200000; ++n) f(state, ectx);
      const start = performance.now();
      for (let n = 0; n < 5000000; ++n) f(state, ectx);
      samples.push({ name: spec.name, bytes: bytes.length, iterations: 5000000,
        operationsPerIteration: 64, elapsedMs: performance.now() - start,
        result: dv.getUint32(reg(2), true) });
    }
    console.log(JSON.stringify({ node: process.version, v8: process.versions.v8, samples }));
  }
} finally {
  if (generated) rmSync(dir, { recursive: true, force: true });
}
