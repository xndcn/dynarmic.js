#!/usr/bin/env node
// Recompute the retained static ablation comparison; this is not a timing gate.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {compareReports} from './analyze.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const EXPECTED_FILES = [
  'baseline.json.gz', 'candidate.json.gz', 'candidate.patch', 'candidate-status.txt',
  'commands.md', 'comparison.json', 'targets.txt',
];
const TARGETS_SHA256 = '50db76050f78e1e9130a89690fc90e125cc330067276d6e1d6aec883413927af';
// Independent expected bytes: refreshing the mutable inventory must not make
// coordinated evidence tampering valid. Semantic checks above this final gate
// still provide focused failures and regression coverage.
const FROZEN_SHA256 = {
  'comparison.json': '0c4cb6dbb93b397fb605a787d1df9e05274eb32318e48310872902050c496b29',
  'candidate.patch': '3a1c2849f8c39ab51307191dd46870f39521b3db41986a2c067e8468ce4483b0',
  'targets.txt': TARGETS_SHA256,
  'baseline.json.gz': 'a3b23ff7102d3630f7b29b855a61fbb790d2e260cafda16bc57f71bc74e89a75',
  'candidate.json.gz': '095be8434b9e2e61cd2d27ab11c2b02ca2b6fe5be7ac5926dd81ab4eb158c342',
  'candidate-status.txt': '7e6f4307fc718c209ac501e5ee4878a680b53686360917ea770d8871972913ec',
  'commands.md': '6da333486396ff1c3cf2db5f7d520affffa8c86c89fe86d8bbdc9e47ebf593dd',
};
const HOT_ENTRIES = [
  '0x0000000000010b00', '0x0000000000010b18', '0x0000000000011700',
  '0x0000000000011fd0', '0x000000000001202c', '0x000000000001209c',
];
const DISAPPEARED_TARGETS = [
  '0x0000000000010cb8', '0x0000000000010edc', '0x0000000000010f10',
  '0x0000000000010f4c', '0x0000000000011008',
];
const sha256 = data => createHash('sha256').update(data).digest('hex');
const readJson = (root, name) => JSON.parse(fs.readFileSync(path.join(root, name), 'utf8'));
const readGzipJson = (root, name) => JSON.parse(gunzipSync(fs.readFileSync(path.join(root, name))));

function verifyInventory(root) {
  const lines = fs.readFileSync(path.join(root, 'SHA256SUMS'), 'utf8').trim().split('\n');
  const entries = lines.map(line => {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    assert(match, `invalid inventory line: ${line}`);
    return {hash: match[1], name: match[2]};
  });
  assert.deepEqual(entries.map(entry => entry.name).sort(), [...EXPECTED_FILES].sort(), 'inventory files');
  for (const entry of entries) {
    assert.equal(sha256(fs.readFileSync(path.join(root, entry.name))), entry.hash,
      `inventory hash: ${entry.name}`);
  }
}

function verifyPatch(root) {
  const patch = fs.readFileSync(path.join(root, 'candidate.patch'), 'utf8');
  const paths = [...patch.matchAll(/^diff --git a\/(\S+) b\/(\S+)$/gm)]
    .map(match => [match[1], match[2]])
    .sort(([left], [right]) => left.localeCompare(right));
  assert.deepEqual(paths, [
    ['src/dynarmic/backend/wasm/region_selector.cpp', 'src/dynarmic/backend/wasm/region_selector.cpp'],
    ['tests/unit/region_selector_test.cpp', 'tests/unit/region_selector_test.cpp'],
  ], 'candidate patch paths');
  const array = /kProbeSuppressedReturns\{([\s\S]*?)\};/.exec(patch);
  assert(array, 'candidate patch target array missing');
  const targets = [...array[1].matchAll(/0x([0-9a-f]+)/g)]
    .map(match => `0x${match[1].padStart(16, '0')}`);
  const targetData = fs.readFileSync(path.join(root, 'targets.txt'), 'utf8');
  assert.equal(sha256(targetData), TARGETS_SHA256, 'pinned target list');
  const expected = targetData.trim().split('\n');
  assert.deepEqual(targets, expected, 'patch target list');
  assert(patch.includes('Diagnostic-only CoreMark ablation: never a production descriptor policy.'));
  const status = fs.readFileSync(path.join(root, 'candidate-status.txt'), 'utf8');
  assert.equal(status, ' M src/dynarmic/backend/wasm/region_selector.cpp\n' +
    ' M tests/unit/region_selector_test.cpp\n' +
    '?? docs/measurements/2026-10-24-push-rsb-ablation/analyze.mjs\n' +
    '?? docs/measurements/2026-10-24-push-rsb-ablation/analyze.test.mjs\n',
  'candidate source provenance');
}

