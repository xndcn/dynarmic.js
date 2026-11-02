// Execute the recorded modules against guest-level expectations. These checks
// are independent of byte snapshots and run before accepting a new baseline.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import path from 'node:path';

const STATE = 0x100;
const ECTX = 0x500;
const FAST_DISPATCH = 0x200000;
const LINK_SLOTS = 0x100000;
const FAULT = 0x02000000;
const CHAIN_RESULT = 42;
const A32_PC = STATE + 80;
const A32_LR = STATE + 76;
const A64_PC = STATE + 16;
const A64_LR = STATE + 32 + 30 * 8;

// (i32, i32) -> i32, returning a distinct value to detect actual tail calls.
const chainModule = new WebAssembly.Module(Uint8Array.from([
  0, 97, 115, 109, 1, 0, 0, 0,
  1, 7, 1, 96, 2, 127, 127, 1, 127,
  3, 2, 1, 0,
  7, 5, 1, 1, 102, 0, 0,
  10, 6, 1, 4, 0, 65, 42, 11,
]));

function fixture(directory, name) {
  const memory = new WebAssembly.Memory({initial: 40});
  const view = new DataView(memory.buffer);
  const table = new WebAssembly.Table({initial: 2, element: 'anyfunc'});
  table.set(1, new WebAssembly.Instance(chainModule).exports.f);
  const exceptions = [];
  const module = new WebAssembly.Module(readFileSync(path.join(directory, `${name}.wasm`)));
  const instance = new WebAssembly.Instance(module, {e: {m: memory, t: table,
    a64_raise(state, context, pc, exception) {
      assert.equal(state, STATE);
      assert.equal(context, ECTX);
      exceptions.push({pc, exception});
      view.setUint32(ECTX, FAULT, true);
    },
  }});
  return {view, exceptions, run: () => instance.exports.f(STATE, ECTX)};
}

export function verifyRegionCorpusSemantics(directory) {
  let executions = 0;
  // BEQ directly reaches BX LR; BNE takes the backward block first. Both
  // preserve flags and return to the caller. Exercise both indirect caches.
  for (const nzcv of [0, 0x40000000]) for (const transfer of ['miss', 'rsb', 'fast-dispatch']) {
    const {view, run} = fixture(directory, 'a32_conditional_backward');
    const returnPC = 0x5000;
    const fastEntry = FAST_DISPATCH + (((returnPC >>> 2) ^ (returnPC >>> 14)) & 4095) * 16;
    view.setUint32(STATE, nzcv, true);
    view.setUint32(A32_PC, 0x2000, true);
    view.setUint32(A32_LR, returnPC, true);
    view.setUint32(STATE + 20, 0x12345678, true);
    if (transfer === 'rsb') {
      view.setUint32(ECTX + 12, 1, true);
      view.setBigUint64(ECTX + 40, BigInt(returnPC), true);
      view.setUint32(ECTX + 48, 1, true);
    } else if (transfer === 'fast-dispatch') {
      view.setBigUint64(fastEntry, BigInt(returnPC), true);
      view.setUint32(fastEntry + 8, 1, true);
    }
    assert.equal(run(), transfer === 'miss' ? 0 : CHAIN_RESULT, `A32 ${transfer} Z=${nzcv !== 0}`);
    assert.equal(view.getUint32(A32_PC, true), returnPC);
    assert.equal(view.getUint32(STATE, true), nzcv);
    assert.equal(view.getUint32(STATE + 20, true), 0x12345678, 'unmodified r0');
    assert.equal(view.getUint32(ECTX + 12, true), transfer === 'rsb' ? 0 : 7, 'one RSB pop');
    assert.equal(view.getUint32(ECTX + 16, true), transfer === 'miss' ? fastEntry : 0);
    executions++;
  }

  // BX chooses halfword alignment for Thumb and word alignment for ARM.
  // Exercise both condition paths and every combination of the target's low
  // bits. In particular LR=0x5002 must produce ARM PC=0x5000, not 0x5002.
  for (const nzcv of [0, 0x40000000]) for (const target of [0x5000, 0x5001, 0x5002, 0x5003]) {
    const {view, run} = fixture(directory, 'a32_conditional_backward');
    const thumb = target & 1;
    const expectedPC = target & (thumb ? ~1 : ~3);
    view.setUint32(STATE, nzcv, true);
    view.setUint32(A32_PC, 0x2000, true);
    view.setUint32(A32_LR, target, true);
    assert.equal(run(), 0, 'uncached BX returns to dispatch');
    assert.equal(view.getUint32(A32_PC, true), expectedPC, `BX LR=${target.toString(16)} alignment`);
    assert.equal(view.getUint32(STATE + 84, true), thumb, 'BX completed descriptor and T bit');
    assert.equal(view.getUint32(STATE, true), nzcv, 'BX preserves NZCV');
    executions++;
  }

  for (const [name, entry, faultPC] of [
    ['a64_single_block', 0x1000, 0x1000],
    ['a64_backward_conditional', 0x2000, 0x2004],
    ['a64_bl_ret_push_rsb', 0x3000, 0x3004],
  ]) for (const nzcv of [0, 0x40000000]) for (const rsbTop of [0, 7]) {
    const {view, run, exceptions} = fixture(directory, name);
    view.setUint32(STATE, nzcv, true);
    view.setBigUint64(A64_PC, BigInt(entry), true);
    view.setBigUint64(STATE + 32, 0x123456789abcdef0n, true);
    view.setUint32(ECTX + 12, rsbTop, true);
    assert.equal(run(), FAULT, name);
    assert.deepEqual(exceptions, [{pc: BigInt(faultPC), exception: 8n}], `${name}: one BRK`);
    assert.equal(view.getBigUint64(A64_PC, true), BigInt(faultPC + 4));
    assert.equal(view.getUint32(STATE, true), nzcv);
    assert.equal(view.getBigUint64(STATE + 32, true), 0x123456789abcdef0n, 'unmodified x0');
    assert.equal(view.getUint32(ECTX + 12, true), rsbTop, 'balanced BL/RET or no RSB use');
    if (name === 'a64_bl_ret_push_rsb') assert.equal(view.getBigUint64(A64_LR, true), 0x3004n);
    executions++;
  }

  // The 65th block lies outside the selection budget. The region must leave
  // at its entry (0x4100), patch the 64th slot, and chain only when linked.
  for (const linked of [false, true]) {
    const {view, run, exceptions} = fixture(directory, 'a64_max_64_blocks');
    const lastSlot = LINK_SLOTS + 63 * 4;
    view.setBigUint64(A64_PC, 0x4000n, true);
    if (linked) view.setUint32(lastSlot, 1, true);
    assert.equal(run(), linked ? CHAIN_RESULT : 0);
    assert.equal(view.getBigUint64(A64_PC, true), 0x4100n);
    assert.equal(view.getUint32(ECTX + 8, true), linked ? 0 : lastSlot);
    assert.deepEqual(exceptions, [], 'unselected BRK must not execute');
    executions++;
  }
  return {executions};
}
