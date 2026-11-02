// Keep campaign control-flow tests in the no-build CI gate. Python's suite
// stubs oracle/host streams and requires no Unicorn or Capstone installation.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

test('A32/A64 campaigns reject empty, incomplete and excessive unsupported coverage', () => {
  const suite = fileURLToPath(new URL('../fuzz/test_campaign.py', import.meta.url));
  const result = spawnSync('python3', [suite, '-v'], {encoding: 'utf8', timeout: 30000});
  assert.equal(result.error, undefined, result.error?.message);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stderr, /Ran 7 tests/);
});
