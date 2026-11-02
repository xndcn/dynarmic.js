// Exercise the actual browser host boundary, including normal exit after a fault.
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import path from 'node:path';
const create = createRequire(import.meta.url)(path.resolve(process.argv[2] || 'build-em/examples/browser', 'dist/arm_jit.js'));
const arm = await create();
assert.equal(arm._arm_run(0x10000), 0x02000000, 'run before initialization');
assert.equal(arm._arm_reg(-1), 0, 'register before initialization');
assert.equal(arm._arm_init(3, 0), -1, 'invalid memory mode');
assert.equal(arm._arm_init(0, 2), -1, 'invalid architecture');
for (const arch of [0, 1]) for (const mode of [0, 1, 2]) {
  for (const fault of [false, true, false]) {
    const ram = arm._arm_ram();
    arm.HEAPU8.fill(0, ram, ram + arm._arm_ram_size());
    const code = new DataView(arm.HEAPU8.buffer);
    // A32 UDF / A64 BRK, or SVC #0. Back edges bound speculative discovery.
    code.setUint32(ram + 0x10000, arch === 0 ? (fault ? 0xe7f000f0 : 0xef000000)
      : (fault ? 0xd4200000 : 0xd4000001), true);
    code.setUint32(ram + 0x10004, arch === 0 ? 0xeafffffd : 0x17ffffff, true);
    arm._arm_init(mode, arch);
    assert.equal(arm._arm_run(0x10000), fault ? 0x02000000 : 0x01000000, `arch=${arch} mode=${mode} fault=${fault}`);
  }
}

// Fixed instructions exercise the real memory callbacks and both fast paths.
// Address literals keep the test independent of a guest assembler at runtime.
const accesses = [
  {arch: 0, width: 1, read: 0xe5d20000, write: 0xe5c20000}, // LDRB / STRB r0, [r2]
  {arch: 0, width: 2, read: 0xe1d200b0, write: 0xe1c200b0}, // LDRH / STRH
  {arch: 0, width: 4, read: 0xe5920000, write: 0xe5820000}, // LDR / STR
  {arch: 0, width: 8, read: 0xe1c200d0, write: 0xe1c200f0}, // LDRD / STRD r0, r1, [r2]
  {arch: 1, width: 1, read: 0x39400020, write: 0x39000020}, // LDRB / STRB w0, [x1]
  {arch: 1, width: 2, read: 0x79400020, write: 0x79000020}, // LDRH / STRH
  {arch: 1, width: 4, read: 0xb9400020, write: 0xb9000020}, // LDR / STR w0, [x1]
  {arch: 1, width: 8, read: 0xf9400020, write: 0xf9000020}, // LDR / STR x0, [x1]
  {arch: 1, width: 16, read: 0x3dc00020, write: 0x3d800020}, // LDR / STR q0, [x1]
];
let memoryRuns = 0;
for (const {arch, width, ...ops} of accesses) for (const mode of [0, 1, 2]) {
  const ram = arm._arm_ram();
  const size = arm._arm_ram_size();
  for (const operation of ['read', 'write']) {
    // Include the last valid access, crossing the RAM boundary, unmapped RAM,
    // and A64 addition overflow. Rejected writes must leave all RAM untouched.
    const addresses = [BigInt(size - width), BigInt(size - width + 1), BigInt(size),
      arch === 0 ? 0xffffffffn : 0xfffffffffffffff8n];
    for (const address of addresses) {
      const valid = address + BigInt(width) <= BigInt(size);
      arm.HEAPU8.fill(0xa5, ram, ram + size);
      const code = new DataView(arm.HEAPU8.buffer);
      const words = arch === 0
        ? [0xe59f2010, 0xe3a0005a, 0xe3a0105a, ops[operation], 0xef000000, 0xeafffffe]
        : [0x58000101, 0xd2800b40, 0x4f02e740, ops[operation], 0xd4000001, 0x14000000];
      words.forEach((word, i) => code.setUint32(ram + 0x10000 + i * 4, word, true));
      if (arch === 0) code.setUint32(ram + 0x10018, Number(address), true);
      else code.setBigUint64(ram + 0x10020, address, true);
      arm._arm_init(mode, arch);
      const beforeTail = arm.HEAPU8.slice(ram + size - 16, ram + size + 16);
      const beforeHead = arm.HEAPU8.slice(ram, ram + 16);
      const halt = arm._arm_run(0x10000);
      const label = `arch=${arch} mode=${mode} ${operation}${width} address=${address.toString(16)}`;
      assert.equal((halt & 0x02000000) !== 0, !valid, label);
      if (!valid) {
        assert.deepEqual(arm.HEAPU8.slice(ram + size - 16, ram + size + 16), beforeTail, `${label}: RAM boundary`);
        assert.deepEqual(arm.HEAPU8.slice(ram, ram + 16), beforeHead, `${label}: no wrapped write`);
      } else if (operation === 'write') {
        assert.equal(arm.HEAPU8[ram + Number(address)], 0x5a, label);
      }
      memoryRuns++;
    }
  }
}
for (const arch of [0, 1]) {
  arm._arm_init(0, arch);
  assert.equal(arm._arm_run(arm._arm_ram_size()), 0x02000000, 'unmapped entry');
  assert.equal(arm._arm_run(0x10001), 0x02000000, 'unaligned entry');
  assert.equal(arm._arm_reg(-1), 0, 'negative register index');
  assert.equal(arm._arm_reg(arch === 0 ? 16 : 31), 0, 'invalid register index');
}
console.log(`PASS browser host normal/fault halts (18 runs), RAM boundaries (${memoryRuns} runs) and API validation`);
