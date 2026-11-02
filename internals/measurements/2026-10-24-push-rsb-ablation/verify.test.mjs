import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {gunzipSync, gzipSync} from 'node:zlib';
import {analyzeRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {compareReports} from './analyze.mjs';
import {verifyEvidence} from './verify.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const DATA_FILES = [
  'baseline.json.gz', 'candidate.json.gz', 'candidate.patch', 'candidate-status.txt',
  'commands.md', 'comparison.json', 'targets.txt',
];
const sha256 = data => createHash('sha256').update(data).digest('hex');

function copyEvidence(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'push-rsb-evidence-test-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  for (const name of [...DATA_FILES, 'SHA256SUMS']) fs.copyFileSync(path.join(here, name), path.join(root, name));
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

function readGzip(root, name) {
  return JSON.parse(gunzipSync(fs.readFileSync(path.join(root, name))));
}

function refreshCandidate(root, mutate) {
  rewriteGzip(root, 'candidate.json.gz', report => {
    mutate(report);
    report.duplication = analyzeRegionDiagnostics(report.regionDiagnostics);
    report.sample.compile.blocks = report.duplication.totals.regionCount;
    report.sample.compile.bytes = report.duplication.totals.moduleBytes;
  });
  const comparison = compareReports(
    readGzip(root, 'baseline.json.gz'), readGzip(root, 'candidate.json.gz'));
  fs.writeFileSync(path.join(root, 'comparison.json'), `${JSON.stringify(comparison, null, 2)}\n`);
  rehash(root);
}

test('retained evidence reproduces the exact static comparison', () => {
  assert.deepEqual(verifyEvidence(here), {
    regions: 111, changedEntries: 36, hotEntries: 6, files: DATA_FILES.length,
  });
});

test('a stale retained verdict is rejected even with a refreshed inventory', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'comparison.json');
  const comparison = JSON.parse(fs.readFileSync(file, 'utf8'));
  comparison.verdict = 'REJECT';
  fs.writeFileSync(file, JSON.stringify(comparison));
  rehash(root);
  assert.throws(() => verifyEvidence(root), /fresh comparison/);
});

test('retained stdout tampering is rejected even with a refreshed inventory', t => {
  const root = copyEvidence(t);
  rewriteGzip(root, 'candidate.json.gz', report => {
    report.sample.stdout = report.sample.stdout.replace('crcfinal      : 0x4983', 'crcfinal      : 0xffff');
  });
  rehash(root);
  assert.throws(() => verifyEvidence(root), /candidate guest metrics\/CRC differ from retained stdout/);
});

test('the frozen patch must contain exactly the pinned target set', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'candidate.patch');
  const patch = fs.readFileSync(file, 'utf8').replace('0x10cb8', '0x10cbc');
  fs.writeFileSync(file, patch);
  rehash(root);
  assert.throws(() => verifyEvidence(root), /patch target list/);
});

test('inventory must name every evidence file exactly once', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'SHA256SUMS');
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  fs.writeFileSync(file, lines.slice(1).join('\n') + '\n');
  assert.throws(() => verifyEvidence(root), /inventory files/);
});

test('coordinated patch and target-list tampering is rejected', t => {
  const root = copyEvidence(t);
  for (const name of ['candidate.patch', 'targets.txt']) {
    const file = path.join(root, name);
    const [before, after] = name === 'targets.txt'
      ? ['0x0000000000010cb8', '0x0000000000010cbc'] : ['0x10cb8', '0x10cbc'];
    fs.writeFileSync(file, fs.readFileSync(file, 'utf8').replace(before, after));
  }
  rehash(root);
  assert.throws(() => verifyEvidence(root), /pinned target list/);
});

test('candidate provenance rejects additional modified files', t => {
  const root = copyEvidence(t);
  fs.appendFileSync(path.join(root, 'candidate-status.txt'), ' M unrelated.cpp\n');
  rehash(root);
  assert.throws(() => verifyEvidence(root), /candidate source provenance/);
});

