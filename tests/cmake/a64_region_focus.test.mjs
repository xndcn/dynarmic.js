import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const module = path.join(root, 'cmake/A64RegionFocus.cmake');
const grammarTest = path.join(here, 'validate_a64_region_focus.cmake');
const nonmatching = '0x000000000000dead,0x000000000000deae,0x000000000000deaf';

function runCmake(args) {
  return spawnSync('cmake', args, {encoding: 'utf8'});
}

function withBuild(run) {
  const parent = mkdtempSync(path.join(tmpdir(), 'dynarmic-a64-focus-cmake-'));
  try {
    run(path.join(parent, 'build'));
  } finally {
    rmSync(parent, {recursive: true, force: true});
  }
}

function configure(build, focus) {
  const args = ['-S', root, '-B', build, '-DDYNARMIC_JS_TESTS=OFF',
    '-DDYNARMIC_JS_EXAMPLES=OFF', `-DDYNARMIC_JS_A64_REGION_FOCUS=${focus}`];
  return runCmake(args);
}

function cacheValue(build) {
  const cache = readFileSync(path.join(build, 'CMakeCache.txt'), 'utf8');
  const match = cache.match(/^DYNARMIC_JS_A64_REGION_FOCUS:STRING=(.*)$/m);
  assert(match, 'focus cache entry must be a STRING');
  return match[1];
}

test('independent CMake grammar accepts valid values and rejects malformed values', () => {
  const result = runCmake([`-DA64_REGION_FOCUS_MODULE=${module}`, '-P', grammarTest]);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /grammar tests passed/);
});

test('root configuration rejects malformed focus instead of truncating or normalizing it', () => {
  withBuild(build => {
    const result = configure(build, `${nonmatching};0x0000000000000000`);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /DYNARMIC_JS_A64_REGION_FOCUS.*invalid|invalid.*DYNARMIC_JS_A64_REGION_FOCUS/);
    assert.equal(result.stderr.includes('CMake grammar tests passed'), false);
  });
});

test('explicit empty focus is a valid disabled STRING cache value', () => {
  withBuild(build => {
    const result = configure(build, '');
    assert.equal(result.status, 0, result.stderr);
    assert.equal(cacheValue(build), '');
    const flags = readFileSync(path.join(build,
      'externals/dynarmic/src/dynarmic/CMakeFiles/dynarmic.dir/flags.make'), 'utf8');
    assert.doesNotMatch(flags, /DYNARMIC_JS_A64_REGION_FOCUS/);
  });
});

test('valid nonmatching focus is preserved exactly and enables the private definition', () => {
  withBuild(build => {
    const result = configure(build, nonmatching);
    assert.equal(result.status, 0, result.stderr);
    assert.equal(cacheValue(build), nonmatching);
    const flags = readFileSync(path.join(build,
      'externals/dynarmic/src/dynarmic/CMakeFiles/dynarmic.dir/flags.make'), 'utf8');
    assert.match(flags, /DYNARMIC_JS_A64_REGION_FOCUS/);
    assert.match(flags, /0x000000000000dead,0x000000000000deae,0x000000000000deaf/);
  });
});
