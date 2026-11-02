import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {implementations, fourWayImplementations, rotatingOrder, rotatingFourWayOrder} from './compare.mjs';
import {parseWasmCoremarkSample} from './wasm.mjs';
import {analyzeEvidence} from './analyze.mjs';

const crcLines = 'seedcrc          : 0xe9f5\n[0]crclist       : 0xe714\n[0]crcmatrix     : 0x1fd7\n' +
  '[0]crcstate      : 0x8e3a\n[0]crcfinal      : 0x4983\n';
function stdout(ticks, flags) {
  const seconds = (ticks / 1e6).toFixed(6);
  const rate = (200000 / (ticks / 1e6)).toFixed(6);
  const valid = ticks >= 10000000;
  return '2K performance run parameters for coremark.\nCoreMark Size    : 666\n' +
    `Total ticks      : ${ticks}\nTotal time (secs): ${seconds}\nIterations/Sec   : ${rate}\n` +
    `${valid ? '' : 'ERROR! Must execute for at least 10 secs for a valid result!\n'}` +
    `Iterations       : 200000\nCompiler version : clang test\nCompiler flags   : ${flags}\n` +
    `Memory location  : STACK\n${crcLines}` +
    `${valid ? `Correct operation validated. See README.md for run and reporting rules.\nCoreMark 1.0 : ${rate} / clang test ${flags} / STACK\n` : 'Errors detected\n'}`;
}
function diagnostics(ticks) {
  return 'DYNARMIC_GUEST_BYTES=100\nDYNARMIC_MEMORY_MODE=2\n' +
    `DYNARMIC_RUN_MS=${(ticks / 1000 + 5).toFixed(6)}\nDYNARMIC_HALT_REASON=0x01000000\n`;
}
const nativeBuild = {
  schema: 'dynarmic.js/native-coremark-host-build-v1', buildType: 'Release',
  compiler: {id: 'GNU', version: '12.3.0', path: '/usr/bin/c++'},
  flags: {cxx: '', configuration: '-O3 -DNDEBUG'},
  sources: {hostSha256: '1'.repeat(64), cmakeSha256: '2'.repeat(64)},
  revisions: {repositoryHead: '3'.repeat(40), dynarmicRevision: '4'.repeat(40)},
  artifact: {path: '/host', sha256: '5'.repeat(64), bytes: 200},
};
function createEvidence(root) {
  const hostBuild = structuredClone(nativeBuild);
  const ticks = {x86Native: 10000000, qemuUser: 200000000, dynarmicNative: 50000000};
  const flags = {x86Native: 'x86 flags', qemuUser: 'arm flags', dynarmicNative: 'guest flags'};
  const artifacts = {
    x86Native: {path: '/x86', sha256: '6'.repeat(64), bytes: 100},
    qemuUser: {path: '/qemu-guest', sha256: '7'.repeat(64), bytes: 100},
    qemuExecutable: {path: '/qemu', sha256: '8'.repeat(64), bytes: 100},
    dynarmicHost: hostBuild.artifact,
    a64Guest: {path: '/guest', sha256: '9'.repeat(64), bytes: 100},
  };
  const observations = Array.from({length: 15}, (_, sequence) => {
    const round = Math.floor(sequence / 3);
    const implementation = rotatingOrder(round)[sequence % 3];
    const prefix = `${String(sequence).padStart(2, '0')}-${implementation}`;
    const output = stdout(ticks[implementation], flags[implementation]);
    const stderr = implementation === 'dynarmicNative' ? diagnostics(ticks[implementation]) : '';
    writeFileSync(join(root, `${prefix}.stdout`), output);
    writeFileSync(join(root, `${prefix}.stderr`), stderr);
    const rate = 200000 / (ticks[implementation] / 1e6);
    return {
      sequence, round, implementation, startedAt: 'start', finishedAt: 'finish',
      wallMilliseconds: ticks[implementation] / 1000 + 10, exitCode: 0, signal: null, timedOut: false,
      spawnError: null, stdoutFile: `${prefix}.stdout`, stderrFile: `${prefix}.stderr`,
      metrics: {ticks: ticks[implementation], seconds: ticks[implementation] / 1e6,
        iterationsPerSecond: rate, crc: {seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '4983'},
        durationValid: true, compilerVersion: 'clang test', compilerFlags: flags[implementation]},
      dynarmic: implementation === 'dynarmicNative' ? {memoryMode: 2, guestBytes: 100,
        runMilliseconds: ticks[implementation] / 1000 + 5, haltReason: '01000000'} : null,
      failure: null,
    };
  });
  const config = {
    schema: 'dynarmic.js/native-coremark-comparison-config-v1', startedAt: 'start', iterations: 200000,
    runsPerImplementation: 5, affinity: '0,2,4,6', timeoutMilliseconds: 180000,
    execution: 'fresh processes', timing: 'monotonic', durationPolicy: 'fixed workload',
    commands: {}, plan: [], artifacts, nativeBuild: hostBuild,
    workload: {upstreamRevision: 'a'.repeat(40), dynarmicMemoryMode: 2},
    provenance: {qemuVersion: 'qemu', compiler: 'clang', cpu: 'cpu', uname: 'uname',
      repositoryHead: hostBuild.revisions.repositoryHead,
      dynarmicRevision: hostBuild.revisions.dynarmicRevision,
      nativeHostSource: hostBuild.sources.hostSha256, nativeHostCmake: hostBuild.sources.cmakeSha256},
  };
  writeFileSync(join(root, 'run-config.json'), JSON.stringify(config));
  writeFileSync(join(root, 'native-build-manifest.json'), JSON.stringify(hostBuild));
  writeFileSync(join(root, 'observations.jsonl'), observations.map(item => JSON.stringify(item)).join('\n') + '\n');
  writeFileSync(join(root, 'completion.json'), JSON.stringify({
    schema: 'dynarmic.js/native-coremark-comparison-completion-v1', finishedAt: 'finish',
    observations: observations.length, artifactsVerified: true,
  }));
  return {config, observations};
}

