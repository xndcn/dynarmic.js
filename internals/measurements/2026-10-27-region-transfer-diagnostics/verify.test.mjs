import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {gunzipSync, gzipSync} from 'node:zlib';
import {fileURLToPath} from 'node:url';
import {verifyEvidence, verifyEvidenceContent} from './verify.mjs';

const evidence = path.join(path.dirname(fileURLToPath(import.meta.url)), 'evidence');
const sha256 = data => createHash('sha256').update(data).digest('hex');

function copyEvidence(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'region-transfer-evidence-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const copy = path.join(root, 'evidence');
  fs.cpSync(evidence, copy, {recursive: true});
  return copy;
}

function refreshInventory(directory, name) {
  const file = path.join(directory, 'SHA256SUMS');
  const rows = fs.readFileSync(file, 'utf8').split('\n').filter(Boolean).map(line => {
    const [hash, pathName] = line.split('  ');
    return pathName === name ? `${sha256(fs.readFileSync(path.join(directory, name)))}  ${name}` : `${hash}  ${pathName}`;
  });
  fs.writeFileSync(file, rows.join('\n') + '\n');
}

test('frozen reports and fresh exact analysis pass', () => {
  const result = verifyEvidence(evidence);
  assert.equal(result.cases, 2);
  assert.equal(result.transferCount, '298200064');
  assert.deepEqual(result.hotEdgeCounts, ['47199999', '47199999']);
});

test('stale derived analysis fails even with a refreshed inventory', t => {
  const copy = copyEvidence(t);
  const file = path.join(copy, 'analysis.json');
  const analysis = JSON.parse(fs.readFileSync(file, 'utf8'));
  analysis.cases[0].totals.externalDirectCallCount = '0x0000000000000000';
  fs.writeFileSync(file, JSON.stringify(analysis, null, 2) + '\n');
  refreshInventory(copy, 'analysis.json');
  assert.throws(() => verifyEvidenceContent(copy), /fresh analysis/);
});

test('modified raw bytes fail the frozen hash', t => {
  const copy = copyEvidence(t);
  const file = path.join(copy, 'a64-flat-window.json.gz');
  const raw = fs.readFileSync(file);
  raw[raw.length - 1] ^= 1;
  fs.writeFileSync(file, raw);
  assert.throws(() => verifyEvidence(copy), /pinned evidence bytes/);
});

test('dirty-source tampering fails semantic validation with valid inventory', t => {
  const copy = copyEvidence(t);
  const name = 'a64-flat-window.json.gz';
  const file = path.join(copy, name);
  const report = JSON.parse(gunzipSync(fs.readFileSync(file)));
  report.git.dirty = true;
  fs.writeFileSync(file, gzipSync(JSON.stringify(report)));
  refreshInventory(copy, name);
  assert.throws(() => verifyEvidenceContent(copy), /clean source/);
});
