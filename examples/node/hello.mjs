// Run the smallest useful guest through the same C++ host as the browser demo.
// All opcodes are included here, so this example needs no ARM cross compiler.
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const args = process.argv.slice(2);
if (args.includes('--help')) {
  console.log('Usage: node examples/node/hello.mjs [build-em/examples/browser/dist]');
  process.exit(0);
}
if (args.length > 1 || args.some(arg => arg.startsWith('-'))) {
  console.error('Usage: node examples/node/hello.mjs [build-em/examples/browser/dist]');
  process.exit(1);
}
const defaultDist = fileURLToPath(new URL('../../build-em/examples/browser/dist/', import.meta.url));
const modulePath = path.resolve(args[0] || defaultDist, 'arm_jit.js');
const LOAD_BASE = 0x10000;
const NORMAL_EXIT = 0x01000000;
const guests = [
  {
    name: 'ARM32', arch: 0,
    code: [
      0xe3a0002a, // mov r0, #42
      0xef000002, // svc #2: print r0 as hexadecimal
      0xef000000, // svc #0: exit
      0xeafffffe, // b .: bound speculative block discovery
    ],
  },
  {
    name: 'AArch64', arch: 1,
    code: [
      0xd2800540, // mov x0, #42
      0xd4000041, // svc #2: print the low 32 bits of x0
      0xd4000001, // svc #0: exit
      0x14000000, // b .
    ],
  },
];

try {
  const createArmJit = createRequire(import.meta.url)(modulePath);
  const arm = await createArmJit();
  for (const {name, arch, code} of guests) {
    // A new JIT owns a fresh code cache. Mode 0 uses the memory callbacks.
    assert.equal(arm._arm_init(0, arch), 0, `${name}: initialization`);
    const ram = arm._arm_ram();
    arm.HEAPU8.fill(0, ram, ram + arm._arm_ram_size());
    const view = new DataView(arm.HEAPU8.buffer);
    code.forEach((word, i) => view.setUint32(ram + LOAD_BASE + i * 4, word, true));
    arm._arm_console_clear();

    assert.equal(arm._arm_run(LOAD_BASE), NORMAL_EXIT, `${name}: guest halt`);
    assert.equal(arm._arm_reg(0), 42, `${name}: register result`);
    // Fetch the current heap after execution: Emscripten can grow its memory.
    const start = arm._arm_console();
    const output = new TextDecoder().decode(arm.HEAPU8.subarray(start, start + arm._arm_console_len()));
    assert.equal(output, '0000002a\n', `${name}: console output`);
    console.log(`${name}: ${output.trim()} (42) — PASS`);
  }
} catch (error) {
  console.error(`Could not run the Node example: ${error?.message || error}`);
  if (error?.code === 'MODULE_NOT_FOUND') {
    console.error('Build the arm_jit target first; see examples/node/README.md.');
  }
  process.exitCode = 1;
}
