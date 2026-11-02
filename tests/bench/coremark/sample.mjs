// One fresh process, one module, one JIT, one Run. Internal collector child.
import {createRequire} from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {captureCpuProfile} from '../scoped_cpu_profile.mjs';

export async function collectCoremarkSample(jit, binary, architecture, memoryMode, cpuProfile = null) {
  if (![jit, binary].every(value => typeof value === 'string' && value)
    || !['a32', 'a64'].includes(architecture) || !['0', '1', '2'].includes(memoryMode)
    || (cpuProfile !== null && (typeof cpuProfile !== 'string' || !cpuProfile))) {
    throw new Error('invalid sample arguments');
  }
  const create = createRequire(import.meta.url)(path.resolve(jit));
  const arm = await create();
  const code = fs.readFileSync(binary), ram = arm._arm_ram(), size = arm._arm_ram_size();
  if (code.length + 0x10000 > size) throw new Error('guest exceeds RAM');
  arm.HEAPU8.fill(0, ram, ram + size);
  arm.HEAPU8.set(code, ram + 0x10000);
  arm._arm_console_clear();
  if (cpuProfile !== null) {
    if (typeof arm._dynarmic_wasm_debug_names !== 'function') throw new Error('debug function names unavailable');
    arm._dynarmic_wasm_debug_names(1);
  }
  arm._arm_init(Number(memoryMode), architecture === 'a32' ? 0 : 1);
  const execute = () => {
    const begin = performance.now();
    const haltReason = arm._arm_run(0x10000);
    const runMs = performance.now() - begin;
    if (haltReason !== 0x01000000) throw new Error(`guest fault: halt reason ${haltReason}`);
    return {haltReason, runMs};
  };
  const {haltReason, runMs} = cpuProfile === null ? execute() : await captureCpuProfile(cpuProfile, execute);
  const stdout = new TextDecoder().decode(arm.HEAPU8.subarray(arm._arm_console(), arm._arm_console() + arm._arm_console_len()));
  const stats = arm.dynarmicWasmStats;
  const snapshot = stats?.getInstance(stats.activeInstance);
  if (!snapshot) throw new Error('missing owning JIT statistics');
  const result = {haltReason, runMs, stdout, compile: snapshot.compile, profile: snapshot.profile || null};
  // Optional diagnostic-only leaf; ordinary production/profile sample records
  // retain exactly their existing shape.
  if (snapshot.regionDiagnostics !== undefined) result.regionDiagnostics = snapshot.regionDiagnostics;
  if (snapshot.regionTransferDiagnostics !== undefined) {
    result.regionTransferDiagnostics = snapshot.regionTransferDiagnostics;
  }
  return result;
}

export function emitCoremarkSample(sample) {
  console.log('COREMARK_SAMPLE ' + JSON.stringify(sample));
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [jit, binary, architecture, memoryMode, ...extra] = process.argv.slice(2);
  if (extra.length) throw new Error('usage: sample.mjs <arm_jit.js> <binary> <a32|a64> <0|1|2>');
  emitCoremarkSample(await collectCoremarkSample(jit, binary, architecture, memoryMode));
}
