// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

// ARM FMINNM/FMAXNM reference: signaling NaNs take priority, one quiet NaN
// loses to a number, and two quiet NaNs select the first. Arithmetic reference
// uses JS only after classifying NaNs by bits; NaN payloads never pass through
// JS floating arithmetic. Signed zeros are checked exactly.
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const dir = mkdtempSync(join(tmpdir(), 'dynarmic-fp-edge-'));
const scratch = new DataView(new ArrayBuffer(8));
let checks = 0, failures = 0;
try {
  const generated = spawnSync(process.argv[2], [dir], { encoding: 'utf8' });
  assert.equal(generated.status, 0, generated.stderr || String(generated.error));
  const offsets = JSON.parse(generated.stdout);
  const memory = new WebAssembly.Memory({ initial: 1 });
  const state = 0x100, ectx = 0x800;
  const view = new DataView(memory.buffer);

  for (const width of [32, 64]) {
    const sign = 1n << BigInt(width - 1);
    const inf = width === 32 ? 0x7f800000n : 0x7ff0000000000000n;
    const quiet = width === 32 ? 0x00400000n : 0x0008000000000000n;
    const one = width === 32 ? 0x3f800000n : 0x3ff0000000000000n;
    const minNormal = width === 32 ? 0x00800000n : 0x0010000000000000n;
    const numbers = [0n, 1n, minNormal - 1n, minNormal, one, one + 1n, inf - 1n, inf,
      inf | 1n, inf | quiet, inf | quiet | 0x123n];
    const values = [...numbers, ...numbers.map(x => x | sign)];
    const getBits = address => width === 32 ? BigInt(view.getUint32(address, true)) : view.getBigUint64(address, true);
    const setBits = (address, bits) => width === 32 ? view.setUint32(address, Number(bits), true) : view.setBigUint64(address, bits, true);
    const toNumber = bits => {
      if (width === 32) { scratch.setUint32(0, Number(bits), true); return scratch.getFloat32(0, true); }
      scratch.setBigUint64(0, bits, true); return scratch.getFloat64(0, true);
    };
    const fromNumber = value => {
      if (width === 32) { scratch.setFloat32(0, value, true); return BigInt(scratch.getUint32(0, true)); }
      scratch.setFloat64(0, value, true); return scratch.getBigUint64(0, true);
    };
    function reference(a, b, mode, isMax) {
      let flags = 0;
      const flush = x => {
        const magnitude = x & (sign - 1n);
        if ((mode & 1) && magnitude > 0n && magnitude < minNormal) {
          flags |= 0x80;
          return x & sign;
        }
        return x;
      };
      a = flush(a); b = flush(b);
      const aNaN = (a & (sign - 1n)) > inf;
      const bNaN = (b & (sign - 1n)) > inf;
      const aSignaling = aNaN && !(a & quiet);
      const bSignaling = bNaN && !(b & quiet);
      if (aSignaling || bSignaling) flags |= 1;
      let nan;
      if (aSignaling) nan = a;
      else if (bSignaling) nan = b;
      else if (aNaN && bNaN) nan = a;
      if (nan !== undefined) return [(mode & 2) ? (inf | quiet) : (nan | quiet), flags];
      if (aNaN) return [b, flags];
      if (bNaN) return [a, flags];
      return [fromNumber(isMax ? Math.max(toNumber(a), toNumber(b)) : Math.min(toNumber(a), toNumber(b))), flags];
    }
    for (let mode = 0; mode < 4; ++mode) for (const isMax of [0, 1]) for (const vector of [0, 1]) for (const controlled of [0, 1]) {
      if (!vector && !controlled) continue;
      const name = `${width}-${mode}-${isMax}-${vector}-${controlled}`;
      const bytes = readFileSync(join(dir, `${name}.wasm`));
      assert.ok(WebAssembly.validate(bytes), name);
      const { instance } = await WebAssembly.instantiate(bytes, { e: { m: memory } });
      for (let i = 0; i < values.length; ++i) for (let j = 0; j < values.length; ++j) {
        const lanes = vector ? 128 / width : 1;
        const base = state + (vector ? offsets.vectors : offsets.registers);
        const stride = vector ? 16 : 8;
        const expected = [];
        // Preserve QC and unrelated cumulative flags, and verify IOC/IDC stay set.
        let flags = ((i + j) & 1) ? 0x08000012 : 0x08000093;
        view.setUint32(state + offsets.fpsr, flags, true);
        for (let lane = 0; lane < lanes; ++lane) {
          const a = values[(i + lane * 7) % values.length];
          const b = values[(j + lane * 3) % values.length];
          setBits(base + lane * (width / 8), a);
          setBits(base + stride + lane * (width / 8), b);
          const [result, raised] = reference(a, b, controlled ? mode : 3, isMax);
          expected.push(result); flags |= raised;
        }
        instance.exports.f(state, ectx);
        for (let lane = 0; lane < lanes; ++lane) {
          const actual = getBits(base + 2 * stride + lane * (width / 8));
          if (actual !== expected[lane]) {
            if (failures++ < 8) console.error(`${name} pair=${i},${j} lane=${lane}: 0x${actual.toString(16)} != 0x${expected[lane].toString(16)}`);
          }
          ++checks;
        }
        const actualFlags = view.getUint32(state + offsets.fpsr, true);
        if (actualFlags !== flags) {
          if (failures++ < 8) console.error(`${name} pair=${i},${j}: FPSR 0x${actualFlags.toString(16)} != 0x${flags.toString(16)}`);
        }
        ++checks;
      }
    }
  }
  assert.equal(failures, 0, `${failures} FP result/FPSR mismatches in ${checks} checks`);
  console.log(`FP numeric edge cases passed: ${checks} result/FPSR checks`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
