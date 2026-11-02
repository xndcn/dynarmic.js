import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {cases} from './protocol.mjs';
import {samples} from './fixtures.mjs';
import {validateNativeReport} from './native_protocol.mjs';

// Synthetic executables exercise process collection and evidence handling.
// Their timings are fixture values, never benchmark performance evidence.
const cli = fileURLToPath(new URL('./native_run.mjs', import.meta.url));
const supported = process.platform === 'linux' && ['x64', 'arm64'].includes(process.arch);
function fixture(fn, {fail = false, mutate = false, diagnostics = false, exceptions = false} = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-native-collector-'));
  try {
    const sourceRoot = path.join(dir, 'source');
    const upstream = path.join(sourceRoot, 'externals/dynarmic');
    fs.mkdirSync(upstream, {recursive: true});
    fs.mkdirSync(path.join(sourceRoot, 'tests/hosts'), {recursive: true});
    // Deliberately independent of the checkout's optional submodule. The fast
    // protocol tests must work before any dependency is fetched or built.
    for (const repo of [sourceRoot, upstream]) {
      for (const args of [['init', '--quiet'], ['-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
        '-c', 'core.hooksPath=/dev/null', '-c', 'commit.gpgsign=false', 'commit', '--quiet', '--allow-empty', '-m', 'fixture']]) {
        const git = spawnSync('git', ['-C', repo, ...args], {encoding: 'utf8'});
        assert.equal(git.status, 0, git.stderr);
      }
    }
    fs.writeFileSync(path.join(sourceRoot, 'tests/hosts/benchmark.h'), '// synthetic measurement helper\n');
    for (const file of new Set(cases.map(spec => spec.file))) {
      fs.writeFileSync(path.join(sourceRoot, 'tests/hosts', file.replace(/\.js$/, '.cpp')), '// synthetic guest host\n');
    }
    const native = path.join(dir, 'native'), wasm = path.join(dir, 'wasm'), wasmHosts = path.join(wasm, 'tests');
    for (const build of [native, wasm]) fs.mkdirSync(path.join(build, 'CMakeFiles', 'test'), {recursive: true});
    fs.mkdirSync(wasmHosts); fs.mkdirSync(path.join(dir, 'tool'));
    fs.writeFileSync(path.join(dir, 'tool', 'emscripten-version.txt'), 'test\n');
    const flags = 'CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_FLAGS_RELEASE:STRING=-O3\n';
    fs.writeFileSync(path.join(native, 'CMakeCache.txt'), flags +
      `CMAKE_PROJECT_NAME:STATIC=dynarmic_native_bench\nDYNARMIC_JS_SOURCE_ROOT:INTERNAL=${sourceRoot}\n` +
      `DYNARMIC_NATIVE_BACKEND:INTERNAL=${process.arch === 'x64' ? 'x86_64' : 'arm64'}\n` +
      'DYNARMIC_TESTS:BOOL=OFF\nDYNARMIC_USE_BUNDLED_EXTERNALS:BOOL=ON\nDYNARMIC_FRONTENDS:STRING=A32;A64\nDYNARMIC_IGNORE_ASSERTS:BOOL=OFF\n');
    fs.writeFileSync(path.join(wasm, 'CMakeCache.txt'), flags + `CMAKE_HOME_DIRECTORY:INTERNAL=${sourceRoot}\n` +
      `DYNARMIC_JS_PROFILE:BOOL=OFF\nDYNARMIC_JS_A64_REGION_FOCUS:STRING=\nDYNARMIC_JS_REGION_DIAGNOSTICS:BOOL=${diagnostics ? 'ON' : 'OFF'}\n` +
      `DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS:BOOL=${exceptions ? 'ON' : 'OFF'}\n`);
    for (const build of [native, wasm]) {
      fs.writeFileSync(path.join(build, 'CMakeFiles', 'test', 'CMakeCXXCompiler.cmake'),
        `set(CMAKE_CXX_COMPILER "${dir}/tool/em++")\nset(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "test")\n`);
      fs.writeFileSync(path.join(build, 'CMakeFiles', 'test', 'CMakeSystem.cmake'), 'set(CMAKE_SYSTEM_PROCESSOR "test")\n');
    }
    const observations = samples().map(({process, ...sample}) => sample);
    const stdout = values => values.map(s => `BENCHMARK ${JSON.stringify(s)}`).join('\n') + '\nPASS\n';
    for (const file of new Set(cases.map(spec => spec.file))) {
      fs.writeFileSync(path.join(wasmHosts, file), `process.stdout.write(${JSON.stringify(stdout(observations))});\n`);
      fs.writeFileSync(path.join(wasmHosts, file.replace(/\.js$/, '.wasm')), 'fixture artifact');
      const nativeFile = path.join(native, file.replace(/\.js$/, ''));
      fs.writeFileSync(nativeFile, '#!/bin/sh\n' + (fail ? 'echo "FAIL native fixture"\necho "native diagnostic" >&2\nexit 7\n'
        : `cat <<'BENCH_OUTPUT'\n${stdout(observations.map(s => ({...s, compile: null})))}BENCH_OUTPUT\n` +
          (mutate ? 'printf "# changed\\n" >> "$0"\n' : '')));
      fs.chmodSync(nativeFile, 0o755);
    }
    const output = path.join(dir, 'report.json');
    const run = () => spawnSync(process.execPath, [cli, wasmHosts, native, '--runs', '1', '--output', output],
      {encoding: 'utf8', timeout: 30000, env: {...process.env, DYN_DUMP_DIR: ''}});
    fn({dir, output, run});
  } finally { fs.rmSync(dir, {recursive: true, force: true}); }
}

