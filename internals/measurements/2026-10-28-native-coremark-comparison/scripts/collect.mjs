import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {appendFileSync, existsSync, mkdirSync, readFileSync, statSync, writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {spawnSync, execFileSync} from 'node:child_process';
import os from 'node:os';
import {performance} from 'node:perf_hooks';
import {fileURLToPath} from 'node:url';
import {parseCoremark, validateCoremarkBuild} from '../../../../tests/bench/coremark_protocol.mjs';
import {readBuildInfo, sourceInfo} from '../../../../tests/bench/environment.mjs';
import {fourWayImplementations, implementations, parseDynarmicDiagnostics, rotatingFourWayOrder,
  rotatingOrder} from './compare.mjs';
import {parseWasmCoremarkSample} from './wasm.mjs';
import {createBenchmarkEnvironment, parseCollectArguments, runWithFailureEvidence, streamIdentity, validateWasmBuildManifest,
  validateWasmProductionBuild} from './collect_support.mjs';

const [manifestArg, hostArg, guestArg, outputArg] = process.argv.slice(2);
let parsedArguments;
try {
  parsedArguments = parseCollectArguments(process.argv.slice(2));
} catch (error) {
  console.error(error.message);
  process.exit(64);
}
const {runs, wasmBuildDir: wasmBuildArg} = parsedArguments;
const fourWay = wasmBuildArg !== null;
const cohort = fourWay ? fourWayImplementations : implementations;
const orderFor = fourWay ? rotatingFourWayOrder : rotatingOrder;
const manifestPath = resolve(manifestArg);
const hostPath = resolve(hostArg);
const guestPath = resolve(guestArg);
const output = resolve(outputArg);
const wasmBuildDir = fourWay ? resolve(wasmBuildArg) : null;
assert(!existsSync(output), `refusing to overwrite ${output}`);
mkdirSync(output, {recursive: false});
const qemuPath = '/usr/bin/qemu-aarch64-static';
const affinity = '0,2,4,6';
const timeoutMilliseconds = 180000;
const sha256 = path => createHash('sha256').update(readFileSync(path)).digest('hex');
const artifact = path => ({path, sha256: sha256(path), bytes: statSync(path).size});
const execute = (command, args) => execFileSync(command, args, {encoding: 'utf8'}).trim();
const scriptPath = fileURLToPath(import.meta.url);
const scriptDirectory = resolve(scriptPath, '..');
const repository = resolve(scriptDirectory, '../../../..');
const linux = JSON.parse(readFileSync(manifestPath, 'utf8'));
assert.equal(linux.schema, 'dynarmic.js/native-coremark-linux-build-v1');
const base = JSON.parse(readFileSync(linux.baseManifest.path, 'utf8'));
validateCoremarkBuild(base);
assert.equal(sha256(linux.baseManifest.path), linux.baseManifest.sha256);
assert.equal(linux.iterations, base.iterations);
for (const implementation of ['x86Native', 'qemuUser']) {
  const record = linux.artifacts[implementation];
  assert.equal(sha256(record.path), record.sha256, `${implementation} binary changed`);
}
assert.equal(sha256(guestPath), base.artifacts.a64.sha256, 'A64 guest does not match pinned workload');

function readWasmFeatures(buildDir) {
  const cache = Object.fromEntries(readFileSync(resolve(buildDir, 'CMakeCache.txt'), 'utf8')
    .split('\n').map(line => /^([^#/:][^:]*):[^=]+=(.*)$/.exec(line)).filter(Boolean)
    .map(([, key, value]) => [key, value]));
  assert.equal(cache.DYNARMIC_JS_REGION_DIAGNOSTICS, 'OFF', 'region diagnostics must be OFF');
  assert.equal(cache.DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS, 'OFF', 'focus test exceptions must be OFF');
  assert.equal(cache.DYNARMIC_JS_EXAMPLES, 'ON', 'browser examples must be ON');
  return {regionDiagnostics: false, focusTestExceptions: false, examples: true};
}

const wasmInfo = fourWay ? readBuildInfo(wasmBuildDir) : null;
const wasmFeatures = fourWay ? readWasmFeatures(wasmBuildDir) : null;
const wasmSource = fourWay ? sourceInfo(wasmInfo.sourceRoot) : null;
if (fourWay) {
  validateWasmProductionBuild(wasmInfo, wasmFeatures, wasmSource);
  assert.equal(resolve(wasmInfo.sourceRoot), repository, 'Wasm build uses a different source checkout');
}
const wasmPaths = fourWay ? {
  js: resolve(wasmBuildDir, 'examples/browser/dist/arm_jit.js'),
  module: resolve(wasmBuildDir, 'examples/browser/dist/arm_jit.wasm'),
  sample: resolve(repository, 'tests/bench/coremark/sample.mjs'),
} : null;

const artifacts = {
  x86Native: artifact(linux.artifacts.x86Native.path),
  qemuUser: artifact(linux.artifacts.qemuUser.path),
  qemuExecutable: artifact(qemuPath),
  dynarmicHost: artifact(hostPath),
  a64Guest: artifact(guestPath),
  ...(fourWay ? {wasmJs: artifact(wasmPaths.js), wasmModule: artifact(wasmPaths.module),
    nodeExecutable: artifact(process.execPath)} : {}),
};
const nativeBuildPath = resolve(hostPath, '..', 'native-build-manifest.json');
const nativeBuildText = readFileSync(nativeBuildPath, 'utf8');
const nativeBuild = JSON.parse(nativeBuildText);
assert.equal(nativeBuild.schema, 'dynarmic.js/native-coremark-host-build-v1');
assert(['Release', 'RelWithDebInfo', 'MinSizeRel'].includes(nativeBuild.buildType), 'native host is not optimized');
assert.deepEqual(nativeBuild.artifact, artifacts.dynarmicHost, 'native host does not match its build manifest');
assert.equal(nativeBuild.sources.hostSha256, sha256(resolve(scriptDirectory, '../native/a64_coremark_host.cpp')), 'native host source differs from build');
assert.equal(nativeBuild.sources.cmakeSha256, sha256(resolve(scriptDirectory, '../native/CMakeLists.txt')), 'native CMake source differs from build');
assert.equal(nativeBuild.sources.manifestScriptSha256, sha256(resolve(scriptDirectory, '../native/write_build_manifest.cmake')), 'native manifest generator differs from build');
const repositoryHead = execute('git', ['-C', repository, 'rev-parse', 'HEAD']);
const dynarmicRevision = execute('git', ['-C', `${repository}/externals/dynarmic`, 'rev-parse', 'HEAD']);
assert.equal(nativeBuild.revisions.repositoryHead, repositoryHead, 'repository revision differs from native build');
assert.equal(nativeBuild.revisions.dynarmicRevision, dynarmicRevision, 'dynarmic revision differs from native build');
if (fourWay) {
  assert.equal(wasmSource.revision, repositoryHead, 'Wasm source revision differs from repository');
  assert.equal(wasmSource.upstream, dynarmicRevision, 'Wasm upstream revision differs from dynarmic');
}
writeFileSync(`${output}/native-build-manifest.json`, nativeBuildText, {flag: 'wx'});
const wasmSourcePaths = fourWay ? {
  sampleSha256: wasmPaths.sample,
  scopedCpuProfileSha256: resolve(repository, 'tests/bench/scoped_cpu_profile.mjs'),
  environmentSha256: resolve(repository, 'tests/bench/environment.mjs'),
  protocolSha256: resolve(repository, 'tests/bench/protocol.mjs'),
  coremarkProtocolSha256: resolve(repository, 'tests/bench/coremark_protocol.mjs'),
} : null;
const wasmBuildPath = fourWay ? resolve(wasmBuildDir, 'examples/browser/dist/wasm-build-manifest.json') : null;
const wasmBuildText = fourWay ? readFileSync(wasmBuildPath, 'utf8') : null;
const wasmBuild = fourWay ? JSON.parse(wasmBuildText) : null;
const wasmBuildSources = fourWay ? {
  browserCmakeSha256: sha256(resolve(repository, 'examples/browser/CMakeLists.txt')),
  manifestScriptSha256: sha256(resolve(repository, 'examples/browser/write_build_manifest.cmake')),
} : null;
if (fourWay) validateWasmBuildManifest(wasmBuild, {
  info: wasmInfo, features: wasmFeatures, source: wasmSource,
  artifacts: {js: artifacts.wasmJs, wasm: artifacts.wasmModule}, buildSources: wasmBuildSources,
});
const wasmRuntime = fourWay ? {
  nodeVersion: process.version, v8: process.versions.v8, nodeOptions: '', executable: artifacts.nodeExecutable,
} : null;
const wasmRunner = fourWay ? Object.fromEntries(Object.entries(wasmSourcePaths).map(([key, path]) => [key, sha256(path)])) : null;
if (fourWay) writeFileSync(`${output}/wasm-build-manifest.json`, wasmBuildText, {flag: 'wx'});
const plan = Array.from({length: runs * cohort.length}, (_, sequence) => {
  const round = Math.floor(sequence / cohort.length);
  return {sequence, round, implementation: orderFor(round)[sequence % cohort.length]};
});
const commands = {
  x86Native: [artifacts.x86Native.path],
  qemuUser: [qemuPath, artifacts.qemuUser.path],
  dynarmicNative: [hostPath, guestPath, '2'],
  ...(fourWay ? {dynarmicWasm: [process.execPath, wasmPaths.sample, wasmPaths.js, guestPath, 'a64', '2']} : {}),
};
const scriptNames = ['collect.mjs', 'analyze.mjs', 'compare.mjs', 'build_linux_coremark.py',
  ...(fourWay ? ['collect_support.mjs', 'wasm.mjs'] : [])];
const configuration = {
  schema: `dynarmic.js/native-coremark-comparison-config-v${fourWay ? 2 : 1}`,
  startedAt: new Date().toISOString(),
  iterations: linux.iterations,
  runsPerImplementation: runs,
  affinity,
  timeoutMilliseconds,
  execution: `one fresh process per sample; ${fourWay ? 'four' : 'three'} implementations rotate within each round`,
  timing: 'CoreMark CLOCK_MONOTONIC wrapping-u32 microseconds around iterate; separate whole-process wall time',
  durationPolicy: 'fixed 200000-iteration comparison; samples below ten seconds are retained but not duration-qualified',
  commands,
  plan,
  artifacts,
  nativeBuild,
  ...(fourWay ? {wasmBuild, wasmRuntime, wasmRunner, childEnvironment: createBenchmarkEnvironment()} : {}),
  workload: {linuxManifest: artifact(manifestPath), baseManifest: artifact(linux.baseManifest.path),
    upstreamRevision: base.revision, dynarmicMemoryMode: 2, guestLoadAddress: '0x10000',
    ...(fourWay ? {guestCompilerFlags: base.flags.a64} : {})},
  provenance: {
    repositoryHead,
    dynarmicRevision,
    qemuVersion: execute(qemuPath, ['--version']).split('\n')[0],
    compiler: linux.compiler,
    node: process.version,
    uname: execute('uname', ['-a']),
    cpu: os.cpus()[0]?.model ?? 'unknown',
    logicalCpus: os.cpus().length,
    scripts: Object.fromEntries(scriptNames.map(name => [name, sha256(resolve(scriptDirectory, name))])),
    nativeHostSource: sha256(resolve(scriptDirectory, '../native/a64_coremark_host.cpp')),
    nativeHostCmake: sha256(resolve(scriptDirectory, '../native/CMakeLists.txt')),
  },
};
writeFileSync(`${output}/run-config.json`, `${JSON.stringify(configuration, null, 2)}\n`, {flag: 'wx'});
const hostSnapshot = label => [
  label, new Date().toISOString(), configuration.provenance.uname,
  `loadavg: ${os.loadavg().join(' ')}`,
  execute('ps', ['-eo', 'pid,comm,%cpu,%mem', '--sort=-%cpu']).split('\n').slice(0, 16).join('\n'), '',
].join('\n');
writeFileSync(`${output}/host-before.txt`, hostSnapshot('before'), {flag: 'wx'});

let sequence = 0;
const childEnvironment = createBenchmarkEnvironment();
runWithFailureEvidence(output, () => {
  for (const item of plan) {
    const implementation = item.implementation;
    const prefix = `${String(sequence).padStart(2, '0')}-${implementation}`;
    const args = implementation === 'x86Native' ? [] : commands[implementation].slice(1);
    const startedAt = new Date().toISOString();
    const start = performance.now();
    const child = spawnSync('taskset', ['-c', affinity, commands[implementation][0], ...args], {
      encoding: 'buffer', timeout: timeoutMilliseconds, maxBuffer: 4 * 1024 * 1024,
      env: childEnvironment,
    });
    const wallMilliseconds = performance.now() - start;
    const stdoutBytes = child.stdout ?? Buffer.alloc(0);
    const stderrBytes = child.stderr ?? Buffer.alloc(0);
    const stdout = stdoutBytes.toString('utf8');
    const stderr = stderrBytes.toString('utf8');
    const observation = {
      ...item, startedAt, finishedAt: new Date().toISOString(), wallMilliseconds,
      exitCode: child.status, signal: child.signal, timedOut: child.error?.code === 'ETIMEDOUT',
      spawnError: child.error ? String(child.error) : null,
      stdoutFile: `${prefix}.stdout`, stderrFile: `${prefix}.stderr`,
      ...(fourWay ? {stdoutSha256: streamIdentity(stdoutBytes).sha256,
        stderrSha256: streamIdentity(stderrBytes).sha256,
        stdoutBytes: stdoutBytes.length, stderrBytes: stderrBytes.length} : {}),
      metrics: null, dynarmic: null, ...(fourWay ? {wasm: null} : {}), failure: null,
    };
    writeFileSync(`${output}/${observation.stdoutFile}`, stdoutBytes, {flag: 'wx'});
    writeFileSync(`${output}/${observation.stderrFile}`, stderrBytes, {flag: 'wx'});
    try {
      assert.equal(child.error, undefined, `spawn error: ${child.error}`);
      assert.equal(child.status, 0, `nonzero exit: ${child.status}`);
      assert.equal(child.signal, null, `terminated by ${child.signal}`);
      if (implementation !== 'dynarmicNative') assert.equal(stderrBytes.length, 0, `${implementation} stderr must be empty`);
      if (implementation === 'dynarmicWasm') {
        const parsed = parseWasmCoremarkSample(stdout, linux.iterations, artifacts.a64Guest.bytes);
        observation.metrics = parsed.metrics;
        observation.wasm = parsed.wasm;
        assert.equal(observation.metrics.compilerFlags, base.flags.a64, 'guest compiler flags changed');
        assert(observation.wasm.runMilliseconds + 0.01 >= observation.metrics.ticks / 1000, 'kernel exceeds Wasm Run');
        assert(wallMilliseconds + 0.01 >= observation.wasm.runMilliseconds, 'Wasm Run exceeds process wall');
      } else {
        observation.metrics = parseCoremark(stdout, linux.iterations);
        if (implementation === 'dynarmicNative') {
          observation.dynarmic = parseDynarmicDiagnostics(stderr);
          assert.equal(observation.dynarmic.guestBytes, artifacts.a64Guest.bytes);
          assert(observation.dynarmic.runMilliseconds + 0.01 >= observation.metrics.ticks / 1000, 'kernel exceeds Run');
          assert(wallMilliseconds + 0.01 >= observation.dynarmic.runMilliseconds, 'Run exceeds process wall');
          if (fourWay) assert.equal(observation.metrics.compilerFlags, base.flags.a64, 'guest compiler flags changed');
        } else {
          assert.equal(observation.metrics.compilerFlags, linux.artifacts[implementation].flags, 'compiler flags changed');
        }
      }
      assert.equal(observation.metrics.crc.crcfinal, '4983', 'wrong final CRC');
    } catch (error) {
      observation.failure = error.stack;
    }
    appendFileSync(`${output}/observations.jsonl`, `${JSON.stringify(observation)}\n`, {flag: 'a'});
    if (observation.failure) throw new Error(`sample ${sequence} failed; evidence retained in ${output}\n${observation.failure}`);
    console.log(`${prefix}: ${observation.metrics.ticks / 1e6} s, ${observation.metrics.iterationsPerSecond} CoreMark/s, wall ${wallMilliseconds.toFixed(3)} ms`);
    sequence++;
  }
  for (const [name, record] of Object.entries(artifacts)) assert.equal(sha256(record.path), record.sha256, `${name} changed during cohort`);
  for (const [name, record] of Object.entries(configuration.workload)) {
    if (record && typeof record === 'object' && record.path) assert.equal(sha256(record.path), record.sha256, `${name} changed during cohort`);
  }
  if (fourWay) {
    assert.deepEqual(readBuildInfo(wasmBuildDir), wasmInfo, 'Wasm build configuration changed during cohort');
    assert.deepEqual(readWasmFeatures(wasmBuildDir), wasmFeatures, 'Wasm feature configuration changed during cohort');
    assert.deepEqual(sourceInfo(wasmInfo.sourceRoot), wasmSource, 'Wasm source observation changed during cohort');
    assert.equal(readFileSync(wasmBuildPath, 'utf8'), wasmBuildText, 'Wasm post-link manifest changed during cohort');
    validateWasmBuildManifest(JSON.parse(wasmBuildText), {
      info: readBuildInfo(wasmBuildDir), features: readWasmFeatures(wasmBuildDir), source: sourceInfo(wasmInfo.sourceRoot),
      artifacts: {js: artifact(wasmPaths.js), wasm: artifact(wasmPaths.module)}, buildSources: {
        browserCmakeSha256: sha256(resolve(repository, 'examples/browser/CMakeLists.txt')),
        manifestScriptSha256: sha256(resolve(repository, 'examples/browser/write_build_manifest.cmake')),
      },
    });
    for (const [key, path] of Object.entries(wasmSourcePaths)) assert.equal(sha256(path), wasmRunner[key], `${key} changed during cohort`);
  }
  writeFileSync(`${output}/host-after.txt`, hostSnapshot('after'), {flag: 'wx'});
  writeFileSync(`${output}/completion.json`, `${JSON.stringify({schema: `dynarmic.js/native-coremark-comparison-completion-v${fourWay ? 2 : 1}`,
    finishedAt: new Date().toISOString(), observations: sequence, artifactsVerified: true}, null, 2)}\n`, {flag: 'wx'});
});
