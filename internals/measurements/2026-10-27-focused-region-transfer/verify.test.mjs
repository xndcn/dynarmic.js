import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {gunzipSync, gzipSync} from 'node:zlib';
import {analyzeReports} from './analyze.mjs';
import {makeReports, refreshReport, u64} from './test_fixtures.mjs';
import {FROZEN_SHA256, verifyEvidence, verifyEvidenceContent} from './verify.mjs';

const verifySyntheticContent = directory => verifyEvidenceContent(directory, {realEvidence: false});
const verifySyntheticEvidence = (directory, pins) => verifyEvidence(directory, pins, {realEvidence: false});

const FILES = ['baseline.json.gz', 'candidate.json.gz', 'analysis.json', 'commands.md'];
const sha256 = data => createHash('sha256').update(data).digest('hex');
function rehash(directory) {
  fs.writeFileSync(path.join(directory, 'SHA256SUMS'), FILES.map(name =>
    `${sha256(fs.readFileSync(path.join(directory, name)))}  ${name}`).join('\n') + '\n');
}
function pins(directory) {
  return Object.fromEntries(FILES.map(name => [name, sha256(fs.readFileSync(path.join(directory, name)))]));
}
function evidence(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'focused-transfer-verify-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const {baseline, candidate} = makeReports();
  fs.writeFileSync(path.join(directory, 'baseline.json.gz'), gzipSync(JSON.stringify(baseline)));
  fs.writeFileSync(path.join(directory, 'candidate.json.gz'), gzipSync(JSON.stringify(candidate)));
  fs.writeFileSync(path.join(directory, 'analysis.json'), JSON.stringify(analyzeReports(baseline, candidate), null, 2));
  fs.writeFileSync(path.join(directory, 'commands.md'), '# Synthetic fixture only\n');
  rehash(directory);
  return directory;
}
function rewriteAnalysis(directory, mutate) {
  const file = path.join(directory, 'analysis.json');
  const value = JSON.parse(fs.readFileSync(file));
  mutate(value);
  fs.writeFileSync(file, JSON.stringify(value));
  rehash(directory);
}
function rewriteReport(directory, name, mutate) {
  const file = path.join(directory, name);
  const value = JSON.parse(gunzipSync(fs.readFileSync(file)));
  mutate(value);
  fs.writeFileSync(file, gzipSync(JSON.stringify(value)));
  rehash(directory);
}

test('synthetic inventory, fresh semantic analysis and independent frozen pins pass', t => {
  const directory = evidence(t);
  assert.deepEqual(verifySyntheticContent(directory), {
    schema: 'dynarmic.js/focused-region-transfer-analysis-v1', files: 4,
    scope: 'one-Run mechanism accounting; no performance qualification',
  });
  assert.equal(verifySyntheticEvidence(directory, pins(directory)).files, 4);
  assert.equal(FROZEN_SHA256['analysis.json'], '138c7cf3a38209cb33d1bc1131e68c88018730ae1e7649407b35c96f13f41103');
  assert.throws(() => verifyEvidence(directory), /pinned evidence bytes/);
});

test('stale retained analysis fails even with refreshed inventory and pins', t => {
  const directory = evidence(t);
  const before = pins(directory);
  rewriteAnalysis(directory, value => { value.wholeRunTotals.baseline.transferCount = '0x0000000000000000'; });
  assert.throws(() => verifySyntheticContent(directory), /fresh analysis differs/);
  assert.throws(() => verifySyntheticEvidence(directory, before), /pinned evidence bytes/);
});

test('raw semantic tampering fails with a refreshed inventory', t => {
  const directory = evidence(t);
  rewriteReport(directory, 'candidate.json.gz', report => { report.sample.haltReason = 0; });
  assert.throws(() => verifySyntheticContent(directory), /normal halt/);
});

