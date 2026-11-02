// Synthetic protocol data; never performance measurements.
import {report as environment, samples} from './fixtures.mjs';
import {coremarkCases, coremarkMethodology, parseCoremark, coremarkIdentity, summarizeCoremark} from './coremark_protocol.mjs';
export function output(ticks = 500000, crcfinal = '65c5') {
  const duration = ticks >= 10000000;
  return `2K performance run parameters for coremark.
CoreMark Size    : 666
Total ticks      : ${ticks}
Total time (secs): ${(ticks / 1e6).toFixed(6)}
Iterations/Sec   : ${(4000 / (ticks / 1e6)).toFixed(6)}
${duration ? '' : 'ERROR! Must execute for at least 10 secs for a valid result!\n'}Iterations       : 4000
Compiler version : test compiler
Compiler flags   : -O2
Memory location  : STACK
seedcrc          : 0xe9f5
[0]crclist       : 0xe714
[0]crcmatrix     : 0x1fd7
[0]crcstate      : 0x8e3a
[0]crcfinal      : 0x${crcfinal}
${duration ? `Correct operation validated. See README.md for run and reporting rules.\nCoreMark 1.0 : ${(4000 / (ticks / 1e6)).toFixed(6)} / test compiler -O2 / STACK` : 'Errors detected'}\n`;
}
export function workload() {
  const names = ['core_list_join.c', 'core_main.c', 'core_matrix.c', 'core_state.c', 'core_util.c',
    'barebones/cvt.c', 'barebones/ee_printf.c', 'coremark.h'].map(n => `upstream/${n}`);
  names.push('port/core_portme.c', 'port/core_portme.h', 'port/build.py', 'a32/start.S', 'a32/link.ld',
    'a64/start.S', 'a64/link.ld', 'a64/include/string.h', 'a64/include/math.h');
  return {schema: 'dynarmic.js/coremark-build-v1', revision: 'c'.repeat(40), iterations: 4000,
    dataSize: 2000, contexts: 1, sources: Object.fromEntries(names.map(n => [n, 'd'.repeat(64)])),
    uartSourceSha256: 'e'.repeat(64), compilers: {a32: 'gcc', a64: 'clang', native: 'gcc'},
    flags: {a32: '-O2', a64: '-O2', native: '-O2'}, commands: {a32: ['gcc'], a64: ['clang'], native: ['gcc']},
    artifacts: Object.fromEntries(['a32', 'a64', 'native'].map(a => [a,
      {file: a === 'native' ? 'coremark_host' : `coremark_${a}.bin`, sha256: 'f'.repeat(64)}]))};
}
export function fixture(profile = false, ticks = 500000) {
  const r = environment(profile);
  r.schema = 'dynarmic.js/coremark-workload-v1';
  r.runtime.cpuAffinity = '0-3'; r.artifact = {jsSha256: 'a'.repeat(64), wasmSha256: 'b'.repeat(64)};
  r.workload = workload(); r.workloadSha256 = coremarkIdentity(r.workload); r.methodology = coremarkMethodology;
  r.native = {stdout: output(), metrics: parseCoremark(output(), 4000)};
  r.cases = coremarkCases.map(({name, architecture, memoryMode}) => ({name, architecture, memoryMode, runs: 5,
    samples: Array.from({length: 5}, (_, process) => ({process, haltReason: 0x01000000, runMs: ticks / 1000 + 50,
      stdout: output(ticks), metrics: parseCoremark(output(ticks), 4000),
      compile: samples(profile)[0].compile, profile: samples(profile)[0].profile}))}));
  return refresh(r);
}
export function refresh(r) {
  r.cases.forEach(c => { c.summary = summarizeCoremark(c.samples, r.build.profile); });
  r.durationQualified = r.cases.every(c => c.summary.durationQualified);
  return r;
}
