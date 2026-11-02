// Shared benchmark provenance. These describe configured builds and observed
// executables; they are not a build-time source attestation.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const hashFile = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
export function readBuildInfo(buildDir) {
  const cache = Object.fromEntries(fs.readFileSync(path.join(buildDir, 'CMakeCache.txt'), 'utf8')
    .split('\n').map(line => /^([^#/:][^:]*):[^=]+=(.*)$/.exec(line)).filter(Boolean)
    .map(([, key, value]) => [key, value]));
  const sourceRoot = cache.CMAKE_HOME_DIRECTORY;
  assert(sourceRoot, 'configured source directory is missing');
  assert(Object.hasOwn(cache, 'DYNARMIC_JS_A64_REGION_FOCUS'), 'missing DYNARMIC_JS_A64_REGION_FOCUS CMakeCache entry');
  const compilerFiles = fs.readdirSync(path.join(buildDir, 'CMakeFiles'))
    .map(dir => path.join(buildDir, 'CMakeFiles', dir, 'CMakeCXXCompiler.cmake')).filter(fs.existsSync);
  assert.equal(compilerFiles.length, 1, 'need one unambiguous configured compiler');
  const compilerText = fs.readFileSync(compilerFiles[0], 'utf8');
  const compiler = key => new RegExp(`set\\(${key} "([^"\\n]*)"\\)`).exec(compilerText)?.[1];
  assert(['ON', 'OFF'].includes(cache.DYNARMIC_JS_PROFILE), 'unknown profiling configuration');
  const type = cache.CMAKE_BUILD_TYPE;
  assert(type, 'missing build type');
  const instrumentation = {};
  for (const [key, field] of [
    ['DYNARMIC_JS_REGION_DIAGNOSTICS', 'regionDiagnostics'],
    ['DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS', 'testExceptions'],
  ]) {
    // Older fixtures/archives predate these switches. Never infer a value for
    // an absent field; present configuration participates in compatibility.
    if (Object.hasOwn(cache, key)) {
      assert(['ON', 'OFF'].includes(cache[key]), `unknown ${key} configuration`);
      instrumentation[field] = cache[key] === 'ON';
    }
  }
  return {sourceRoot, build: {
    type, profile: cache.DYNARMIC_JS_PROFILE === 'ON', a64RegionFocus: cache.DYNARMIC_JS_A64_REGION_FOCUS,
    ...instrumentation,
    compilerId: compiler('CMAKE_CXX_COMPILER_ID'), compilerVersion: compiler('CMAKE_CXX_COMPILER_VERSION'),
    emscriptenVersion: fs.readFileSync(path.join(path.dirname(compiler('CMAKE_CXX_COMPILER')), 'emscripten-version.txt'), 'utf8').trim(),
    cxxFlags: cache.CMAKE_CXX_FLAGS || '', configCxxFlags: cache[`CMAKE_CXX_FLAGS_${type.toUpperCase()}`] || '',
    linkFlags: cache.CMAKE_EXE_LINKER_FLAGS || '', configLinkFlags: cache[`CMAKE_EXE_LINKER_FLAGS_${type.toUpperCase()}`] || '',
  }};
}
export function sourceInfo(sourceRoot) {
  assert(fs.existsSync(path.join(sourceRoot, 'externals/dynarmic/.git')), 'uninitialized upstream submodule');
  const git = args => {
    const r = spawnSync('git', args, {cwd: sourceRoot, encoding: 'utf8'});
    assert.equal(r.status, 0, r.stderr || r.error?.message);
    return r.stdout.trim();
  };
  return {revision: git(['rev-parse', 'HEAD']), upstream: git(['-C', 'externals/dynarmic', 'rev-parse', 'HEAD']),
    dirty: Boolean(git(['status', '--porcelain', '--ignore-submodules=none', '--untracked-files=all']))};
}
export function systemInfo() {
  return {node: process.version, v8: process.versions.v8,
    runtime: {nodeOptions: process.env.NODE_OPTIONS || '', cpuAffinity: process.platform === 'linux'
      ? /^Cpus_allowed_list:\s*(.*)$/m.exec(fs.readFileSync('/proc/self/status', 'utf8'))?.[1] || 'unknown' : 'unavailable'},
    host: {platform: process.platform, release: os.release(), arch: process.arch,
      cpu: os.cpus()[0]?.model, cpus: os.cpus().length}};
}

export function validateEnvironment(report) {
  const text = (value, name) => assert(typeof value === 'string' && value.length > 0, `missing ${name}`);
  assert(Number.isFinite(Date.parse(report.timestamp)), 'invalid timestamp');
  for (const key of ['node', 'v8']) text(report[key], key);
  assert.equal(typeof report.runtime?.nodeOptions, 'string', 'missing runtime NODE_OPTIONS');
  if (report.runtime.cpuAffinity !== undefined) text(report.runtime.cpuAffinity, 'CPU affinity');
  for (const key of ['platform', 'release', 'arch', 'cpu']) text(report.host?.[key], `host.${key}`);
  assert(Number.isSafeInteger(report.host.cpus) && report.host.cpus > 0);
  for (const key of ['revision', 'upstream']) assert(/^[a-f0-9]{40}$/.test(report.git?.[key]), `invalid git.${key}`);
  assert.equal(typeof report.git.dirty, 'boolean');
  for (const key of ['type', 'compilerId', 'compilerVersion', 'emscriptenVersion']) text(report.build?.[key], `build.${key}`);
  assert.equal(typeof report.build.profile, 'boolean');
  for (const field of ['regionDiagnostics', 'testExceptions']) {
    if (Object.hasOwn(report.build, field)) assert.equal(typeof report.build[field], 'boolean', `invalid build.${field}`);
  }
  if (Object.hasOwn(report.build, 'a64RegionFocus')) {
    const focus = report.build.a64RegionFocus;
    assert.equal(typeof focus, 'string', 'invalid build.a64RegionFocus');
    if (focus !== '') {
      const descriptors = focus.split(',');
      assert(descriptors.length >= 3 && descriptors.every(value => /^0x[0-9a-f]{16}$/.test(value)),
        'invalid build.a64RegionFocus');
      for (let i = 3; i < descriptors.length; ++i) {
        assert(BigInt(descriptors[i]) > BigInt(descriptors[i - 1]),
          'build.a64RegionFocus return descriptors must strictly increase');
      }
    }
  }
  for (const key of ['cxxFlags', 'configCxxFlags', 'linkFlags', 'configLinkFlags']) assert.equal(typeof report.build[key], 'string');
}
