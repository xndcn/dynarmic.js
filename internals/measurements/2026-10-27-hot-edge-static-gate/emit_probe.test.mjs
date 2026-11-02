import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

const tool = process.env.EMIT_PROBE;
const input = process.env.EMIT_PROBE_INPUT;
assert(tool && input, 'set EMIT_PROBE and EMIT_PROBE_INPUT to run native emitter integration tests');

const EXPECTED_SHA256 = Object.freeze({
  'baseline.wasm': '3e5a89aa815d658b85988cebbea49475f7ec4bde907c916c98098f877b29a306',
  'candidate.wasm': '9effb79067b9a658cc6c94a872f8cb093fa134e94d7b4c8baab4e492b7405102',
});

function outputDirectory(t, mode = 0o700) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hot-edge-emit-test-'));
  const output = path.join(root, 'output');
  fs.mkdirSync(output, {mode});
  t.after(() => {
    fs.chmodSync(output, 0o700);
    fs.rmSync(root, {recursive: true, force: true});
  });
  return output;
}

function run(output) {
  const result = spawnSync(tool, [input, output], {
    encoding: 'utf8', maxBuffer: 1024 * 1024, timeout: 120000,
  });
  assert.equal(result.error, undefined, result.error?.message);
  return result;
}

const failureOutput = result => `status=${result.status}\nstdout=${result.stdout}\nstderr=${result.stderr}`;

test('stale output is refused without overwriting an existing module', t => {
  const output = outputDirectory(t);
  const baseline = path.join(output, 'baseline.wasm');
  const sentinel = Buffer.from('stale module must survive');
  fs.writeFileSync(baseline, sentinel);
  const result = run(output);
  assert.notEqual(result.status, 0, failureOutput(result));
  assert.deepEqual(fs.readFileSync(baseline), sentinel);
});

test('an unwritable empty output directory returns nonzero', t => {
  if (process.geteuid?.() === 0) {
    t.skip('root can write through directory permission bits');
    return;
  }
  const output = outputDirectory(t, 0o500);
  const result = run(output);
  assert.notEqual(result.status, 0, failureOutput(result));
  assert.deepEqual(fs.readdirSync(output), []);
});

test('fresh output reproduces valid retained Wasm bytes', t => {
  const output = outputDirectory(t);
  const result = run(output);
  assert.equal(result.status, 0, failureOutput(result));
  assert.doesNotThrow(() => JSON.parse(result.stdout), 'emitter stdout must remain JSON');
  for (const [name, expected] of Object.entries(EXPECTED_SHA256)) {
    const bytes = fs.readFileSync(path.join(output, name));
    assert(WebAssembly.validate(bytes), `${name} must be valid Wasm`);
    assert.equal(createHash('sha256').update(bytes).digest('hex'), expected, `${name} hash`);
  }
});
