import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {output, workload} from './coremark_fixtures.mjs';
import {hashFile} from './environment.mjs';
import {validateCoremarkReport} from './coremark_protocol.mjs';

function setup(t, fail = false) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'coremark-collector-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const build = path.join(root, 'build'), browser = path.join(build, 'examples/browser');
  fs.mkdirSync(path.join(browser, 'dist'), {recursive: true});
  fs.mkdirSync(path.join(build, 'CMakeFiles/1'), {recursive: true});
  fs.writeFileSync(path.join(build, 'emscripten-version.txt'), 'test\n');
  fs.writeFileSync(path.join(build, 'CMakeFiles/1/CMakeCXXCompiler.cmake'),
    `set(CMAKE_CXX_COMPILER "${build}/em++")\nset(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "test")\n`);
  const source = fileURLToPath(new URL('../..', import.meta.url));
  fs.writeFileSync(path.join(build, 'CMakeCache.txt'),
    `CMAKE_HOME_DIRECTORY:INTERNAL=${source}\nCMAKE_BUILD_TYPE:STRING=Release\nDYNARMIC_JS_PROFILE:BOOL=OFF\nDYNARMIC_JS_A64_REGION_FOCUS:STRING=\n`);
  const jit = path.join(browser, 'dist/arm_jit.js');
  fs.writeFileSync(jit, `module.exports = async () => {
    const text = new TextEncoder().encode(${JSON.stringify(output(1000))});
    const heap = new Uint8Array(200000); heap.set(text);
    const compile = {blocks: 1, bytes: 100, translateMs: 0.1, optMs: 0, emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0};
    return {HEAPU8: heap, _arm_ram: () => 10000, _arm_ram_size: () => 100000,
      _arm_console_clear() {}, _arm_init() {}, _arm_run() {
        const start = performance.now(); while (performance.now() - start < 2) {}
        ${fail ? "process.on('beforeExit', () => { console.error('failure after valid sample'); process.exitCode = 7; });" : ''}
        return 0x01000000;
      }, _arm_console: () => 0, _arm_console_len: () => text.length,
      dynarmicWasmStats: {activeInstance: 1, getInstance: () => ({compile})}};
  };`);
  fs.writeFileSync(path.join(browser, 'dist/arm_jit.wasm'), 'synthetic artifact');
  const w = workload();
  for (const a of ['a32', 'a64', 'native']) {
    const f = path.join(root, w.artifacts[a].file);
    fs.writeFileSync(f, a === 'native' ? `#!/bin/sh\ncat <<'END_COREMARK'\n${output(1000)}END_COREMARK\n` : 'guest');
    if (a === 'native') fs.chmodSync(f, 0o755);
    w.artifacts[a].sha256 = hashFile(f);
  }
  fs.writeFileSync(path.join(root, 'manifest.json'), JSON.stringify(w));
  const report = path.join(root, 'report.json');
  const run = (options = []) => spawnSync(process.execPath, [fileURLToPath(new URL('./coremark/run.mjs', import.meta.url)),
    browser, root, '--runs', '1', '--output', report, ...options], {encoding: 'utf8', timeout: 20000});
  return {root, report, run};
}
test('CoreMark collector writes validated short reports and rejects overwrites', t => {
  const {report, run} = setup(t);
  const first = run(); assert.equal(first.status, 0, first.stderr);
  const written = JSON.parse(fs.readFileSync(report));
  validateCoremarkReport(written);
  assert.equal(written.build.a64RegionFocus, '');
  assert.equal(run().status, 2);
});
test('CoreMark require-duration retains short report and explicit failure', t => {
  const {report, run} = setup(t);
  assert.equal(run(['--require-duration']).status, 2);
  validateCoremarkReport(JSON.parse(fs.readFileSync(report)));
  assert.match(JSON.parse(fs.readFileSync(report + '.failure.json')).error, /below 10 seconds/);
});
test('CoreMark failure after valid output retains child status and stderr', t => {
  const {report, run} = setup(t, true);
  assert.equal(run().status, 2);
  assert(!fs.existsSync(report));
  const failure = JSON.parse(fs.readFileSync(report + '.failure.json'));
  assert.equal(failure.last.status, 7);
  assert.match(failure.last.stdout, /COREMARK_SAMPLE/);
  assert.match(failure.last.stderr, /failure after valid sample/);
});
test('CoreMark collector rejects changed binary before running the oracle', t => {
  const {root, report, run} = setup(t);
  fs.appendFileSync(path.join(root, 'coremark_a32.bin'), 'changed');
  const result = run(); assert.equal(result.status, 2); assert.match(result.stderr, /executable changed/);
  assert(!fs.existsSync(report));
});
