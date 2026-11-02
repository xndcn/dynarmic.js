import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {gunzipSync, gzipSync} from 'node:zlib';
import {verifyEvidence, verifyEvidenceContent} from './verify.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const DATA_FILES = ['after-a64-flat.json.gz', 'analysis.json', 'before-a64-flat.json.gz',
  'candidate.patch', 'commands.md'];
const sha256 = data => createHash('sha256').update(data).digest('hex');

function copyEvidence(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'alias-static-gate-test-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  for (const name of [...DATA_FILES, 'SHA256SUMS']) {
    fs.copyFileSync(path.join(here, name), path.join(root, name));
  }
  return root;
}
function rehash(root) {
  fs.writeFileSync(path.join(root, 'SHA256SUMS'), DATA_FILES.map(name =>
    `${sha256(fs.readFileSync(path.join(root, name)))}  ${name}`).join('\n') + '\n');
}
function rewriteGzip(root, name, mutate) {
  const file = path.join(root, name);
  const value = JSON.parse(gunzipSync(fs.readFileSync(file)));
  mutate(value);
  fs.writeFileSync(file, gzipSync(JSON.stringify(value), {level: 9}));
}

test('retained evidence independently reproduces the exact STOP verdict', () => {
  assert.deepEqual(verifyEvidence(here), {
    baseline: 111, expected: 95, actual: 99, aliasReused: 15, added: 8, files: 5,
  });
});

test('a stale retained verdict is rejected with a refreshed inventory', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'analysis.json');
  const analysis = JSON.parse(fs.readFileSync(file, 'utf8'));
  analysis.decision = 'PASS_STATIC_GATE';
  fs.writeFileSync(file, JSON.stringify(analysis));
  rehash(root);
  assert.throws(() => verifyEvidenceContent(root), /fresh analysis/);
});

test('coordinated native/guest CRC changes cannot bypass the pinned oracle', t => {
  const root = copyEvidence(t);
  for (const name of ['before-a64-flat.json.gz', 'after-a64-flat.json.gz']) {
    rewriteGzip(root, name, report => {
      for (const sample of [report.sample, report.native]) {
        sample.stdout = sample.stdout.replace('crcfinal      : 0x4983', 'crcfinal      : 0xffff');
        sample.metrics.crc.crcfinal = 'ffff';
      }
    });
  }
  rehash(root);
  assert.throws(() => verifyEvidenceContent(root), /pinned CRC set/);
});

test('compile totals cannot be changed behind a refreshed inventory', t => {
  const root = copyEvidence(t);
  rewriteGzip(root, 'after-a64-flat.json.gz', report => { report.sample.compile.blocks += 1; });
  rehash(root);
  assert.throws(() => verifyEvidenceContent(root), /compile region total/);
});

test('frozen payload hashes reject changes invisible to the semantic analysis', t => {
  const root = copyEvidence(t);
  rewriteGzip(root, 'after-a64-flat.json.gz', report => { report.timestamp = '2000-01-01T00:00:00.000Z'; });
  rehash(root);
  assert.equal(verifyEvidenceContent(root).actual, 99);
  assert.throws(() => verifyEvidence(root), /pinned evidence bytes/);
});

test('inventory must contain every evidence file exactly once', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'SHA256SUMS');
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  fs.writeFileSync(file, lines.slice(1).join('\n') + '\n');
  assert.throws(() => verifyEvidence(root), /inventory files/);
});

test('candidate patch destinations are pinned independently of the inventory', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'candidate.patch');
  const patch = fs.readFileSync(file, 'utf8').replace(
    'b/src/dynarmic/backend/wasm/address_space.cpp', 'b/unrelated.cpp');
  fs.writeFileSync(file, patch);
  rehash(root);
  assert.throws(() => verifyEvidenceContent(root), /source\/destination|patch paths/);
});