test('candidate patch destination paths are pinned', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'candidate.patch');
  const patch = fs.readFileSync(file, 'utf8').replace(
    'b/src/dynarmic/backend/wasm/region_selector.cpp', 'b/unrelated.cpp');
  fs.writeFileSync(file, patch);
  rehash(root);
  assert.throws(() => verifyEvidence(root), /candidate patch paths/);
});

test('static total deltas are independently pinned', t => {
  const root = copyEvidence(t);
  refreshCandidate(root, report => {
    report.regionDiagnostics.regions[0].moduleBytes += 1;
  });
  assert.throws(() => verifyEvidence(root), /static total deltas/);
});

test('entry identity is checked before derived static totals', t => {
  const root = copyEvidence(t);
  refreshCandidate(root, report => {
    const [first, second] = report.regionDiagnostics.regions;
    const firstEntry = first.entryDescriptor;
    first.selectedDescriptors[first.entryIndex] = second.entryDescriptor;
    first.entryDescriptor = second.entryDescriptor;
    second.selectedDescriptors[second.entryIndex] = firstEntry;
    second.entryDescriptor = firstEntry;
  });
  assert.throws(() => verifyEvidence(root), /entry sequence changed/);
});

test('region identity is checked before derived static totals', t => {
  const root = copyEvidence(t);
  refreshCandidate(root, report => {
    report.regionDiagnostics.regions.pop();
  });
  assert.throws(() => verifyEvidence(root), /region IDs changed/);
});

test('the disappeared-target set is independently pinned', t => {
  const root = copyEvidence(t);
  refreshCandidate(root, report => {
    const region = report.regionDiagnostics.regions.find(row => row.regionId === 65);
    const index = region.selectedDescriptors.indexOf('0x0000000000010e58');
    assert.notEqual(index, -1, 'mutation fixture descriptor');
    region.selectedDescriptors[index] = '0x0000000000010cb8';
  });
  assert.throws(() => verifyEvidence(root), /disappeared target set/);
});

test('changed-entry count cannot hide a new history difference', t => {
  const root = copyEvidence(t);
  const comparison = JSON.parse(fs.readFileSync(path.join(root, 'comparison.json'), 'utf8'));
  const changed = new Set(comparison.changedEntryHistories.map(row => row.entryDescriptor));
  const hot = new Set(comparison.retainedHotHistories.map(row => row.entryDescriptor));
  refreshCandidate(root, report => {
    const region = report.regionDiagnostics.regions.find(row =>
      !changed.has(row.entryDescriptor) && !hot.has(row.entryDescriptor));
    region.frontend = region.frontend === 'a64' ? 'a32' : 'a64';
  });
  assert.throws(() => verifyEvidence(root), /changed entry count/);
});

test('retained hot histories are compared before changed-entry count', t => {
  const root = copyEvidence(t);
  const comparison = JSON.parse(fs.readFileSync(path.join(root, 'comparison.json'), 'utf8'));
  const entry = comparison.retainedHotHistories[0].entryDescriptor;
  refreshCandidate(root, report => {
    const region = report.regionDiagnostics.regions.find(row => row.entryDescriptor === entry);
    region.frontend = region.frontend === 'a64' ? 'a32' : 'a64';
  });
  assert.throws(() => verifyEvidence(root), /hot history changed/);
});

test('candidate patch semantics are pinned beyond paths and targets', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'candidate.patch');
  const patch = fs.readFileSync(file, 'utf8').replace('if (std::find', 'if (true || std::find');
  fs.writeFileSync(file, patch);
  rehash(root);
  assert.throws(() => verifyEvidence(root), /pinned evidence bytes: candidate\.patch/);
});

test('same-count changed-history tampering is rejected', t => {
  const root = copyEvidence(t);
  const comparison = JSON.parse(fs.readFileSync(path.join(root, 'comparison.json'), 'utf8'));
  const entry = comparison.changedEntryHistories[0].entryDescriptor;
  refreshCandidate(root, report => {
    const region = report.regionDiagnostics.regions.find(row => row.entryDescriptor === entry);
    region.frontend = region.frontend === 'a64' ? 'a32' : 'a64';
  });
  assert.throws(() => verifyEvidence(root), /pinned evidence bytes: comparison\.json/);
});