function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'native-coremark-analysis-test-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  return {root, ...createEvidence(root)};
}

test('analysis has its own schema and validates native build provenance', t => {
  const {root} = fixture(t);
  const report = analyzeEvidence(root, join(root, 'analysis.json'));
  assert.equal(report.schema, 'dynarmic.js/native-coremark-comparison-analysis-v1');
  assert.equal(report.runtime.nativeBuild.buildType, 'Release');
  assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), /EEXIST/);
});

for (const [name, change, pattern] of [
  ['truncated dynarmic stderr', root => {
    writeFileSync(join(root, '02-dynarmicNative.stderr'), 'DYNARMIC_MEMORY_MODE=2\n');
  }, /missing\/duplicate/],
  ['missing recorded dynarmic diagnostics', (root, data) => {
    data.observations[2].dynarmic = null;
  }, /dynarmic diagnostics differ/],
  ['tampered recorded Run time', (root, data) => {
    data.observations[2].dynarmic.runMilliseconds = 1;
  }, /dynarmic diagnostics differ/],
  ['unexpected diagnostics on x86', (root, data) => {
    data.observations[0].dynarmic = {memoryMode: 2};
  }, /unexpected dynarmic diagnostics/],
  ['unoptimized native host', (root, data) => {
    data.config.nativeBuild.buildType = 'Debug';
  }, /native build manifest|optimized/],
]) test(`analysis rejects ${name}`, t => {
  const {root, config, observations} = fixture(t);
  const data = {config, observations};
  change(root, data);
  writeFileSync(join(root, 'run-config.json'), JSON.stringify(config));
  writeFileSync(join(root, 'observations.jsonl'), observations.map(item => JSON.stringify(item)).join('\n') + '\n');
  assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), pattern);
});

function rawIntegrity(bytes) {
  return {
    sha256: createHash('sha256').update(bytes).digest('hex'),
    bytes: Buffer.byteLength(bytes),
  };
}

