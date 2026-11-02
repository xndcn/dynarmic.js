#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

const [input, ...extra] = process.argv.slice(2);
if (!input || extra.length) throw new Error('Usage: node tests/npm/verify_package.mjs staged-package-directory');
const source = path.resolve(input);
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-package-integrity-'));
const run = (directory, args = []) => spawnSync(process.execPath, ['verify.mjs', ...args], {cwd: directory, encoding: 'utf8'});
const checkFailure = (name, mutate, expected) => {
  const directory = path.join(temporary, name);
  fs.cpSync(source, directory, {recursive: true});
  mutate(directory);
  const result = run(directory);
  assert.notEqual(result.status, 0, `${name} must fail`);
  assert.match(result.stderr, expected);
};
try {
  const valid = run(source);
  assert.equal(valid.status, 0, valid.stderr);
  const packed = spawnSync('npm', ['pack', source, '--pack-destination', temporary, '--json'],
    {encoding: 'utf8', timeout: 60000});
  assert.equal(packed.status, 0, packed.stderr);
  const inventory = JSON.parse(packed.stdout);
  assert.equal(inventory.length, 1, 'npm pack must produce one JSON inventory');
  assert.deepEqual(inventory[0].files.map(file => file.path).sort(), fs.readdirSync(source).sort(),
    'the actual tarball must include exactly the verified staged files');
  checkFailure('corrupt-runtime', directory => fs.appendFileSync(path.join(directory, 'runtime.wasm'), 'tampered'), /checksum mismatch: runtime.wasm/);
  checkFailure('missing-source', directory => fs.unlinkSync(path.join(directory, 'source.tar.gz')), /ENOENT/);
  checkFailure('extra-file', directory => fs.writeFileSync(path.join(directory, 'accidental-secret.txt'), 'fixture-only'), /outside its allowlist/);
  checkFailure('symlink-runtime', directory => {
    fs.unlinkSync(path.join(directory, 'runtime.wasm'));
    fs.symlinkSync(path.join(source, 'runtime.wasm'), path.join(directory, 'runtime.wasm'));
  }, /Non-regular package file/);
  const manifest = JSON.parse(fs.readFileSync(path.join(source, 'source-manifest.json')));
  const release = run(source, ['--require-clean']);
  if (manifest.correspondenceVerified) assert.equal(release.status, 0, release.stderr);
  else {
    assert.notEqual(release.status, 0);
    assert.match(release.stderr, /Publication requires a clean recursive checkout/);
  }
  console.log('Package integrity: valid package, npm pack JSON and inventory, modified runtime, missing source, extra file, symlink and release cleanliness passed');
} finally {
  fs.rmSync(temporary, {recursive: true, force: true});
}
