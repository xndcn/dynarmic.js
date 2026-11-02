// Checks the 64-bit-lane saturating add/sub block emitted by sat64_ir.
// usage: node sat64.mjs sat64.wasm
import { readFileSync } from 'node:fs';
const memory = new WebAssembly.Memory({ initial: 1 });
const { instance } = await WebAssembly.instantiate(readFileSync(process.argv[2]), { e: { m: memory } });
const dv = new DataView(memory.buffer);
const STATE = 0x100, ECTX = 0x400, EXT = STATE + 96, FPSR = STATE + 16;
const vals = [0n, 1n, 2n, 0x7fffffffffffffffn, 0x8000000000000000n, 0xffffffffffffffffn, 0x4000000000000000n, 0x8000000000000001n, 0xfffffffffffffffen, 0x123456789abcdefn, 0xfedcba9876543210n];
const M = 1n << 64n, S = 1n << 63n;
const sgn = x => (x >= S ? x - M : x);
const clS = x => (x > S - 1n ? [S - 1n, 1] : x < -S ? [-S, 1] : [x, 0]);
const clU = x => (x > M - 1n ? [M - 1n, 1] : x < 0n ? [0n, 1] : [x, 0]);
const u64 = x => ((x % M) + M) % M;
let bad = 0, n = 0;
for (const a0 of vals) for (const a1 of vals) for (const b0 of vals) for (const b1 of vals) {
  dv.setBigUint64(EXT + 16, a0, true); dv.setBigUint64(EXT + 24, a1, true); // Q1 = ext_regs[4..7]
  dv.setBigUint64(EXT + 32, b0, true); dv.setBigUint64(EXT + 40, b1, true); // Q2
  dv.setUint32(FPSR, 0, true);
  instance.exports.f(STATE, ECTX);
  const A = [a0, a1], B = [b0, b1];
  let qc = 0; const exp = [[], [], [], []];
  for (let l = 0; l < 2; l++) {
    const sa = sgn(A[l]), sb = sgn(B[l]);
    let r;
    r = clS(sa + sb); exp[0][l] = u64(r[0]); qc |= r[1];
    r = clS(sa - sb); exp[1][l] = u64(r[0]); qc |= r[1];
    r = clU(A[l] + B[l]); exp[2][l] = r[0]; qc |= r[1];
    r = clU(A[l] - B[l]); exp[3][l] = r[0]; qc |= r[1];
  }
  // each op sets QC independently; the register file only reveals the OR
  const got = [];
  for (let q = 0; q < 4; q++) got.push([dv.getBigUint64(EXT + 16 * (3 - 1 + q) + 0 + 16, true), dv.getBigUint64(EXT + 16 * (3 - 1 + q) + 8 + 16, true)]);
  const gq = (dv.getUint32(FPSR, true) >>> 27) & 1;
  n++;
  for (let q = 0; q < 4; q++) for (let l = 0; l < 2; l++) if (got[q][l] !== exp[q][l]) { if (bad++ < 5) console.log('MISMATCH op', q, 'lane', l, A, B, got[q][l].toString(16), exp[q][l].toString(16)); }
  if (gq !== qc) { if (bad++ < 5) console.log('QC mismatch', A, B, gq, qc); }
}
console.log(`cases=${n} bad=${bad}`);
process.exitCode = bad ? 1 : 0;