function refreshRawIntegrity(root, observation, stream) {
  const file = stream === 'stdout' ? observation.stdoutFile : observation.stderrFile;
  const bytes = readFileSync(join(root, file));
  const integrity = rawIntegrity(bytes);
  observation[`${stream}Sha256`] = integrity.sha256;
  observation[`${stream}Bytes`] = integrity.bytes;
}

function createV2Evidence(root) {
  const hostBuild = structuredClone(nativeBuild);
  const ticks = {x86Native: 10000000, qemuUser: 200000000, dynarmicNative: 50000000, dynarmicWasm: 100000000};
  const guestCompilerFlags = '-target aarch64-freestanding -O2';
  const flags = {x86Native: 'x86 flags', qemuUser: 'arm flags',
    dynarmicNative: guestCompilerFlags, dynarmicWasm: guestCompilerFlags};
  const artifacts = {
    x86Native: {path: '/x86', sha256: '6'.repeat(64), bytes: 100},
    qemuUser: {path: '/qemu-guest', sha256: '7'.repeat(64), bytes: 100},
    qemuExecutable: {path: '/qemu', sha256: '8'.repeat(64), bytes: 100},
    dynarmicHost: hostBuild.artifact,
    a64Guest: {path: '/guest', sha256: '9'.repeat(64), bytes: 100},
    wasmJs: {path: '/wasm/dynarmic.js', sha256: 'b'.repeat(64), bytes: 300},
    wasmModule: {path: '/wasm/dynarmic.wasm', sha256: 'c'.repeat(64), bytes: 400},
    nodeExecutable: {path: '/node', sha256: 'd'.repeat(64), bytes: 500},
  };
  const wasmBuild = {
    schema: 'dynarmic.js/wasm-coremark-host-build-v1',
    sourceRoot: '/repo',
    build: {type: 'Release', profile: false, a64RegionFocus: '', compilerId: 'Clang',
      compilerVersion: '24.0.0', emscriptenVersion: '6.0.13', cxxFlags: '',
      configCxxFlags: '-O3 -DNDEBUG', linkFlags: '', configLinkFlags: ''},
    configuration: {regionDiagnostics: false, focusTestExceptions: false, examples: true},
    buildEnvironment: {configureEmccCflags: '', buildEmccCflags: ''},
    source: {revision: hostBuild.revisions.repositoryHead,
      upstream: hostBuild.revisions.dynarmicRevision, dirty: false},
    artifacts: {js: artifacts.wasmJs, wasm: artifacts.wasmModule},
    buildSources: {browserCmakeSha256: 'e'.repeat(64), manifestScriptSha256: 'f'.repeat(64)},
  };
  const wasmRuntime = {nodeVersion: 'v24.19.0', v8: 'test-v8', nodeOptions: '',
    executable: artifacts.nodeExecutable};
  const wasmRunner = {sampleSha256: '0'.repeat(64), scopedCpuProfileSha256: '1'.repeat(64),
    environmentSha256: '2'.repeat(64), protocolSha256: '3'.repeat(64), coremarkProtocolSha256: '4'.repeat(64)};
  const observations = Array.from({length: 20}, (_, sequence) => {
    const round = Math.floor(sequence / fourWayImplementations.length);
    const implementation = rotatingFourWayOrder(round)[sequence % fourWayImplementations.length];
    const prefix = `${String(sequence).padStart(2, '0')}-${implementation}`;
    const nestedStdout = stdout(ticks[implementation], flags[implementation]);
    const compile = {blocks: 100 + sequence, bytes: 1000 + sequence, translateMs: 1,
      optMs: 2, emitMs: 3, compileMs: 6, instMs: 0, tableMs: 0};
    const output = implementation === 'dynarmicWasm'
      ? `COREMARK_SAMPLE ${JSON.stringify({haltReason: 0x01000000,
        runMs: ticks[implementation] / 1000 + 5, stdout: nestedStdout, compile, profile: null})}\n`
      : nestedStdout;
    const stderr = implementation === 'dynarmicNative' ? diagnostics(ticks[implementation]) : '';
    const stdoutBytes = Buffer.from(output);
    const stderrBytes = Buffer.from(stderr);
    writeFileSync(join(root, `${prefix}.stdout`), stdoutBytes);
    writeFileSync(join(root, `${prefix}.stderr`), stderrBytes);
    const stdoutIntegrity = rawIntegrity(stdoutBytes);
    const stderrIntegrity = rawIntegrity(stderrBytes);
    const rate = 200000 / (ticks[implementation] / 1e6);
    const plainMetrics = {ticks: ticks[implementation], seconds: ticks[implementation] / 1e6,
      iterationsPerSecond: rate, crc: {seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '4983'},
      durationValid: true, compilerVersion: 'clang test', compilerFlags: flags[implementation]};
    const parsed = implementation === 'dynarmicWasm'
      ? parseWasmCoremarkSample(output, 200000, artifacts.a64Guest.bytes)
      : {metrics: plainMetrics, wasm: null};
    return {
      sequence, round, implementation, startedAt: 'start', finishedAt: 'finish',
      wallMilliseconds: ticks[implementation] / 1000 + 10, exitCode: 0, signal: null, timedOut: false,
      spawnError: null, stdoutFile: `${prefix}.stdout`, stderrFile: `${prefix}.stderr`,
      stdoutSha256: stdoutIntegrity.sha256, stderrSha256: stderrIntegrity.sha256,
      stdoutBytes: stdoutIntegrity.bytes, stderrBytes: stderrIntegrity.bytes,
      metrics: parsed.metrics,
      dynarmic: implementation === 'dynarmicNative' ? {memoryMode: 2, guestBytes: 100,
        runMilliseconds: ticks[implementation] / 1000 + 5, haltReason: '01000000'} : null,
      wasm: parsed.wasm,
      failure: null,
    };
  });
  const config = {
    schema: 'dynarmic.js/native-coremark-comparison-config-v2', startedAt: 'start', iterations: 200000,
    runsPerImplementation: 5, affinity: '0,2,4,6', timeoutMilliseconds: 180000,
    execution: 'fresh processes', timing: 'monotonic', durationPolicy: 'fixed workload',
    commands: {}, plan: [], artifacts, nativeBuild: hostBuild, wasmBuild, wasmRuntime, wasmRunner,
    childEnvironment: {PATH: '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin', LC_ALL: 'C', NODE_OPTIONS: ''},
    workload: {upstreamRevision: 'a'.repeat(40), dynarmicMemoryMode: 2, guestCompilerFlags},
    provenance: {qemuVersion: 'qemu', compiler: 'clang', cpu: 'cpu', uname: 'uname', node: wasmRuntime.nodeVersion,
      repositoryHead: hostBuild.revisions.repositoryHead,
      dynarmicRevision: hostBuild.revisions.dynarmicRevision,
      nativeHostSource: hostBuild.sources.hostSha256, nativeHostCmake: hostBuild.sources.cmakeSha256},
  };
  const manifest = structuredClone(wasmBuild);
  writeFileSync(join(root, 'native-build-manifest.json'), JSON.stringify(hostBuild));
  saveV2Evidence(root, {config, observations, manifest});
  return {config, observations, manifest};
}

