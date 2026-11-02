#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {analyzeReports} from './analyze.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const EXPECTED_FILES = ['after-a64-flat.json.gz', 'analysis.json', 'before-a64-flat.json.gz',
  'candidate.patch', 'commands.md'];
const FROZEN_SHA256 = {
  'after-a64-flat.json.gz': 'c0f61ea951e9a6060eb820355b2d35a2fe4f0b5820dcf564a525b83a5fb43ee8',
  'analysis.json': 'e749eaae14a91b236268dc3ea6cdf5eed8cca91d796a8fba1d205be8a786e6ad',
  'before-a64-flat.json.gz': '4c67f84f1b697b85ffea70cebfcd45543ad560e82b2f653c245b35d447044879',
  'candidate.patch': 'ebdd8c57bb153a7831459169c7650bd9a2ed1c60751250af47abe64e237b1887',
};
const ALIAS_REUSED = ['10370', '1154c', '11700', '11288', '100ac', '1202c', '1209c', '10084',
  '10464', '105b8', '10b10', '10b18', '13ca4', '12704', '1287c'];
const NON_ALIAS = ['1135c', '1238c', '123f8', '1229c', '12174'];
const NEW_ENTRIES = ['11698', '11980', '1125c', '12398', '12350', '12144', '10098', '10518'];
const PATCH_PATHS = [
  'src/dynarmic/backend/wasm/a64_address_space.cpp',
  'src/dynarmic/backend/wasm/address_space.cpp',
  'src/dynarmic/backend/wasm/address_space.h',
  'tests/CMakeLists.txt',
  'tests/hosts/a64_region_alias_test.cpp',
  'tests/regress.sh',
];
const sha256 = data => createHash('sha256').update(data).digest('hex');
const readGzip = file => JSON.parse(gunzipSync(fs.readFileSync(file)));
const descriptors = rows => rows.map(row => row.entryDescriptor.replace(/^0x0+/, ''));

function verifyInventory(directory) {
  const lines = fs.readFileSync(path.join(directory, 'SHA256SUMS'), 'utf8').trim().split('\n');
  const entries = lines.map(line => {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    assert(match, `invalid inventory line: ${line}`);
    return {hash: match[1], name: match[2]};
  });
  assert.deepEqual(entries.map(entry => entry.name).sort(), [...EXPECTED_FILES].sort(), 'inventory files');
  for (const entry of entries) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, entry.name))), entry.hash,
      `inventory hash: ${entry.name}`);
  }
}

function verifyPatch(directory) {
  const patch = fs.readFileSync(path.join(directory, 'candidate.patch'), 'utf8');
  const paths = [...patch.matchAll(/^diff --git a\/(.+) b\/(.+)$/gm)].map(match => {
    assert.equal(match[1], match[2], 'candidate patch source/destination differ');
    return match[2];
  });
  assert.deepEqual(paths.sort(), [...PATCH_PATHS].sort(), 'candidate patch paths');
  assert(patch.includes('.enable_a64_fallback_multi_entry = true'), 'candidate A64 enablement missing');
  assert(patch.includes('block_entries.emplace(alias, entry)'), 'candidate no-overwrite publication missing');
}

function verifyContent(directory) {
  const before = readGzip(path.join(directory, 'before-a64-flat.json.gz'));
  const after = readGzip(path.join(directory, 'after-a64-flat.json.gz'));
  const fresh = analyzeReports(before, after);
  const retained = JSON.parse(fs.readFileSync(path.join(directory, 'analysis.json'), 'utf8'));
  assert.deepEqual(retained, fresh, 'fresh analysis differs from retained analysis');
  assert.equal(fresh.decision, 'STOP_STATIC_GATE', 'registered static gate verdict');
  assert.deepEqual(fresh.registeredGate, {
    baselineRegionCount: 111, predictedCoveredLaterEntryCount: 16, expectedCandidateRegionCount: 95,
  });
  assert.deepEqual(fresh.comparison, {
    baselineRegionCount: 111,
    candidateRegionCount: 99,
    netEliminatedRegionCount: 12,
    eliminatedBaselineEntryCount: 20,
    addedCandidateEntryCount: 8,
    predictedCoveredLaterEntryCount: 16,
    expectedCandidateRegionCount: 95,
    reconciledCandidateRegionCount: 99,
    moduleBytesBefore: 708917,
    moduleBytesAfter: 576602,
    moduleBytesDelta: -132315,
  }, 'static gate totals');
  assert.deepEqual(descriptors(fresh.diagnosis.aliasReusedEntries), ALIAS_REUSED, 'actual alias-reuse cohort');
  assert.deepEqual(descriptors(fresh.diagnosis.nonAliasAbsorbedEntries), NON_ALIAS, 'ordinary absorption cohort');
  assert.deepEqual(descriptors(fresh.diagnosis.newEntries), NEW_ENTRIES, 'new candidate entries');
  assert.deepEqual(descriptors(fresh.diagnosis.ownerDependencySurvivors), ['10380'], 'owner-dependency survivor');
  assert.deepEqual(fresh.diagnosis.unanticipatedAliasReusedEntries, [], 'unanticipated alias reuse');
  assert.deepEqual(fresh.diagnosis.publicationReplay, {
    publishedAliasCount: 26, conflictingAliasExposureCount: 9, liveOriginalEntryConflictCount: 0,
  }, 'publication replay totals');
  verifyPatch(directory);
  return {
    baseline: 111, expected: 95, actual: 99,
    aliasReused: fresh.diagnosis.aliasReusedEntries.length,
    added: fresh.diagnosis.newEntries.length,
    files: EXPECTED_FILES.length,
  };
}

export function verifyEvidenceContent(directory) {
  verifyInventory(directory);
  return verifyContent(directory);
}

export function verifyEvidence(directory) {
  verifyInventory(directory);
  for (const [name, expected] of Object.entries(FROZEN_SHA256)) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, name))), expected, `pinned evidence bytes: ${name}`);
  }
  return verifyContent(directory);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  assert(process.argv.length === 2, 'usage: verify.mjs');
  const result = verifyEvidence(here);
  console.log(`region-alias static gate STOP baseline=${result.baseline} expected=${result.expected} ` +
    `actual=${result.actual} aliasReused=${result.aliasReused} added=${result.added} files=${result.files}`);
}
