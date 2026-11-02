import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {hashFile, readBuildInfo, sourceInfo, systemInfo, validateEnvironment} from './environment.mjs';
import {report} from './fixtures.mjs';

test('configured build provenance resolves compiler and flags without invoking a compiler', t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bench-env-'));
  t.after(() => fs.rmSync(dir, {recursive: true, force: true}));
  fs.mkdirSync(path.join(dir, 'CMakeFiles/1'), {recursive: true});
  fs.writeFileSync(path.join(dir, 'emscripten-version.txt'), '6.test\n');
  const cache = `CMAKE_HOME_DIRECTORY:INTERNAL=/configured/source
DYNARMIC_JS_PROFILE:BOOL=ON
DYNARMIC_JS_A64_REGION_FOCUS:STRING=
CMAKE_BUILD_TYPE:STRING=Release
CMAKE_CXX_FLAGS:STRING=-Wall
CMAKE_CXX_FLAGS_RELEASE:STRING=-O3
CMAKE_EXE_LINKER_FLAGS:STRING=-sALLOW_MEMORY_GROWTH=1
CMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=
`;
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'), cache);
  fs.writeFileSync(path.join(dir, 'CMakeFiles/1/CMakeCXXCompiler.cmake'),
    `set(CMAKE_CXX_COMPILER "${dir}/em++")\nset(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "22.test")\n`);
  const {sourceRoot, build} = readBuildInfo(dir);
  assert.equal(sourceRoot, '/configured/source');
  assert.deepEqual(build, {type: 'Release', profile: true, a64RegionFocus: '', compilerId: 'Clang', compilerVersion: '22.test',
    emscriptenVersion: '6.test', cxxFlags: '-Wall', configCxxFlags: '-O3', linkFlags: '-sALLOW_MEMORY_GROWTH=1', configLinkFlags: ''});
  const r = report(); r.build = build; validateEnvironment(r);
  const focus = '0x0000000000001000,0x0000000000001004,0x0000000000002000,0x0000000000003000';
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'),
    cache.replace('DYNARMIC_JS_A64_REGION_FOCUS:STRING=\n', `DYNARMIC_JS_A64_REGION_FOCUS:STRING=${focus}\n`));
  assert.equal(readBuildInfo(dir).build.a64RegionFocus, focus);
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'), cache.replace('DYNARMIC_JS_A64_REGION_FOCUS:STRING=\n', ''));
  assert.throws(() => readBuildInfo(dir), /DYNARMIC_JS_A64_REGION_FOCUS/);
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'), cache);
  assert.match(hashFile(path.join(dir, 'CMakeCache.txt')), /^[a-f0-9]{64}$/);
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'), cache.replace('BOOL=ON', 'BOOL=MAYBE'));
  assert.throws(() => readBuildInfo(dir), /profiling/);
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'), cache +
    'DYNARMIC_JS_REGION_DIAGNOSTICS:BOOL=ON\nDYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS:BOOL=OFF\n');
  const instrumented = readBuildInfo(dir).build;
  assert.equal(instrumented.regionDiagnostics, true);
  assert.equal(instrumented.testExceptions, false);
  validateEnvironment({...report(), build: instrumented});
  for (const field of ['regionDiagnostics', 'testExceptions']) {
    assert.throws(() => validateEnvironment({...report(), build: {...instrumented, [field]: 'OFF'}}), /invalid build/);
  }
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'), cache + 'DYNARMIC_JS_REGION_DIAGNOSTICS:BOOL=MAYBE\n');
  assert.throws(() => readBuildInfo(dir), /DYNARMIC_JS_REGION_DIAGNOSTICS/);
  fs.writeFileSync(path.join(dir, 'CMakeCache.txt'), cache);
  fs.mkdirSync(path.join(dir, 'CMakeFiles/2'));
  fs.copyFileSync(path.join(dir, 'CMakeFiles/1/CMakeCXXCompiler.cmake'), path.join(dir, 'CMakeFiles/2/CMakeCXXCompiler.cmake'));
  assert.throws(() => readBuildInfo(dir), /unambiguous/);
  assert.throws(() => sourceInfo(dir), /uninitialized upstream/);
});
test('A64 region focus is optional in historical reports and strictly validated when present', () => {
  const r = report();
  delete r.build.a64RegionFocus;
  validateEnvironment(r);
  const same = '0x0000000000001000';
  for (const focus of [
    '',
    `${same},${same},${same}`,
    '0x0000000000001000,0x0000000000001004,0x0000000000002000',
    '0x0000000000001000,0x0000000000001004,0x0000000000002000,0x0000000000003000,0x0000000000003001',
  ]) {
    r.build.a64RegionFocus = focus;
    assert.doesNotThrow(() => validateEnvironment(r), focus);
  }
  for (const focus of [
    null, 1, {}, [],
    '0x0000000000001000',
    '0x0000000000001000,0x0000000000001004',
    '0x0000000000001000,0x0000000000001004,0x0000000000002000,',
    '0000000000001000,0x0000000000001004,0x0000000000002000',
    '0x000000000000100,0x0000000000001004,0x0000000000002000',
    '0x00000000000010000,0x0000000000001004,0x0000000000002000',
    '0x000000000000100A,0x0000000000001004,0x0000000000002000',
    '0x0000000000001000,0x0000000000001004,0x0000000000002000,0x0000000000002000',
    '0x0000000000001000,0x0000000000001004,0x0000000000002000,0x0000000000003000,0x0000000000002fff',
  ]) {
    r.build.a64RegionFocus = focus;
    assert.throws(() => validateEnvironment(r), /a64RegionFocus/);
  }
});
test('observed system metadata validates while missing provenance is rejected', () => {
  const r = {...report(), ...systemInfo()}; validateEnvironment(r);
  delete r.git.upstream; assert.throws(() => validateEnvironment(r));
});