function saveV2Evidence(root, data) {
  writeFileSync(join(root, 'run-config.json'), JSON.stringify(data.config));
  writeFileSync(join(root, 'observations.jsonl'), data.observations.map(item => JSON.stringify(item)).join('\n') + '\n');
  writeFileSync(join(root, 'completion.json'), JSON.stringify({
    schema: 'dynarmic.js/native-coremark-comparison-completion-v2', finishedAt: 'finish',
    observations: data.observations.length, artifactsVerified: true,
  }));
  if (!data.omitManifest) writeFileSync(join(root, 'wasm-build-manifest.json'), JSON.stringify(data.manifest));
}

function v2Fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'native-coremark-analysis-v2-test-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  return {root, ...createV2Evidence(root)};
}

function changeBuildAndManifest(data, change) {
  change(data.config.wasmBuild);
  change(data.manifest);
}

function changeWasmWrapper(root, observation, change) {
  const file = join(root, observation.stdoutFile);
  const source = readFileSync(file, 'utf8');
  const sample = JSON.parse(source.slice('COREMARK_SAMPLE '.length));
  change(sample);
  writeFileSync(file, `COREMARK_SAMPLE ${JSON.stringify(sample)}\n`);
  refreshRawIntegrity(root, observation, 'stdout');
}

test('v2 analysis validates four-way Wasm evidence and records build and diagnostics', t => {
  const {root, config, observations, manifest} = v2Fixture(t);
  const output = join(root, 'analysis.json');
  const report = analyzeEvidence(root, output);
  assert.equal(report.schema, 'dynarmic.js/native-coremark-comparison-analysis-v2');
  assert.equal(report.implementations.dynarmicWasm.kernelMedianMs, 100000);
  assert.equal(report.implementations.dynarmicWasm.coreMarkPerSecond, 2000);
  assert.equal(report.ratios.dynarmicWasmOverX86NativeTime, 10);
  assert.equal(report.ratios.dynarmicWasmOverQemuUserTime, 0.5);
  assert.equal(report.ratios.dynarmicWasmOverDynarmicNativeTime, 2);
  assert.deepEqual(report.runtime.wasmBuild, manifest);
  assert.deepEqual(report.runtime.wasmRuntime, config.wasmRuntime);
  assert.deepEqual(report.runtime.wasmRunner, config.wasmRunner);
  assert.deepEqual(report.runtime.childEnvironment, config.childEnvironment);
  assert(!Object.hasOwn(report.runtime.wasmBuild, 'runtime'));
  assert(!Object.hasOwn(report.runtime.wasmBuild, 'sources'));
  assert.deepEqual(report.workload.artifacts, config.artifacts);
  assert.equal(report.methodology.wasmTiming.includes('_arm_run'), true);
  assert.match(report.methodology.wasmTiming, /cold compilation\/publication/);
  assert.match(report.methodology.wasmTiming, /module loading, RAM setup, and JIT construction/);
  assert.match(report.methodology.wasmTiming, /guest kernel.*whole-process wall/);
  assert.match(report.methodology.wasmProvenance, /arm_jit POST_BUILD/);
  assert.match(report.methodology.wasmProvenance, /build-time source, configuration, and artifacts/);
  assert.match(report.methodology.wasmProvenance, /wasmRuntime and wasmRunner are collection-time observations/);
  assert(!JSON.stringify(report.methodology).includes('post-link attestation'));
  assert(!Object.hasOwn(report.implementations.dynarmicWasm, 'compile'));
  report.observations.forEach((summary, index) => {
    assert.deepEqual(summary.wasm, observations[index].wasm);
    if (summary.implementation !== 'dynarmicWasm') assert.equal(summary.wasm, null);
  });
  for (const observation of observations.filter(item => ['dynarmicNative', 'dynarmicWasm'].includes(item.implementation))) {
    assert.equal(observation.metrics.compilerFlags, config.workload.guestCompilerFlags);
  }
  assert.throws(() => analyzeEvidence(root, output), /EEXIST/);
});