test('collector preserves every process and produces a verifiable smoke artifact', {skip: !supported}, () => fixture(({output, run}) => {
  const child = run(); assert.equal(child.status, 0, child.stderr);
  const report = validateNativeReport(JSON.parse(fs.readFileSync(output, 'utf8')));
  assert.equal(report.qualification, 'smoke-only');
  assert.equal(report.cases.length, 12);
  assert(report.cases.every(item => item.pairs[0].native.stdout.endsWith('PASS\n')));
  const before = fs.readFileSync(output);
  assert.equal(run().status, 2, 'must refuse to overwrite an experiment');
  assert.deepEqual(fs.readFileSync(output), before);
}));

test('failed native child retains raw diagnostics without publishing a valid report', {skip: !supported}, () => fixture(({output, run}) => {
  const child = run(); assert.equal(child.status, 2);
  assert(!fs.existsSync(output));
  const failure = JSON.parse(fs.readFileSync(`${output}.failure.json`, 'utf8'));
  assert.equal(failure.schema, 'dynarmic.js/native-comparison-failure-v1');
  assert.equal(failure.active.pair.native.exitCode, 7);
  assert.match(failure.active.pair.native.stdout, /FAIL native fixture/);
  assert.match(failure.active.pair.native.stderr, /native diagnostic/);
  assert(failure.partial.cases.every(item => item.pairs.length === 0));
  assert.equal(run().status, 2, 'must retain the original failed experiment');
}, {fail: true}));

test('collector detects executable replacement between the paired processes', {skip: !supported}, () => fixture(({output, run}) => {
  assert.equal(run().status, 2);
  assert(!fs.existsSync(output));
  const failure = JSON.parse(fs.readFileSync(`${output}.failure.json`, 'utf8'));
  assert.match(failure.error, /executables changed/);
}, {mutate: true}));

for (const option of ['diagnostics', 'exceptions']) {
  test(`collector rejects ${option} overhead before launching children`, {skip: !supported}, () => fixture(({output, run}) => {
    const child = run(); assert.equal(child.status, 2);
    assert.match(child.stderr, /disable (region diagnostics|test-only exception overhead)/);
    assert(!fs.existsSync(output)); assert(!fs.existsSync(`${output}.failure.json`));
  }, {[option]: true}));
}
