import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {parseCoremark} from '../../../../tests/bench/coremark_protocol.mjs';
import {parseDynarmicDiagnostics, summarizeComparison, summarizeFourWayComparison} from './compare.mjs';
import {parseWasmCoremarkSample} from './wasm.mjs';
import {createBenchmarkEnvironment, validateWasmBuildEnvironment, validateWasmProductionFlags} from './collect_support.mjs';

function validateNativeBuild(config, evidence) {
  const frozen = JSON.parse(readFileSync(`${evidence}/native-build-manifest.json`, 'utf8'));
  assert.deepEqual(frozen, config.nativeBuild, 'native build manifest differs from run configuration');
  assert.equal(frozen.schema, 'dynarmic.js/native-coremark-host-build-v1');
  assert(['Release', 'RelWithDebInfo', 'MinSizeRel'].includes(frozen.buildType), 'native host is not an optimized build');
  assert.deepEqual(frozen.artifact, config.artifacts.dynarmicHost, 'native host artifact identity differs');
  assert.equal(frozen.sources.hostSha256, config.provenance.nativeHostSource);
  assert.equal(frozen.sources.cmakeSha256, config.provenance.nativeHostCmake);
  assert.equal(frozen.revisions.repositoryHead, config.provenance.repositoryHead);
  assert.equal(frozen.revisions.dynarmicRevision, config.provenance.dynarmicRevision);
  for (const value of [frozen.compiler.id, frozen.compiler.version, frozen.compiler.path,
    frozen.flags.cxx, frozen.flags.configuration]) assert(typeof value === 'string', 'incomplete native build identity');
  return frozen;
}

const buildSourceHashKeys = ['browserCmakeSha256', 'manifestScriptSha256'];
const runnerHashKeys = ['sampleSha256', 'scopedCpuProfileSha256', 'environmentSha256',
  'protocolSha256', 'coremarkProtocolSha256'];

function validateHashes(record, keys, label) {
  assert(record !== null && typeof record === 'object' && !Array.isArray(record), `missing ${label}`);
  for (const key of keys) {
    const value = record[key];
    assert(typeof value === 'string' && /^[0-9a-f]{64}$/.test(value), `invalid ${label}.${key}`);
  }
}

function validateWasmBuild(config, evidence) {
  const frozen = JSON.parse(readFileSync(`${evidence}/wasm-build-manifest.json`, 'utf8'));
  assert.deepEqual(frozen, config.wasmBuild, 'Wasm build manifest differs from run configuration');
  assert.deepEqual(Object.keys(frozen).sort(),
    ['schema', 'sourceRoot', 'build', 'configuration', 'buildEnvironment', 'source', 'artifacts', 'buildSources'].sort(),
    'invalid Wasm build manifest shape: schema, sourceRoot, build, configuration, buildEnvironment, source, artifacts, buildSources');
  assert.equal(frozen.schema, 'dynarmic.js/wasm-coremark-host-build-v1', 'invalid Wasm build schema');
  assert(typeof frozen.sourceRoot === 'string' && frozen.sourceRoot.length > 0, 'invalid Wasm sourceRoot');
  assert.equal(frozen.build.type, 'Release', 'Wasm host is not an optimized Release build');
  assert.equal(frozen.build.profile, false, 'Wasm host profile must be disabled');
  assert.equal(frozen.build.a64RegionFocus, '', 'Wasm host region focus must be empty');
  for (const key of ['compilerId', 'compilerVersion', 'emscriptenVersion']) {
    assert(typeof frozen.build[key] === 'string' && frozen.build[key].length > 0, `incomplete Wasm build ${key}`);
  }
  for (const key of ['cxxFlags', 'configCxxFlags', 'linkFlags', 'configLinkFlags']) {
    assert(typeof frozen.build[key] === 'string', `invalid Wasm build ${key}`);
  }
  validateWasmProductionFlags(frozen.build);
  validateWasmBuildEnvironment(frozen.buildEnvironment);
  assert.equal(frozen.configuration.regionDiagnostics, false, 'Wasm host regionDiagnostics must be disabled');
  assert.equal(frozen.configuration.focusTestExceptions, false, 'Wasm host focusTestExceptions must be disabled');
  assert.equal(frozen.configuration.examples, true, 'Wasm host examples must be enabled');
  assert.equal(frozen.source.dirty, false, 'Wasm host source must not be dirty');
  assert.equal(frozen.source.revision, config.provenance.repositoryHead, 'Wasm source revision differs from repositoryHead');
  assert.equal(frozen.source.upstream, config.provenance.dynarmicRevision, 'Wasm upstream revision differs from dynarmicRevision');
  assert.deepEqual(frozen.artifacts.js, config.artifacts.wasmJs, 'Wasm JavaScript artifact differs from wasmJs');
  assert.deepEqual(frozen.artifacts.wasm, config.artifacts.wasmModule, 'Wasm module artifact differs from wasmModule');
  validateHashes(frozen.buildSources, buildSourceHashKeys, 'Wasm buildSources');
  assert.deepEqual(Object.keys(frozen.buildSources).sort(), [...buildSourceHashKeys].sort(), 'invalid Wasm buildSources shape');
  return frozen;
}