for (const [name, change, pattern] of [
  ['nested Wasm stdout CRC', (root, data) => {
    changeWasmWrapper(root, data.observations[3], sample => {
      sample.stdout = sample.stdout.replace('0xe714', '0x1234');
    });
  }, /crclist/],
  ['Wasm wrapper Run time', (root, data) => {
    changeWasmWrapper(root, data.observations[3], sample => { sample.runMs += 1; });
  }, /Wasm diagnostics differ/],
  ['recorded Wasm diagnostics', (root, data) => {
    data.observations[3].wasm.compile.emitMs += 1;
  }, /Wasm diagnostics differ/],
  ['raw stdout digest', (root, data) => {
    const observation = data.observations[0];
    const file = join(root, observation.stdoutFile);
    writeFileSync(file, readFileSync(file, 'utf8').replace('2K performance', '3K performance'));
  }, /stdoutSha256/],
  ['raw stdout byte count', (root, data) => {
    const observation = data.observations[0];
    writeFileSync(join(root, observation.stdoutFile), readFileSync(join(root, observation.stdoutFile), 'utf8') + 'extra\n');
    const bytes = readFileSync(join(root, observation.stdoutFile));
    observation.stdoutSha256 = createHash('sha256').update(bytes).digest('hex');
  }, /stdoutBytes/],
  ['raw stderr digest', (root, data) => {
    const observation = data.observations[0];
    writeFileSync(join(root, observation.stderrFile), 'tampered\n');
  }, /stderrSha256/],
  ['raw stderr byte count', (root, data) => {
    const observation = data.observations[0];
    writeFileSync(join(root, observation.stderrFile), 'tampered\n');
    const bytes = readFileSync(join(root, observation.stderrFile));
    observation.stderrSha256 = createHash('sha256').update(bytes).digest('hex');
  }, /stderrBytes/],
  ['unexpected non-dynarmic stderr', (root, data) => {
    const observation = data.observations[0];
    const bytes = Buffer.from('unrecorded diagnostic\n');
    writeFileSync(join(root, observation.stderrFile), bytes);
    observation.stderrSha256 = createHash('sha256').update(bytes).digest('hex');
    observation.stderrBytes = bytes.length;
  }, /stderr must be empty/],
  ['Wasm diagnostics on a non-Wasm observation', (root, data) => {
    data.observations[0].wasm = {architecture: 'a64'};
  }, /unexpected Wasm diagnostics/],
  ['native diagnostics on a Wasm observation', (root, data) => {
    data.observations[3].dynarmic = {memoryMode: 2};
  }, /unexpected dynarmic diagnostics on Wasm/],
  ['missing Wasm build manifest', (root, data) => {
    data.omitManifest = true;
    rmSync(join(root, 'wasm-build-manifest.json'));
  }, /ENOENT/],
  ['mismatched Wasm build manifest', (root, data) => {
    data.manifest.build.type = 'Debug';
  }, /Wasm build manifest differs/],
]) test(`v2 analysis rejects ${name}`, t => {
  const {root, config, observations, manifest} = v2Fixture(t);
  const data = {config, observations, manifest};
  change(root, data);
  saveV2Evidence(root, data);
  assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), pattern);
});

