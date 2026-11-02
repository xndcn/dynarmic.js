import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {gzipSync, gunzipSync} from 'node:zlib';
import {analyzeRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {analyzeReports} from './analyze.mjs';
import {verifyEvidence, verifyEvidenceContent} from './verify.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const baselinePath = path.resolve(here, '../2026-10-24-region-diagnostics/a64-flat.json.gz');
const sha256 = data => createHash('sha256').update(data).digest('hex');
const readCurrent = root => JSON.parse(gunzipSync(fs.readFileSync(path.join(root, 'a64-flat-v2.json.gz'))));
const readBaseline = () => JSON.parse(gunzipSync(fs.readFileSync(baselinePath)));

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-alias-evidence-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  for (const name of ['a64-flat-v2.json.gz', 'analysis.json', 'commands.md',
    'generated-wasm-comparison.json', 'generated-wasm.tar.gz', 'SHA256SUMS']) {
    fs.copyFileSync(path.join(here, name), path.join(root, name));
  }
  return root;
}

function refreshInventory(root) {
  const names = ['a64-flat-v2.json.gz', 'analysis.json', 'commands.md',
    'generated-wasm-comparison.json', 'generated-wasm.tar.gz'];
  fs.writeFileSync(path.join(root, 'SHA256SUMS'), names.map(name =>
    `${sha256(fs.readFileSync(path.join(root, name)))}  ${name}`).join('\n') + '\n');
}

function writeCurrent(root, value) {
  fs.writeFileSync(path.join(root, 'a64-flat-v2.json.gz'), gzipSync(JSON.stringify(value), {level: 9, mtime: 0}));
  refreshInventory(root);
}

test('retained evidence verifies the exact 16-entry GO cohort', () => {
  const result = verifyEvidence(here);
  assert.deepEqual(result, {regions: 111, laterEntries: 82, covered: 16, files: 5});
});

test('generated-Wasm evidence rejects refreshed metadata and archive tampering', t => {
  for (const name of ['generated-wasm-comparison.json', 'generated-wasm.tar.gz']) {
    const root = fixture(t);
    const file = path.join(root, name);
    if (name.endsWith('.json')) {
      const comparison = JSON.parse(fs.readFileSync(file, 'utf8'));
      comparison.modules[0].bytes += 1;
      fs.writeFileSync(file, JSON.stringify(comparison, null, 2) + '\n');
    } else {
      fs.appendFileSync(file, Buffer.from([0]));
    }
    refreshInventory(root);
    const verify = name.endsWith('.json') ? verifyEvidenceContent : verifyEvidence;
    assert.throws(() => verify(root, baselinePath), /generated|module|archive|pinned evidence bytes/);
  }
});

test('frozen payload hashes precede decompression and archive extraction', t => {
  for (const name of ['a64-flat-v2.json.gz', 'generated-wasm.tar.gz']) {
    const root = fixture(t);
    fs.writeFileSync(path.join(root, name), 'not a compressed archive');
    refreshInventory(root);
    assert.throws(() => verifyEvidence(root, baselinePath), /pinned evidence bytes/);
  }
});

test('stale projection and altered cohort membership survive refreshed inventories', t => {
  for (const mutate of [
    value => { value.regionDiagnostics.regions[0].entryPc = '0x8000000000001000'; },
    value => {
      const region = value.regionDiagnostics.regions[1];
      region.entryDescriptor = '0x8000000000001000';
      region.blocks[region.entryIndex].descriptor = region.entryDescriptor;
      value.duplication = analyzeRegionDiagnostics(value.regionDiagnostics);
    },
  ]) {
    const root = fixture(t), current = readCurrent(root);
    if (mutate.length === 1) mutate(current);
    writeCurrent(root, current);
    assert.throws(() => verifyEvidenceContent(root, baselinePath), /projection|duplication/);
  }
});

test('changed masks, layouts and invalid live generations are rejected', t => {
  for (const [mutate, expected] of [
    [value => { value.regionDiagnostics.regions[0].layout = 'invalid'; }, /layout/],
    [value => {
      const region = value.regionDiagnostics.regions.find(row => row.layout === 'fallback');
      region.blocks[0].emitted = false;
    }, /emit/],
    [value => { value.regionDiagnostics.regions[1].liveRegionIdsBeforeCompile = [2]; }, /live/],
    [value => { value.regionDiagnostics.regions[1].liveRegionIdsBeforeCompile = [1, 1]; }, /live/],
  ]) {
    const root = fixture(t), current = readCurrent(root);
    mutate(current);
    writeCurrent(root, current);
    assert.throws(() => verifyEvidenceContent(root, baselinePath), expected);
  }
});

test('count-preserving owner mutations fail full analysis and coordinated mutation fails frozen hashes', t => {
  const root = fixture(t), current = readCurrent(root);
  const region = current.regionDiagnostics.regions.find(row => row.regionId === 28);
  region.liveRegionIdsBeforeCompile = region.liveRegionIdsBeforeCompile.filter(id => id !== 22);
  writeCurrent(root, current);
  const fresh = analyzeReports(readBaseline(), current);
  assert.equal(fresh.coverage.totals.coveredLaterEntryCount, 16);
  assert.deepEqual(fresh.coverage.laterEntries.find(row => row.regionId === 28).liveEligibleOwnerRegionIds, [23]);
  assert.throws(() => verifyEvidenceContent(root, baselinePath), /fresh analysis/);

  fs.writeFileSync(path.join(root, 'analysis.json'), JSON.stringify(fresh, null, 2) + '\n');
  refreshInventory(root);
  assert.throws(() => verifyEvidence(root, baselinePath), /pinned evidence bytes/);
});
