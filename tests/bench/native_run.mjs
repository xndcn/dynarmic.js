#!/usr/bin/env node
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {performance} from 'node:perf_hooks';
import {hashFile, readBuildInfo, sourceInfo, systemInfo} from './environment.mjs';
import {cases, parseHostOutput} from './protocol.mjs';
import {nativeMethodology, pairOrder, parseNativeOutput, summarizeNativePairs, validateNativeReport} from './native_protocol.mjs';

const readCache = buildDir => Object.fromEntries(fs.readFileSync(path.join(buildDir, 'CMakeCache.txt'), 'utf8')
  .split('\n').map(line => /^([^#/:][^:]*):[^=]+=(.*)$/.exec(line)).filter(Boolean)
  .map(([, key, value]) => [key, value]));

export function parseArgs(args) {
  const [wasmDir, nativeDir, ...options] = args;
  assert(wasmDir && nativeDir && !wasmDir.startsWith('-') && !nativeDir.startsWith('-'),
    'usage: native_run.mjs <wasm-build>/tests <native-build> [--runs 5] [--output report.json]');
  let runs = 5, output = null;
  const seen = new Set();
  for (let i = 0; i < options.length; i += 2) {
    const key = options[i], value = options[i + 1];
    assert(['--runs', '--output'].includes(key) && value && !seen.has(key), 'unknown, duplicate or incomplete option');
    seen.add(key);
    if (key === '--runs') runs = Number(value);
    else output = path.resolve(value);
  }
  assert(Number.isInteger(runs) && runs >= 1 && runs <= 100, '--runs must be 1..100');
  return {wasmDir: path.resolve(wasmDir), nativeDir: path.resolve(nativeDir), runs, output};
}

export function readNativeBuildInfo(buildDir) {
  const cache = readCache(buildDir);
  assert.equal(cache.CMAKE_PROJECT_NAME, 'dynarmic_native_bench', 'use the standalone tests/bench/native CMake project');
  const sourceRoot = cache.DYNARMIC_JS_SOURCE_ROOT;
  assert(sourceRoot, 'missing native source root');
  const compilerDirs = fs.readdirSync(path.join(buildDir, 'CMakeFiles')).map(dir => path.join(buildDir, 'CMakeFiles', dir))
    .filter(dir => fs.existsSync(path.join(dir, 'CMakeCXXCompiler.cmake')));
  assert.equal(compilerDirs.length, 1, 'need one unambiguous native compiler');
  const compilerText = fs.readFileSync(path.join(compilerDirs[0], 'CMakeCXXCompiler.cmake'), 'utf8');
  const systemText = fs.readFileSync(path.join(compilerDirs[0], 'CMakeSystem.cmake'), 'utf8');
  const field = (text, key) => new RegExp(`set\\(${key} "([^"\\n]*)"\\)`).exec(text)?.[1];
  const type = cache.CMAKE_BUILD_TYPE;
  const upstreamStatus = spawnSync('git', ['-C', path.join(sourceRoot, 'externals/dynarmic'),
    'status', '--porcelain', '--ignore-submodules=none', '--untracked-files=all'], {encoding: 'utf8'});
  assert.equal(upstreamStatus.status, 0, 'cannot inspect upstream worktree');
  assert.equal(upstreamStatus.stdout.trim(), '', 'native comparator must use the pristine pinned upstream submodule');
  return {sourceRoot, build: {backend: 'upstream-native', ...sourceInfo(sourceRoot), type,
    upstreamDirty: false,
    architecture: cache.DYNARMIC_NATIVE_BACKEND,
    compilerId: field(compilerText, 'CMAKE_CXX_COMPILER_ID'), compilerVersion: field(compilerText, 'CMAKE_CXX_COMPILER_VERSION'),
    systemProcessor: field(systemText, 'CMAKE_SYSTEM_PROCESSOR'),
    cxxFlags: cache.CMAKE_CXX_FLAGS || '', configCxxFlags: cache[`CMAKE_CXX_FLAGS_${type.toUpperCase()}`] || '',
    linkFlags: cache.CMAKE_EXE_LINKER_FLAGS || '', configLinkFlags: cache[`CMAKE_EXE_LINKER_FLAGS_${type.toUpperCase()}`] || '',
    options: Object.fromEntries(Object.entries(cache).filter(([key]) => key.startsWith('DYNARMIC_') && !key.startsWith('DYNARMIC_JS_'))),
  }};
}

const hashFiles = files => {
  const hash = createHash('sha256');
  for (const file of files) hash.update(fs.readFileSync(file));
  return hash.digest('hex');
};
const workloadHash = (root, spec) => hashFiles([
  path.join(root, 'tests/hosts', spec.file.replace(/\.js$/, '.cpp')),
  path.join(root, 'tests/hosts/benchmark.h'),
]);

export function collectNativeComparison({wasmDir, nativeDir, runs, output}) {
  assert(!process.env.DYN_DUMP_DIR, 'disable DYN_DUMP_DIR during benchmark measurement');
  assert.equal(process.platform, 'linux', 'native comparison currently supports Linux');
  assert(['x64', 'arm64'].includes(process.arch), 'native comparison supports x64 and arm64');
  if (output) {
    assert(!fs.existsSync(output), 'output already exists; retain the original experiment');
    assert(!fs.existsSync(`${output}.failure.json`), 'failure artifact already exists');
  }
  const wasm = readBuildInfo(path.dirname(wasmDir)), native = readNativeBuildInfo(nativeDir);
  const wasmCache = readCache(path.dirname(wasmDir));
  assert.equal(wasmCache.DYNARMIC_JS_REGION_DIAGNOSTICS, 'OFF', 'disable region diagnostics for native comparison');
  assert.equal(wasmCache.DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS, 'OFF', 'disable test-only exception overhead');
  wasm.build.regionDiagnostics = false;
  wasm.build.testExceptions = false;
  assert.equal(wasm.build.type, 'Release', 'use a Release Wasm build');
  assert.equal(wasm.build.profile, false, 'use a production Wasm build');
  assert.equal(wasm.build.a64RegionFocus, '', 'use the default region configuration');
  assert.equal(native.build.type, 'Release', 'use a Release native build');
  const git = sourceInfo(wasm.sourceRoot);
  assert.equal(native.build.upstream, git.upstream, 'native/Wasm upstream revisions differ');
  const executable = (spec, side) => path.join(side === 'wasm' ? wasmDir : nativeDir,
    side === 'wasm' ? spec.file : spec.file.replace(/\.js$/, process.platform === 'win32' ? '.exe' : ''));
  const artifacts = spec => ({nativeSha256: hashFile(executable(spec, 'native')),
    jsSha256: hashFile(executable(spec, 'wasm')),
    wasmSha256: hashFile(executable(spec, 'wasm').replace(/\.js$/, '.wasm'))});
  const results = cases.map(spec => {
    const workloadSha256 = workloadHash(wasm.sourceRoot, spec);
    assert.equal(workloadHash(native.sourceRoot, spec), workloadSha256, 'native/Wasm host sources differ');
    return {name: spec.name, architecture: spec.architecture, memoryMode: spec.memoryMode,
      workloadSha256, artifacts: artifacts(spec), pairs: []};
  });
  const verifyArtifacts = () => cases.forEach((spec, i) => {
    assert.deepEqual(artifacts(spec), results[i].artifacts, 'benchmark executables changed during collection');
    for (const root of [wasm.sourceRoot, native.sourceRoot]) {
      assert.equal(workloadHash(root, spec), results[i].workloadSha256, 'benchmark workload sources changed during collection');
    }
  });
  const report = {schema: 'dynarmic.js/native-comparison-v1', timestamp: new Date().toISOString(),
    ...systemInfo(), git, build: wasm.build, nativeBuild: native.build, methodology: nativeMethodology,
    collectorSha256: hashFiles(['native_run.mjs', 'native_protocol.mjs', 'protocol.mjs', 'environment.mjs']
      .map(file => fileURLToPath(new URL(file, import.meta.url)))),
    runs, qualification: runs >= 5 ? 'comparison' : 'smoke-only', cases: results};
  let active = null;
  try {
    for (let round = 0; round < runs; ++round) {
      for (let offset = 0; offset < cases.length; ++offset) {
        const index = (round + offset) % cases.length, spec = cases[index];
        const pair = {round, order: pairOrder(round, index)};
        for (const side of pair.order) {
          verifyArtifacts();
          active = {case: spec.name, round, side, pair};
          const command = side === 'wasm' ? process.execPath : executable(spec, side);
          const args = side === 'wasm' ? [executable(spec, side), ...spec.argv] : spec.argv;
          const start = performance.now();
          const child = spawnSync(command, args, {encoding: 'utf8', timeout: 60000, maxBuffer: 16 * 1024 * 1024});
          const observation = {exitCode: child.status, signal: child.signal, wallMs: performance.now() - start,
            stdout: child.stdout || '', stderr: child.stderr || ''};
          pair[side] = observation;
          assert(!child.error && child.status === 0 && child.signal === null,
            `${spec.name}/${side}: ${child.error?.message || child.signal || child.stderr || `exit ${child.status}`}`);
          observation.samples = (side === 'native' ? parseNativeOutput : parseHostOutput)(observation.stdout);
        }
        assert.equal(pair.native.samples[0].instructions, pair.wasm.samples[0].instructions, 'native/Wasm instruction counts differ');
        results[index].pairs.push(pair);
        process.stderr.write(`native comparison ${round + 1}/${runs} ${spec.name} PASS\n`);
      }
    }
    verifyArtifacts();
    for (const item of results) item.summary = summarizeNativePairs(item.pairs);
    validateNativeReport(report);
    const json = JSON.stringify(report, null, 2) + '\n';
    if (output) fs.writeFileSync(output, json, {flag: 'wx'});
    else process.stdout.write(json);
    return report;
  } catch (error) {
    if (output) fs.writeFileSync(`${output}.failure.json`, JSON.stringify({
      schema: 'dynarmic.js/native-comparison-failure-v1', error: error.message, active, partial: report,
    }, null, 2) + '\n', {flag: 'wx'});
    throw error;
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try { collectNativeComparison(parseArgs(process.argv.slice(2))); }
  catch (error) { console.error(`Cannot collect native comparison: ${error.message}`); process.exitCode = 2; }
}
