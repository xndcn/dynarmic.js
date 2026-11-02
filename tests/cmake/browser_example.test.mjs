import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';

const browser = fileURLToPath(new URL('../../examples/browser/', import.meta.url));
const referenceWriter = path.join(browser, 'write_expected.cmake');

function fixture(t) {
  // Spaces exercise execute_process argument boundaries, not shell quoting.
  const root = mkdtempSync(path.join(tmpdir(), 'dynarmic browser example '));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  return root;
}

function program(root, name, source) {
  const file = path.join(root, name);
  writeFileSync(file, `#!/usr/bin/env node\n${source}\n`, {mode: 0o755});
  return file;
}

function cmake(args) {
  const result = spawnSync('cmake', args, {
    encoding: 'utf8', timeout: 30_000,
    env: {...process.env, PATH: `${path.dirname(process.execPath)}${path.delimiter}${process.env.PATH || ''}`},
  });
  assert.ifError(result.error);
  return result;
}

test('strict browser configuration reports each missing prerequisite before compilation', t => {
  const root = fixture(t);
  // Configure the real example in a compiler-free parent project. The host
  // helper is irrelevant to prerequisite discovery and no target is built.
  writeFileSync(path.join(root, 'emscripten-version.txt'), 'test-fixture\n');
  writeFileSync(path.join(root, 'CMakeLists.txt'), `
cmake_minimum_required(VERSION 3.16)
project(browser_example_prerequisites LANGUAGES NONE)
set(CMAKE_CXX_COMPILER "\${CMAKE_CURRENT_SOURCE_DIR}/em++")
function(dynarmic_js_host)
endfunction()
add_subdirectory([==[${browser}]==] example)
`);
  const available = program(root, 'available tool', 'process.exit(0);');
  const missingModule = program(root, 'python without ziglang', 'process.exit(1);');
  const defaults = {ARM_GCC: available, ARM_OBJCOPY: available, HOST_CC: available, DEMO_PYTHON: available};
  const cases = [
    {name: 'all', tools: {ARM_GCC: '', ARM_OBJCOPY: '', HOST_CC: '', DEMO_PYTHON: ''},
      expected: ['arm-none-eabi-gcc', 'arm-none-eabi-objcopy', 'native C compiler', 'ziglang']},
    {name: 'arm compiler', tools: {ARM_GCC: ''}, expected: ['arm-none-eabi-gcc']},
    {name: 'arm objcopy', tools: {ARM_OBJCOPY: ''}, expected: ['arm-none-eabi-objcopy']},
    {name: 'native compiler', tools: {HOST_CC: ''}, expected: ['native C compiler']},
    {name: 'python', tools: {DEMO_PYTHON: ''}, expected: ['Python 3', 'ziglang']},
    {name: 'ziglang', tools: {DEMO_PYTHON: missingModule}, expected: ['Python 3', 'ziglang']},
  ];
  for (const {name, tools, expected} of cases) {
    const result = cmake(['-S', root, '-B', path.join(root, `build ${name}`),
      '-DDYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON',
      ...Object.entries({...defaults, ...tools}).map(([key, value]) => `-D${key}:FILEPATH=${value}`)]);
    assert.notEqual(result.status, 0, `${name}: strict configure must fail`);
    const diagnostic = result.stderr.replace(/\s+/g, ' ');
    assert.match(diagnostic, /browser example is incomplete/, result.stderr);
    assert.match(diagnostic, /Install these tools and rerun CMake/, result.stderr);
    assert.match(diagnostic, /examples\/browser\/README\.md/, result.stderr);
    for (const tool of expected) assert(diagnostic.includes(tool), `${name}: ${result.stderr}`);
  }
});

test('native reference capture preserves exact stdout and spaced executable/output paths', t => {
  const root = fixture(t);
  const expected = 'lcg   = 2932257054\nsieve = 283146\npi*1e6 = 3141592\n';
  const executable = program(root, 'native reference program',
    `process.stdout.write(${JSON.stringify(expected)}); process.stderr.write('timing diagnostic\\n');`);
  const output = path.join(root, 'expected output.txt');
  writeFileSync(output, 'stale reference');
  const result = cmake([`-DPROGRAM=${executable}`, `-DOUTPUT=${output}`, '-P', referenceWriter]);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(readFileSync(output, 'utf8'), expected);
  assert.match(result.stderr, /timing diagnostic/);
});

test('native reference failures remove partial or stale output and fail the build', t => {
  const root = fixture(t);
  const failing = program(root, 'failing reference program',
    "process.stdout.write('partial result'); process.exitCode = 7;");
  const output = path.join(root, 'expected output.txt');
  for (const executable of [failing, path.join(root, 'missing reference program')]) {
    writeFileSync(output, 'stale reference');
    const result = cmake([`-DPROGRAM=${executable}`, `-DOUTPUT=${output}`, '-P', referenceWriter]);
    assert.notEqual(result.status, 0, 'a failed reference must fail the build');
    assert.match(result.stderr, /native demo reference failed/, result.stderr);
    assert.equal(existsSync(output), false, 'a failed reference must not leave an oracle behind');
  }
});