for (const [name, change, pattern] of [
  ['schema', build => { build.schema = 'wrong'; }, /schema/],
  ['Debug build', build => { build.build.type = 'Debug'; }, /optimized/],
  ['profile build', build => { build.build.profile = true; }, /profile/],
  ['region focus', build => { build.build.a64RegionFocus = '0x1000'; }, /region focus/],
  ['focus macro in global flags', build => {
    build.build.cxxFlags = '-DDYNARMIC_JS_A64_REGION_FOCUS=\\"0x1,0x2,0x3\\"';
  }, /DYNARMIC_JS_A64_REGION_FOCUS/],
  ['focus macro in Release flags', build => {
    build.build.configCxxFlags = '-O3 -D DYNARMIC_JS_A64_REGION_FOCUS=\\"0x1,0x2,0x3\\"';
  }, /DYNARMIC_JS_A64_REGION_FOCUS/],
  ['Xclang focus macro forwarding', build => {
    build.build.cxxFlags = '-Xclang -D -Xclang DYNARMIC_JS_A64_REGION_FOCUS=\\"0x1,0x2,0x3\\"';
  }, /indirect compiler input/],
  ['long profile macro definition', build => {
    build.build.cxxFlags = '--define-macro=DYNARMIC_WASM_PROFILE=0';
  }, /DYNARMIC_WASM_PROFILE/],
  ['long diagnostics macro definition', build => {
    build.build.configCxxFlags = '--define-macro DYNARMIC_WASM_REGION_DIAGNOSTICS=1';
  }, /DYNARMIC_WASM_REGION_DIAGNOSTICS/],
  ['long focus macro definition', build => {
    build.build.cxxFlags = '--define-macro=DYNARMIC_JS_A64_REGION_FOCUS=1';
  }, /DYNARMIC_JS_A64_REGION_FOCUS/],
  ['long forced include', build => {
    build.build.cxxFlags = '--include=/tmp/focus.h';
  }, /indirect compiler input/],
  ['attached forced include', build => {
    build.build.cxxFlags = '-include/tmp/focus.h';
  }, /indirect compiler input/],
  ['quoted attached forced macros', build => {
    build.build.cxxFlags = '"-imacros/tmp/focus.h"';
  }, /indirect compiler input/],
  ['compiler configuration file', build => {
    build.build.cxxFlags = '--config /tmp/focus.cfg';
  }, /indirect compiler input/],
  ['configure EMCC_CFLAGS injection', build => {
    build.buildEnvironment.configureEmccCflags = '-DDYNARMIC_WASM_PROFILE=1';
  }, /EMCC_CFLAGS/],
  ['build EMCC_CFLAGS injection', build => {
    build.buildEnvironment.buildEmccCflags = '-DDYNARMIC_WASM_REGION_DIAGNOSTICS=1';
  }, /EMCC_CFLAGS/],
  ['profile macro in global flags', build => {
    build.build.cxxFlags = '-DDYNARMIC_WASM_PROFILE=1';
  }, /DYNARMIC_WASM_PROFILE/],
  ['diagnostics macro in Release flags', build => {
    build.build.configCxxFlags = '-O3 /DDYNARMIC_WASM_REGION_DIAGNOSTICS';
  }, /DYNARMIC_WASM_REGION_DIAGNOSTICS/],
  ['region diagnostics', build => { build.configuration.regionDiagnostics = true; }, /regionDiagnostics/],
  ['focus test exceptions', build => { build.configuration.focusTestExceptions = true; }, /focusTestExceptions/],
  ['disabled examples', build => { build.configuration.examples = false; }, /examples/],
  ['dirty source', build => { build.source.dirty = true; }, /dirty/],
  ['non-string source root', build => { build.sourceRoot = 42; }, /sourceRoot/],
  ['empty source root', build => { build.sourceRoot = ''; }, /sourceRoot/],
]) test(`v2 analysis rejects ${name} in the Wasm build identity`, t => {
  const {root, config, observations, manifest} = v2Fixture(t);
  const data = {config, observations, manifest};
  changeBuildAndManifest(data, change);
  saveV2Evidence(root, data);
  assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), pattern);
});