function validateWasmRuntime(config) {
  const runtime = config.wasmRuntime;
  assert(runtime !== null && typeof runtime === 'object' && !Array.isArray(runtime), 'missing Wasm runtime');
  assert(typeof runtime.nodeVersion === 'string' && runtime.nodeVersion.length > 0, 'incomplete Wasm runtime nodeVersion');
  assert.equal(runtime.nodeVersion, config.provenance.node, 'Wasm runtime nodeVersion differs from provenance.node');
  assert(typeof runtime.v8 === 'string' && runtime.v8.length > 0, 'incomplete Wasm runtime v8');
  assert.equal(runtime.nodeOptions, '', 'Wasm runtime nodeOptions must be empty');
  assert.deepEqual(runtime.executable, config.artifacts.nodeExecutable, 'Wasm runtime executable differs from nodeExecutable');
  return runtime;
}

function validateWasmRunner(config) {
  validateHashes(config.wasmRunner, runnerHashKeys, 'Wasm runner');
  return config.wasmRunner;
}

const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');

export function analyzeEvidence(evidenceArg, outputArg) {
  const evidence = resolve(evidenceArg);
  const output = resolve(outputArg);
  const config = JSON.parse(readFileSync(`${evidence}/run-config.json`, 'utf8'));
  const isV2 = config.schema === 'dynarmic.js/native-coremark-comparison-config-v2';
  assert(isV2 || config.schema === 'dynarmic.js/native-coremark-comparison-config-v1', 'unsupported run configuration schema');
  const nativeBuild = validateNativeBuild(config, evidence);
  const wasmBuild = isV2 ? validateWasmBuild(config, evidence) : null;
  const wasmRuntime = isV2 ? validateWasmRuntime(config) : null;
  const wasmRunner = isV2 ? validateWasmRunner(config) : null;
  if (isV2) assert.deepEqual(config.childEnvironment, createBenchmarkEnvironment(), 'invalid benchmark child environment');
  const completion = JSON.parse(readFileSync(`${evidence}/completion.json`, 'utf8'));
  assert.equal(completion.schema, `dynarmic.js/native-coremark-comparison-completion-${isV2 ? 'v2' : 'v1'}`);
  assert.equal(completion.artifactsVerified, true);
  const observations = readFileSync(`${evidence}/observations.jsonl`, 'utf8').trim().split('\n').map(line => JSON.parse(line));
  assert.equal(observations.length, completion.observations);
  for (const observation of observations) {
    const prefix = `${String(observation.sequence).padStart(2, '0')}-${observation.implementation}`;
    assert.equal(observation.stdoutFile, `${prefix}.stdout`);
    assert.equal(observation.stderrFile, `${prefix}.stderr`);
    assert.equal(observation.failure, null, observation.failure);
    let stdout;
    if (isV2) {
      const bytes = readFileSync(`${evidence}/${observation.stdoutFile}`);
      assert.equal(observation.stdoutSha256, sha256(bytes), `stdoutSha256 differs for ${prefix}`);
      assert.equal(observation.stdoutBytes, Buffer.byteLength(bytes), `stdoutBytes differs for ${prefix}`);
      stdout = bytes.toString('utf8');
    } else stdout = readFileSync(`${evidence}/${observation.stdoutFile}`, 'utf8');
    const isWasm = isV2 && observation.implementation === 'dynarmicWasm';
    const parsed = isWasm
      ? parseWasmCoremarkSample(stdout, config.iterations, config.artifacts.a64Guest.bytes)
      : {metrics: parseCoremark(stdout, config.iterations), wasm: null};
    const metrics = parsed.metrics;
    if (isV2 && ['dynarmicNative', 'dynarmicWasm'].includes(observation.implementation)) {
      assert.equal(metrics.compilerFlags, config.workload.guestCompilerFlags, `guest compiler flags differ for ${prefix}`);
    }
    assert.equal(metrics.crc.crcfinal, '4983');
    assert.deepEqual(observation.metrics, metrics, `parsed metrics differ for ${prefix}`);
    if (isV2) {
      if (isWasm) assert.deepEqual(observation.wasm, parsed.wasm, `Wasm diagnostics differ for ${prefix}`);
      else assert.equal(observation.wasm, null, `unexpected Wasm diagnostics for ${prefix}`);
    }
    let stderr;
    if (isV2) {
      const bytes = readFileSync(`${evidence}/${observation.stderrFile}`);
      assert.equal(observation.stderrSha256, sha256(bytes), `stderrSha256 differs for ${prefix}`);
      assert.equal(observation.stderrBytes, Buffer.byteLength(bytes), `stderrBytes differs for ${prefix}`);
      stderr = bytes.toString('utf8');
    } else stderr = readFileSync(`${evidence}/${observation.stderrFile}`, 'utf8');
    if (observation.implementation !== 'dynarmicNative') {
      assert.equal(stderr.length, 0, `${observation.implementation} stderr must be empty`);
    }
    if (observation.implementation === 'dynarmicNative') {
      const dynarmic = parseDynarmicDiagnostics(stderr);
      assert.deepEqual(observation.dynarmic, dynarmic, 'dynarmic diagnostics differ from raw stderr');
      assert.equal(dynarmic.guestBytes, config.artifacts.a64Guest.bytes, 'dynarmic guest size differs');
      assert(dynarmic.runMilliseconds + 0.01 >= metrics.ticks / 1000, 'kernel time exceeds dynarmic Run');
      assert(observation.wallMilliseconds + 0.01 >= dynarmic.runMilliseconds, 'dynarmic Run exceeds process wall time');
    } else if (isWasm) assert.equal(observation.dynarmic, null, 'unexpected dynarmic diagnostics on Wasm');
    else assert.equal(observation.dynarmic, null, 'unexpected dynarmic diagnostics');
  }
  const summary = isV2 ? summarizeFourWayComparison(observations, config) : summarizeComparison(observations, config);
  const report = {
    ...summary,
    schema: `dynarmic.js/native-coremark-comparison-analysis-${isV2 ? 'v2' : 'v1'}`,
    startedAt: config.startedAt,
    finishedAt: completion.finishedAt,
    workload: {
      iterations: config.iterations,
      upstreamRevision: config.workload.upstreamRevision,
      dynarmicMemoryMode: config.workload.dynarmicMemoryMode,
      artifacts: config.artifacts,
    },
    runtime: {
      affinity: config.affinity,
      qemuVersion: config.provenance.qemuVersion,
      compiler: config.provenance.compiler,
      cpu: config.provenance.cpu,
      uname: config.provenance.uname,
      repositoryHead: config.provenance.repositoryHead,
      dynarmicRevision: config.provenance.dynarmicRevision,
      nativeBuild,
      ...(isV2 ? {wasmBuild, wasmRuntime, wasmRunner, childEnvironment: config.childEnvironment} : {}),
    },
    methodology: {
      execution: config.execution,
      timing: config.timing,
      aggregation: 'per-implementation median guest ticks; ratios of medians; MAD and range retained',
      crc: 'every sample must have all expected individual CRCs and accumulated crcfinal 0x4983',
      duration: config.durationPolicy,
      workloadEquivalence: 'qemu-user runs an AArch64 Linux-musl HOST build; dynarmic runs an AArch64 freestanding build; matching CRCs do not imply identical instruction streams',
      certification: 'fixed-workload end-to-end throughput comparison, not an official CoreMark score or a pure engine-only overhead measurement',
      ...(isV2 ? {
        wasmTiming: 'Wasm runMs surrounds only _arm_run and includes cold compilation/publication; it excludes module loading, RAM setup, and JIT construction, and is not interchangeable with guest kernel ticks or whole-process wall time',
        wasmProvenance: 'the Wasm manifest is generated by arm_jit POST_BUILD and binds build-time source, configuration, and artifacts; wasmRuntime and wasmRunner are collection-time observations',
      } : {}),
    },
    observations: observations.map(observation => ({
      sequence: observation.sequence,
      round: observation.round,
      implementation: observation.implementation,
      ticks: observation.metrics.ticks,
      coreMarkPerSecond: observation.metrics.iterationsPerSecond,
      wallMilliseconds: observation.wallMilliseconds,
      durationValid: observation.metrics.durationValid,
      dynarmic: observation.dynarmic,
      ...(isV2 ? {wasm: observation.wasm} : {}),
    })),
  };
  writeFileSync(output, `${JSON.stringify(report, null, 2)}\n`, {flag: 'wx'});
  return report;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [evidence, output] = process.argv.slice(2);
  if (!output || process.argv.length !== 4) {
    console.error('usage: node analyze.mjs EVIDENCE_DIR NEW_ANALYSIS.json');
    process.exit(64);
  }
  const report = analyzeEvidence(evidence, output);
  console.log(JSON.stringify({implementations: report.implementations, ratios: report.ratios,
    durationQualified: report.durationQualified}, null, 2));
}