function verifyFrozenBytes(root) {
  for (const [name, expected] of Object.entries(FROZEN_SHA256)) {
    assert.equal(sha256(fs.readFileSync(path.join(root, name))), expected,
      `pinned evidence bytes: ${name}`);
  }
}

export function verifyEvidence(root) {
  verifyInventory(root);
  verifyPatch(root);
  const baseline = readGzipJson(root, 'baseline.json.gz');
  const candidate = readGzipJson(root, 'candidate.json.gz');
  const fresh = compareReports(baseline, candidate);
  const retained = readJson(root, 'comparison.json');
  assert.deepEqual(retained, fresh, 'fresh comparison differs from retained comparison');
  assert.equal(fresh.schema, 'dynarmic.js/push-rsb-ablation-comparison-v1');
  assert.equal(fresh.verdict, 'PASS', 'static byte screen');
  assert.deepEqual(candidate.regionDiagnostics.regions.map(region => region.regionId),
    baseline.regionDiagnostics.regions.map(region => region.regionId), 'region IDs changed');
  const beforeEntries = baseline.regionDiagnostics.regions.map(region => region.entryDescriptor);
  const afterEntries = candidate.regionDiagnostics.regions.map(region => region.entryDescriptor);
  assert.deepEqual(afterEntries, beforeEntries, 'entry sequence changed');
  assert.deepEqual(fresh.retainedHotHistories.map(row => row.entryDescriptor), HOT_ENTRIES,
    'retained hot entries');
  for (const history of fresh.retainedHotHistories) {
    assert(history.before.length > 0, `missing hot history: ${history.entryDescriptor}`);
    assert.deepEqual(history.after, history.before, `hot history changed: ${history.entryDescriptor}`);
  }
  assert.deepEqual(fresh.totals.delta, {
    regionCount: 0, selectedBlockCount: -969, uniqueDescriptorCount: -7,
    duplicatedDescriptorCount: -54, duplicateSelectionCount: -962,
    moduleBytes: -18397, entryBodyBytes: -18103,
  }, 'static total deltas');
  assert.deepEqual(fresh.targets.before, {selectionCount: 451, duplicateSelectionCount: 423},
    'baseline target totals');
  assert.deepEqual(fresh.targets.descriptors.filter(row => row.after.selectionCount === 0)
    .map(row => row.descriptor), DISAPPEARED_TARGETS, 'disappeared target set');
  assert.deepEqual(fresh.targets.after, {selectionCount: 25, duplicateSelectionCount: 2},
    'candidate target totals');
  assert.equal(fresh.changedEntryHistories.length, 36, 'changed entry count');
  verifyFrozenBytes(root);
  return {
    regions: fresh.totals.after.regionCount,
    changedEntries: fresh.changedEntryHistories.length,
    hotEntries: fresh.retainedHotHistories.length,
    files: EXPECTED_FILES.length,
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  assert(process.argv.length === 2, 'usage: verify.mjs');
  const result = verifyEvidence(here);
  console.log(`push-RSB ablation evidence PASS regions=${result.regions} ` +
    `changedEntries=${result.changedEntries} hotEntries=${result.hotEntries} files=${result.files}`);
}