for (const key of ['compilerId', 'compilerVersion', 'emscriptenVersion']) {
  for (const [label, change] of [
    ['missing', build => { delete build.build[key]; }],
    ['empty', build => { build.build[key] = ''; }],
    ['non-string', build => { build.build[key] = 42; }],
  ]) test(`v2 analysis rejects ${label} Wasm ${key}`, t => {
    const {root, config, observations, manifest} = v2Fixture(t);
    const data = {config, observations, manifest};
    changeBuildAndManifest(data, change);
    saveV2Evidence(root, data);
    assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), new RegExp(key));
  });
}

for (const key of ['cxxFlags', 'configCxxFlags', 'linkFlags', 'configLinkFlags']) {
  test(`v2 analysis rejects non-string Wasm ${key}`, t => {
    const {root, config, observations, manifest} = v2Fixture(t);
    const data = {config, observations, manifest};
    changeBuildAndManifest(data, build => { build.build[key] = null; });
    saveV2Evidence(root, data);
    assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), new RegExp(key));
  });
}

for (const key of ['browserCmakeSha256', 'manifestScriptSha256']) {
  for (const [label, change] of [
    ['invalid', build => { build.buildSources[key] = 'not-a-sha256'; }],
    ['uppercase', build => { build.buildSources[key] = 'A'.repeat(64); }],
    ['missing', build => { delete build.buildSources[key]; }],
  ]) test(`v2 analysis rejects ${label} Wasm build source hash ${key}`, t => {
    const {root, config, observations, manifest} = v2Fixture(t);
    const data = {config, observations, manifest};
    changeBuildAndManifest(data, change);
    saveV2Evidence(root, data);
    assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), new RegExp(key));
  });
}

