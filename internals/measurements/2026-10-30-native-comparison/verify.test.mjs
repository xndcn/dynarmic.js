import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {test} from 'node:test';

test('retained native comparison verifies all samples and the rejected initial cohort', () => {
  const child = spawnSync(process.execPath, [fileURLToPath(new URL('./verify.mjs', import.meta.url))], {encoding: 'utf8'});
  assert.equal(child.status, 0, child.stderr);
  assert.match(child.stdout, /all 1080 samples retained; initial invalid cohort preserved/);
});