test('frozen byte hashes independently reject semantic no-op tampering', t => {
  const directory = evidence(t);
  const before = pins(directory);
  rewriteReport(directory, 'baseline.json.gz', report => { report.timestamp = '2000-01-01T00:00:00.000Z'; });
  assert.equal(verifySyntheticContent(directory).files, 4);
  assert.throws(() => verifySyntheticEvidence(directory, before), /pinned evidence bytes: baseline.json.gz/);
});

test('refreshed inventory does not make fabricated baseline zero totals valid', t => {
  const directory = evidence(t);
  rewriteAnalysis(directory, value => {
    value.wholeRunTotals.baseline.entryCount = '0x0000000000000000';
    value.wholeRunTotals.baseline.transferCount = '0x0000000000000000';
  });
  assert.throws(() => verifySyntheticContent(directory), /fresh analysis differs/);
});

test('removed zero-count analysis row fails fresh deep equality', t => {
  const directory = evidence(t);
  rewriteAnalysis(directory, value => {
    const index = value.exits.added.findIndex(row => row.candidate?.count === '0x0000000000000000');
    assert(index >= 0);
    value.exits.added.splice(index, 1);
  });
  assert.throws(() => verifySyntheticContent(directory), /fresh analysis differs/);
});

for (const [name, row, pattern] of [
  ['absolute path', `${'0'.repeat(64)}  /tmp/baseline.json.gz`, /unsafe inventory path/],
  ['parent traversal', `${'0'.repeat(64)}  ../baseline.json.gz`, /unsafe inventory path/],
]) test(`verifier rejects ${name}`, t => {
  const directory = evidence(t);
  fs.writeFileSync(path.join(directory, 'SHA256SUMS'), row + '\n');
  assert.throws(() => verifySyntheticContent(directory), pattern);
});

test('verifier rejects duplicate inventory paths', t => {
  const directory = evidence(t);
  const file = path.join(directory, 'SHA256SUMS');
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  fs.writeFileSync(file, [...lines, lines[0]].join('\n') + '\n');
  assert.throws(() => verifySyntheticContent(directory), /duplicate inventory path/);
});

test('verifier rejects missing inventory entries and files', t => {
  const directory = evidence(t);
  const file = path.join(directory, 'SHA256SUMS');
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  fs.writeFileSync(file, lines.slice(1).join('\n') + '\n');
  assert.throws(() => verifySyntheticContent(directory), /inventory files/);
  rehash(directory);
  fs.rmSync(path.join(directory, 'commands.md'));
  assert.throws(() => verifySyntheticContent(directory), /inventory hash|ENOENT/);
});

test('coordinated raw, derived, analysis and inventory tampering fails original frozen pins', t => {
  const directory = evidence(t);
  const originalPins = pins(directory);
  const baseline = JSON.parse(gunzipSync(fs.readFileSync(path.join(directory, 'baseline.json.gz'))));
  let candidate = JSON.parse(gunzipSync(fs.readFileSync(path.join(directory, 'candidate.json.gz'))));
  const common = candidate.regionTransferDiagnostics.regions[2];
  common.entryCount = u64(4);
  common.exits[0].count = u64(4);
  candidate = refreshReport(candidate);
  fs.writeFileSync(path.join(directory, 'candidate.json.gz'), gzipSync(JSON.stringify(candidate)));
  fs.writeFileSync(path.join(directory, 'analysis.json'), JSON.stringify(analyzeReports(baseline, candidate)));
  rehash(directory);
  assert.equal(verifySyntheticContent(directory).files, 4);
  assert.throws(() => verifySyntheticEvidence(directory, originalPins), /pinned evidence bytes: candidate.json.gz/);
});

test('commands.md byte tampering fails original frozen pins despite refreshed inventory', t => {
  const directory = evidence(t);
  const originalPins = pins(directory);
  fs.appendFileSync(path.join(directory, 'commands.md'), 'tampered\n');
  rehash(directory);
  assert.equal(verifySyntheticContent(directory).files, 4);
  assert.throws(() => verifySyntheticEvidence(directory, originalPins), /pinned evidence bytes: commands.md/);
});
