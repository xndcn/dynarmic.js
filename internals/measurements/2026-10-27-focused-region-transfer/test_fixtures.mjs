// Synthetic-only reports for tooling tests. These are not collected CoreMark evidence.
import {createCoremarkRegionDiagnosticReport} from '../../../tests/bench/coremark/region_diagnostics.mjs';
import {coremarkIdentity, parseCoremark} from '../../../tests/bench/coremark_protocol.mjs';
import {analyzeRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {profileCounters} from '../../../tests/bench/protocol.mjs';

export const ENTRY = '0x000000000001202c';
export const SOURCE = '0x0000000000012370';
export const TARGET = '0x000000000001237c';
export const RETURNS = ['0x0000000000012418', '0x0000000000012424', '0x0000000000012428'];
export const BASELINE_FOCUS = '';
export const CANDIDATE_FOCUS = [ENTRY, SOURCE, TARGET, ...RETURNS].join(',');
export const INTERNAL_ENTRY = '0x0000000000013000';
export const INTERNAL_SOURCE = '0x0000000000013004';
export const INTERNAL_TARGET = '0x0000000000013008';
export const COMMON_ENTRY = '0x0000000000015000';
export const COMMON_TARGET = '0x0000000000014000';
export const ZERO = '0x0000000000000000';
export const u64 = value => '0x' + BigInt(value).toString(16).padStart(16, '0');

const STDOUT = `2K performance run parameters for coremark.
CoreMark Size    : 666
Total ticks      : 1000000
Total time (secs): 1.000000
Iterations/Sec   : 200000.000000
ERROR! Must execute for at least 10 secs for a valid result!
Iterations       : 200000
Compiler version : synthetic
Compiler flags   : -O2
Memory location  : STACK
seedcrc          : 0xe9f5
[0]crclist       : 0xe714
[0]crcmatrix     : 0x1fd7
[0]crcstate      : 0x8e3a
[0]crcfinal      : 0x4983
Errors detected`;

const block = (descriptor, emitted = true, returnSite = false) => ({
  descriptor, emitted, entryReachable: emitted, returnSite,
});
const region = (regionId, entryDescriptor, blocks, layout = 'structured') => ({
  regionId, frontend: 'a64', entryDescriptor, entryPc: entryDescriptor,
  entryIndex: blocks.findIndex(row => row.descriptor === entryDescriptor),
  moduleBytes: 100 + regionId, entryBodyBytes: 90 + regionId, layout, blocks,
  liveRegionIdsBeforeCompile: [],
});
const exit = (exitSite, exitBlockDescriptor, kind, staticTargetDescriptor, count) => ({
  exitSite, exitBlockDescriptor, kind, staticTargetDescriptor, count: u64(count),
  lastDynamicTargetDescriptor: count === 0 ? ZERO : staticTargetDescriptor || ZERO,
  lastDynamicTargetPc: count === 0 ? ZERO : staticTargetDescriptor || ZERO,
});
const transfer = (regionId, callerEntryDescriptor, entryCount, exits) => ({
  regionId, frontend: 'a64', callerEntryDescriptor, callerEntryPc: callerEntryDescriptor,
  entryCount: u64(entryCount), exits,
});

function rawHistories(candidate) {
  const focusBlocks = [
    block(ENTRY), block(SOURCE),
    ...(candidate ? [block(TARGET)] : []),
    ...(candidate ? RETURNS : RETURNS.slice(1)).map(value => block(value)),
  ];
  const internalBlocks = [block(INTERNAL_ENTRY), block(INTERNAL_SOURCE),
    ...(candidate ? [] : [block(INTERNAL_TARGET)])];
  const commonCount = candidate ? 3 : 2;
  return {
    regions: [
      region(1, ENTRY, focusBlocks),
      region(2, INTERNAL_ENTRY, internalBlocks),
      region(3, COMMON_ENTRY, [block(COMMON_ENTRY)], 'single'),
    ],
    transfers: [
      transfer(1, ENTRY, 5, [
        exit(0, ENTRY, 'external-direct-call', COMMON_TARGET, 0),
        ...(candidate ? [] : [exit(1, SOURCE, 'external-direct-call', TARGET, 5)]),
        ...(candidate ? [exit(1, RETURNS[0], 'other-tail-transfer', null, 5)] : []),
      ]),
      transfer(2, INTERNAL_ENTRY, 0, candidate
        ? [exit(0, INTERNAL_SOURCE, 'external-direct-call', INTERNAL_TARGET, 0)] : []),
      transfer(3, COMMON_ENTRY, commonCount, [
        exit(0, COMMON_ENTRY, 'rsb-return', null, commonCount),
        exit(1, COMMON_ENTRY, 'external-direct-call', COMMON_TARGET, 0),
      ]),
    ],
  };
}

function syntheticWorkload() {
  const names = ['core_list_join.c', 'core_main.c', 'core_matrix.c', 'core_state.c', 'core_util.c',
    'barebones/cvt.c', 'barebones/ee_printf.c', 'coremark.h'].map(name => `upstream/${name}`);
  names.push('port/core_portme.c', 'port/core_portme.h', 'port/build.py', 'a32/start.S', 'a32/link.ld',
    'a64/start.S', 'a64/link.ld', 'a64/include/string.h', 'a64/include/math.h');
  const compilers = Object.fromEntries(['a32', 'a64', 'native'].map(name => [name, 'synthetic-cc']));
  const flags = Object.fromEntries(['a32', 'a64', 'native'].map(name => [name, '-O2']));
  const commands = Object.fromEntries(['a32', 'a64', 'native'].map(name => [name, ['synthetic-build']]));
  const artifacts = Object.fromEntries(['a32', 'a64', 'native'].map((name, index) => [name, {
    file: name === 'native' ? 'coremark_host' : `coremark_${name}.bin`, sha256: String(index + 1).repeat(64),
  }]));
  return {
    schema: 'dynarmic.js/coremark-build-v1', revision: '1'.repeat(40), iterations: 200000,
    dataSize: 2000, contexts: 1, sources: Object.fromEntries(names.map((name, index) =>
      [name, (index + 10).toString(16).repeat(64).slice(0, 64)])),
    uartSourceSha256: '9'.repeat(64), compilers, flags, commands, artifacts,
  };
}

function rawReport(candidate) {
  const raw = rawHistories(candidate);
  const focus = candidate ? CANDIDATE_FOCUS : BASELINE_FOCUS;
  return {
    schema: 'dynarmic.js/coremark-region-diagnostics-v1', architecture: 'a64', memoryMode: '0',
    workloadSha256: ZERO, artifact: {jsSha256: (candidate ? 'c' : 'a').repeat(64),
      wasmSha256: (candidate ? 'd' : 'b').repeat(64)},
    native: {stdout: STDOUT, metrics: null},
    sample: {
      haltReason: 0x01000000, runMs: 2000, stdout: STDOUT, metrics: null,
      compile: {blocks: 0, bytes: 0, translateMs: 0, optMs: 0, emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0},
      profile: {schema: 'dynarmic.js/profile-v1', enabled: true,
        ...Object.fromEntries(profileCounters.map(key => [key, 0])), runCount: 1},
    },
    regionDiagnostics: {schema: 'dynarmic.js/region-diagnostics-v2', regions: raw.regions},
    regionTransferDiagnostics: {schema: 'dynarmic.js/region-transfer-diagnostics-v1', regions: raw.transfers},
    timestamp: '2026-10-27T00:00:00.000Z', node: 'v99.0.0', v8: '99.0.0',
    runtime: {nodeOptions: '', cpuAffinity: '0'},
    host: {platform: 'linux', release: 'synthetic', arch: 'x64', cpu: 'synthetic', cpus: 1},
    git: {revision: '2'.repeat(40), upstream: '3'.repeat(40), dirty: false},
    build: {type: 'Release', profile: true, a64RegionFocus: focus, compilerId: 'synthetic',
      compilerVersion: '1', emscriptenVersion: '1', cxxFlags: '', configCxxFlags: '-O3', linkFlags: '', configLinkFlags: ''},
    workload: syntheticWorkload(),
  };
}

// Recompute every serialized derived field after synthetic raw-leaf mutations.
export function refreshReport(input) {
  const report = structuredClone(input);
  const native = {...report.native, metrics: parseCoremark(report.native.stdout, report.workload.iterations)};
  const metrics = parseCoremark(report.sample.stdout, report.workload.iterations);
  const duplication = analyzeRegionDiagnostics(report.regionDiagnostics);
  const compile = {...report.sample.compile, blocks: duplication.totals.regionCount,
    bytes: duplication.totals.moduleBytes};
  const profile = {...report.sample.profile, compiledBlocks: compile.blocks, emittedBytes: compile.bytes,
    peakCachedBlocks: compile.blocks, peakCachedBytes: compile.bytes};
  const sample = {...report.sample, compile, profile, metrics,
    regionDiagnostics: report.regionDiagnostics, regionTransferDiagnostics: report.regionTransferDiagnostics};
  const fresh = createCoremarkRegionDiagnosticReport({architecture: report.architecture,
    memoryMode: report.memoryMode, workloadSha256: coremarkIdentity(report.workload),
    artifact: report.artifact, native, sample});
  return {...fresh, timestamp: report.timestamp, node: report.node, v8: report.v8,
    runtime: report.runtime, host: report.host, git: report.git, build: report.build, workload: report.workload};
}

export function makeReports() {
  return {baseline: refreshReport(rawReport(false)), candidate: refreshReport(rawReport(true))};
}