for (const key of ['sampleSha256', 'scopedCpuProfileSha256', 'environmentSha256', 'protocolSha256', 'coremarkProtocolSha256']) {
  for (const [label, change] of [
    ['invalid', config => { config.wasmRunner[key] = 'not-a-sha256'; }],
    ['uppercase', config => { config.wasmRunner[key] = 'A'.repeat(64); }],
    ['missing', config => { delete config.wasmRunner[key]; }],
  ]) test(`v2 analysis rejects ${label} Wasm runner hash ${key}`, t => {
    const {root, config, observations, manifest} = v2Fixture(t);
    const data = {config, observations, manifest};
    change(config);
    saveV2Evidence(root, data);
    assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), new RegExp(key));
  });
}

for (const [name, change, pattern] of [
  ['empty node version', config => { config.wasmRuntime.nodeVersion = ''; }, /nodeVersion/],
  ['mismatched node version', config => { config.wasmRuntime.nodeVersion = 'v0.0.0'; }, /nodeVersion/],
  ['empty V8 version', config => { config.wasmRuntime.v8 = ''; }, /v8/],
  ['nonempty node options', config => { config.wasmRuntime.nodeOptions = '--trace-warnings'; }, /nodeOptions/],
  ['mismatched executable', config => {
    config.wasmRuntime.executable = {...config.wasmRuntime.executable, path: '/other-node'};
  }, /nodeExecutable/],
  ['missing executable', config => { delete config.wasmRuntime.executable; }, /nodeExecutable/],
]) test(`v2 analysis rejects ${name} in the Wasm runtime`, t => {
  const {root, config, observations, manifest} = v2Fixture(t);
  const data = {config, observations, manifest};
  change(config);
  saveV2Evidence(root, data);
  assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), pattern);
});

test('v2 analysis rejects injected dynamic-loader state in the child environment', t => {
  const {root, config, observations, manifest} = v2Fixture(t);
  const data = {config, observations, manifest};
  config.childEnvironment.LD_PRELOAD = '/unrecorded.so';
  saveV2Evidence(root, data);
  assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), /child environment/);
});

for (const [name, change, pattern] of [
  ['repository revision', data => {
    changeBuildAndManifest(data, build => { build.source.revision = '5'.repeat(40); });
  }, /repositoryHead/],
  ['dynarmic revision', data => {
    changeBuildAndManifest(data, build => { build.source.upstream = '5'.repeat(40); });
  }, /dynarmicRevision/],
  ['JavaScript artifact', data => {
    data.config.artifacts.wasmJs = {...data.config.artifacts.wasmJs, path: '/other.js'};
  }, /wasmJs/],
  ['Wasm module artifact', data => {
    data.config.artifacts.wasmModule = {...data.config.artifacts.wasmModule, path: '/other.wasm'};
  }, /wasmModule/],
]) test(`v2 analysis rejects mismatched Wasm ${name}`, t => {
  const {root, config, observations, manifest} = v2Fixture(t);
  const data = {config, observations, manifest};
  change(data);
  saveV2Evidence(root, data);
  assert.throws(() => analyzeEvidence(root, join(root, 'analysis.json')), pattern);
});

test('real v1 evidence reanalysis remains byte-for-byte identical', t => {
  const evidence = fileURLToPath(new URL('../evidence', import.meta.url));
  const root = mkdtempSync(join(tmpdir(), 'native-coremark-real-v1-analysis-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  const output = join(root, 'analysis.json');
  analyzeEvidence(evidence, output);
  assert.deepEqual(readFileSync(output), readFileSync(join(evidence, 'analysis.json')));
});
