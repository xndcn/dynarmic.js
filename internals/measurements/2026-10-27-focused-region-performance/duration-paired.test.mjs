import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {output, workload} from '../../../tests/bench/coremark_fixtures.mjs';
import {hashFile} from '../../../tests/bench/environment.mjs';
import {CANDIDATE_FOCUS, analyzeFocusedCoremark} from './analyze.mjs';

const collector = fileURLToPath(new URL('./duration-paired.mjs', import.meta.url));

function git(cwd, args) {
  const result = spawnSync('git', args, {cwd, encoding: 'utf8'});
  assert.equal(result.status, 0, result.stderr);
}

function makeSource(root) {
  const source = path.join(root, 'source'), upstream = path.join(source, 'externals/dynarmic');
  fs.mkdirSync(upstream, {recursive: true});
  fs.writeFileSync(path.join(upstream, 'source.txt'), 'upstream\n');
  git(upstream, ['init', '-q']);
  git(upstream, ['add', 'source.txt']);
  git(upstream, ['-c', 'user.name=test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'upstream']);
  fs.writeFileSync(path.join(source, 'source.txt'), 'source\n');
  git(source, ['init', '-q']);
  git(source, ['add', 'source.txt', 'externals/dynarmic']);
  git(source, ['-c', 'user.name=test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'source']);
  return source;
}

function makeBuild(root, source, side) {
  const build = path.join(root, side), browser = path.join(build, 'examples/browser');
  fs.mkdirSync(path.join(browser, 'dist'), {recursive: true});
  fs.mkdirSync(path.join(build, 'CMakeFiles/1'), {recursive: true});
  fs.mkdirSync(path.join(build, 'bin'), {recursive: true});
  fs.writeFileSync(path.join(build, 'bin/emscripten-version.txt'), 'test\n');
  fs.writeFileSync(path.join(build, 'CMakeFiles/1/CMakeCXXCompiler.cmake'),
    `set(CMAKE_CXX_COMPILER "${build}/bin/em++")\nset(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "test")\n`);
  const focus = side === 'candidate' ? CANDIDATE_FOCUS : '';
  fs.writeFileSync(path.join(build, 'CMakeCache.txt'),
    `CMAKE_HOME_DIRECTORY:INTERNAL=${source}\nCMAKE_BUILD_TYPE:STRING=Release\nDYNARMIC_JS_PROFILE:BOOL=OFF\n` +
    `DYNARMIC_JS_A64_REGION_FOCUS:STRING=${focus}\nDYNARMIC_JS_REGION_DIAGNOSTICS:BOOL=OFF\n` +
    'DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS:BOOL=OFF\nDYNARMIC_JS_EXAMPLES:BOOL=ON\n');
  fs.writeFileSync(path.join(browser, 'dist/arm_jit.js'), `module.exports = async () => {
    const text = new TextEncoder().encode(${JSON.stringify(output(10_000_000))});
    const heap = new Uint8Array(200000); heap.set(text);
    const compile = {blocks: 1, bytes: 100, translateMs: 0.1, optMs: 0, emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0};
    const realNow = performance.now.bind(performance); let offset = 0;
    performance.now = () => realNow() + offset;
    return {HEAPU8: heap, _arm_ram: () => 10000, _arm_ram_size: () => 100000,
      _arm_console_clear() {}, _arm_init() {}, _arm_run() {
        offset += 10050;
        if (process.env.SYNTHETIC_FAIL === '1' && __filename.includes('candidate')) {
          process.on('beforeExit', () => { console.error('failure after valid sample'); process.exitCode = 7; });
        }
        return 0x01000000;
      }, _arm_console: () => 0, _arm_console_len: () => text.length,
      dynarmicWasmStats: {activeInstance: 1, getInstance: () => ({compile})}};
  };`);
  fs.writeFileSync(path.join(browser, 'dist/arm_jit.wasm'), `synthetic ${side} artifact`);
  return browser;
}

function setup(t, failAfterOutput = false) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'focused-coremark-collector-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const source = makeSource(root);
  const baseline = makeBuild(root, source, 'baseline');
  const candidate = makeBuild(root, source, 'candidate');
  const work = path.join(root, 'workload');
  fs.mkdirSync(work);
  const manifest = workload();
  for (const architecture of ['a32', 'a64', 'native']) {
    const file = path.join(work, manifest.artifacts[architecture].file);
    fs.writeFileSync(file, architecture === 'native'
      ? `#!/bin/sh\ncat <<'END_COREMARK'\n${output(10_000_000)}END_COREMARK\n` : 'guest');
    if (architecture === 'native') fs.chmodSync(file, 0o755);
    manifest.artifacts[architecture].sha256 = hashFile(file);
  }
  fs.writeFileSync(path.join(work, 'manifest.json'), JSON.stringify(manifest));
  const outputDir = path.join(root, 'output');
  const run = (extra = []) => spawnSync(process.execPath, [collector, baseline, candidate, work, outputDir, ...extra],
    {encoding: 'utf8', timeout: 60_000, env: {...process.env, NODE_OPTIONS: '', DYN_DUMP_DIR: '',
      SYNTHETIC_FAIL: failAfterOutput ? '1' : ''}});
  return {outputDir, candidate, run};
}

test('focused collector retains exactly ten alternating pairs and refuses overwrite', t => {
  const {outputDir, run} = setup(t);
  const first = run();
  assert.equal(first.status, 0, first.stderr);
  const raw = JSON.parse(fs.readFileSync(path.join(outputDir, 'report.json'), 'utf8'));
  const comparison = analyzeFocusedCoremark(raw);
  assert.equal(comparison.pass, true);
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(outputDir, 'comparison.json'), 'utf8')), comparison);
  const observations = fs.readFileSync(path.join(outputDir, 'observations.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
  assert.equal(observations.length, 20);
  assert.deepEqual(observations.map(row => row.side), Array.from({length: 10}, (_, pair) =>
    pair % 2 ? ['candidate', 'baseline'] : ['baseline', 'candidate']).flat());
  assert(observations.every(row => row.status === 0));
  assert.equal(run().status, 2);
  assert.equal(run(['10']).status, 2);
});

test('focused collector rejects differing JS glue before timing', t => {
  const {outputDir, candidate, run} = setup(t);
  fs.appendFileSync(path.join(candidate, 'dist/arm_jit.js'), '\n// different glue\n');
  const result = run();
  assert.equal(result.status, 2);
  assert.match(result.stderr, /JS artifacts must match/);
  assert(!fs.existsSync(path.join(outputDir, 'report.json')));
  assert(!fs.existsSync(path.join(outputDir, 'observations.jsonl')));
});

test('focused collector retains failed child output and emits no verdict', t => {
  const {outputDir, run} = setup(t, true);
  const result = run();
  assert.equal(result.status, 2);
  assert(!fs.existsSync(path.join(outputDir, 'report.json')));
  assert(!fs.existsSync(path.join(outputDir, 'comparison.json')));
  const observations = fs.readFileSync(path.join(outputDir, 'observations.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
  assert.equal(observations.length, 2);
  assert.equal(observations[1].status, 7);
  assert.match(observations[1].stdout, /COREMARK_SAMPLE/);
  assert.match(observations[1].stderr, /failure after valid sample/);
  assert.match(JSON.parse(fs.readFileSync(path.join(outputDir, 'failure.json'), 'utf8')).error, /status 7/);
});
