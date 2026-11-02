import assert from 'node:assert/strict';
import test from 'node:test';
import {compareHistories} from './compare.mjs';
import {COMMON_ENTRY, ENTRY, INTERNAL_SOURCE, INTERNAL_TARGET, SOURCE, TARGET, makeReports, refreshReport,
  u64} from './test_fixtures.mjs';

function histories() {
  const {baseline, candidate} = makeReports();
  return {baseline, candidate};
}
function compare(baseline, candidate) {
  return compareHistories(baseline.regionDiagnostics, candidate.regionDiagnostics,
    baseline.regionTransferDiagnostics, candidate.regionTransferDiagnostics);
}
const findEdge = (rows, source, target) => rows.find(row => row.exitBlockDescriptor === source &&
  row.staticTargetDescriptor === target);

test('complete union retains added, removed, unchanged and zero-count exits without mutation', () => {
  const {baseline, candidate} = histories();
  const before = structuredClone(baseline);
  const result = compare(baseline, candidate);
  assert.deepEqual(baseline, before);
  const hot = findEdge(result.exits.removed, SOURCE, TARGET);
  assert.equal(hot.baseline.count, u64(5));
  assert.equal(hot.candidate, null);
  const addedZero = findEdge(result.exits.added, INTERNAL_SOURCE, INTERNAL_TARGET);
  assert.equal(addedZero.baseline, null);
  assert.equal(addedZero.candidate.count, u64(0));
  const unchangedZero = findEdge(result.exits.unchanged, COMMON_ENTRY, '0x0000000000014000');
  assert.equal(unchangedZero.countDelta, '0');
  const changed = result.exits.unchanged.find(row => row.kind === 'rsb-return');
  assert.equal(changed.baseline.count, u64(2));
  assert.equal(changed.candidate.count, u64(3));
  assert.equal(changed.countDelta, '1');
});

test('generation matching ignores region IDs and live IDs but retains raw histories', () => {
  const {baseline, candidate} = histories();
  const renumbered = structuredClone(candidate);
  const length = renumbered.regionDiagnostics.regions.length;
  const ids = new Map(renumbered.regionDiagnostics.regions.map((row, index) => [row.regionId, length - index]));
  renumbered.regionDiagnostics.regions.reverse().forEach((row, index) => {
    row.regionId = index + 1;
    row.liveRegionIdsBeforeCompile = Array.from({length: index}, (_, i) => i + 1);
  });
  renumbered.regionTransferDiagnostics.regions = renumbered.regionTransferDiagnostics.regions.map(row =>
    ({...row, regionId: ids.get(row.regionId)})).sort((a, b) => a.regionId - b.regionId);
  const valid = refreshReport(renumbered);
  const result = compare(candidate, valid);
  assert.equal(result.generations.added.length, 0);
  assert.equal(result.generations.removed.length, 0);
  assert.equal(result.generations.unchanged.length, 3);
  assert(result.generations.unchanged.every(row => row.baseline && row.candidate));
});

test('PC-only aliases do not match generations with different full descriptors', () => {
  const {candidate} = histories();
  const original = structuredClone(candidate);
  candidate.regionDiagnostics.regions[2].entryDescriptor = '0x0000000000015004';
  candidate.regionDiagnostics.regions[2].blocks[0].descriptor = '0x0000000000015004';
  candidate.regionTransferDiagnostics.regions[2].callerEntryDescriptor = '0x0000000000015004';
  candidate.regionTransferDiagnostics.regions[2].exits.forEach(row => {
    row.exitBlockDescriptor = '0x0000000000015004';
  });
  const result = compare(original, refreshReport(candidate));
  assert.equal(result.generations.unchanged.length, 2);
  assert.equal(result.generations.added.length, 1);
  assert.equal(result.generations.removed.length, 1);
  const oldEntry = result.changedEntryHistories.find(row => row.entryDescriptor === COMMON_ENTRY);
  assert.equal(oldEntry.baseline.length, 1);
  assert.equal(oldEntry.candidate.length, 0);
});

test('membership changes and repeated entries retain every changed-entry generation', () => {
  const {baseline, candidate} = histories();
  const result = compare(baseline, candidate);
  assert.equal(result.generations.added.length, 2);
  assert.equal(result.generations.removed.length, 2);
  assert.equal(result.generations.unchanged.length, 1);
  const focused = result.changedEntryHistories.find(row => row.entryDescriptor === ENTRY);
  assert.equal(focused.baseline.length, 1);
  assert.equal(focused.candidate.length, 1);
  const repeated = structuredClone(candidate);
  const extra = structuredClone(repeated.regionDiagnostics.regions[0]);
  extra.regionId = 4;
  extra.moduleBytes += 1;
  extra.entryBodyBytes += 1;
  repeated.regionDiagnostics.regions.push(extra);
  const transfer = structuredClone(repeated.regionTransferDiagnostics.regions[0]);
  transfer.regionId = 4;
  repeated.regionTransferDiagnostics.regions.push(transfer);
  const valid = refreshReport(repeated);
  const withRepeat = compare(baseline, valid);
  assert.equal(withRepeat.changedEntryHistories.find(row => row.entryDescriptor === ENTRY).candidate.length, 2);
});

test('exact duplicate generation identities fail even with different region IDs', () => {
  const {baseline, candidate} = histories();
  const duplicate = structuredClone(candidate.regionDiagnostics.regions[2]);
  duplicate.regionId = 4;
  candidate.regionDiagnostics.regions.push(duplicate);
  const transfer = structuredClone(candidate.regionTransferDiagnostics.regions[2]);
  transfer.regionId = 4;
  candidate.regionTransferDiagnostics.regions.push(transfer);
  assert.throws(() => compare(baseline, refreshReport(candidate)), /duplicate generation identity/);
});

test('duplicate semantic edge identities fail instead of being merged', () => {
  const {baseline, candidate} = histories();
  const duplicate = structuredClone(candidate.regionTransferDiagnostics.regions[2].exits[1]);
  duplicate.exitSite = 2;
  candidate.regionTransferDiagnostics.regions[2].exits.push(duplicate);
  assert.throws(() => compare(baseline, refreshReport(candidate)), /duplicate semantic edge identity/);
});

test('matched edges on unchanged generations must retain their exit sites', () => {
  const {baseline, candidate} = histories();
  [candidate.regionTransferDiagnostics.regions[2].exits[0].exitSite,
    candidate.regionTransferDiagnostics.regions[2].exits[1].exitSite] = [1, 0];
  assert.throws(() => compare(baseline, refreshReport(candidate)), /exitSite/);
});

test('newly externalized direct edges use null baseline and raw zero candidate count', () => {
  const {baseline, candidate} = histories();
  const result = compare(baseline, candidate);
  assert.equal(result.newlyExternalizedDirectEdges.length, 1);
  const row = result.newlyExternalizedDirectEdges[0];
  assert.equal(row.exitBlockDescriptor, INTERNAL_SOURCE);
  assert.equal(row.staticTargetDescriptor, INTERNAL_TARGET);
  assert.equal(row.baselineCount, null);
  assert.equal(row.candidateCount, u64(0));
  assert.equal(row.baselineObservation, 'not-measured-internal-edge');
  assert(findEdge(result.exits.added, INTERNAL_SOURCE, INTERNAL_TARGET));
});

test('ambiguous formerly internal provenance fails closed', () => {
  const {baseline, candidate} = histories();
  const extra = structuredClone(baseline.regionDiagnostics.regions[1]);
  extra.regionId = 4;
  extra.moduleBytes += 1;
  baseline.regionDiagnostics.regions.push(extra);
  const transfer = structuredClone(baseline.regionTransferDiagnostics.regions[1]);
  transfer.regionId = 4;
  baseline.regionTransferDiagnostics.regions.push(transfer);
  assert.throws(() => compare(refreshReport(baseline), candidate), /ambiguous formerly internal edge/);
});

test('A/B/C mixed internal and measured external baseline provenance fails closed', () => {
  const {baseline, candidate} = histories();
  const measured = structuredClone(baseline.regionDiagnostics.regions[1]);
  measured.regionId = 4;
  measured.moduleBytes += 1;
  measured.entryBodyBytes += 1;
  measured.blocks = measured.blocks.filter(block => block.descriptor !== INTERNAL_TARGET);
  baseline.regionDiagnostics.regions.push(measured);
  const transfer = structuredClone(candidate.regionTransferDiagnostics.regions[1]);
  transfer.regionId = 4;
  transfer.entryCount = u64(7);
  transfer.exits[0].count = u64(7);
  transfer.exits[0].lastDynamicTargetDescriptor = INTERNAL_TARGET;
  transfer.exits[0].lastDynamicTargetPc = INTERNAL_TARGET;
  baseline.regionTransferDiagnostics.regions.push(transfer);
  assert.throws(() => compare(refreshReport(baseline), candidate), /mixed baseline internal\/external provenance/);
});

test('removed zero-count exits remain in the complete union', () => {
  const {baseline, candidate} = histories();
  const removed = structuredClone(baseline.regionTransferDiagnostics.regions[0].exits[1]);
  removed.exitSite = 2;
  removed.exitBlockDescriptor = ENTRY;
  removed.count = u64(0);
  removed.lastDynamicTargetDescriptor = u64(0);
  removed.lastDynamicTargetPc = u64(0);
  baseline.regionTransferDiagnostics.regions[0].exits.push(removed);
  const result = compare(refreshReport(baseline), candidate);
  const row = findEdge(result.exits.removed, ENTRY, TARGET);
  assert.equal(row.baseline.count, u64(0));
  assert.equal(row.candidate, null);
});

test('same-kind exits to one target with different full exit descriptors stay distinct', () => {
  const {baseline, candidate} = histories();
  const target = '0x0000000000016000';
  for (const report of [baseline, candidate]) {
    const exits = report.regionTransferDiagnostics.regions[0].exits;
    for (const descriptor of [ENTRY, SOURCE]) {
      const row = structuredClone(exits[0]);
      row.exitSite = exits.length;
      row.exitBlockDescriptor = descriptor;
      row.staticTargetDescriptor = target;
      exits.push(row);
    }
  }
  const result = compare(refreshReport(baseline), refreshReport(candidate));
  const descriptors = rows => rows.filter(row => row.staticTargetDescriptor === target)
    .map(row => row.exitBlockDescriptor).sort();
  assert.deepEqual(descriptors(result.exits.removed), [ENTRY, SOURCE].sort());
  assert.deepEqual(descriptors(result.exits.added), [ENTRY, SOURCE].sort());
});

test('unknown or missing generation, block and exit fields are rejected', () => {
  const mutations = [
    report => { report.regionDiagnostics.regions[0].unknown = true; },
    report => { delete report.regionDiagnostics.regions[0].moduleBytes; },
    report => { report.regionDiagnostics.regions[0].blocks[0].unknown = true; },
    report => { delete report.regionDiagnostics.regions[0].blocks[0].emitted; },
    report => { report.regionTransferDiagnostics.regions[0].exits[0].unknown = true; },
    report => { delete report.regionTransferDiagnostics.regions[0].exits[0].kind; },
  ];
  for (const mutate of mutations) {
    const {baseline, candidate} = histories();
    mutate(baseline);
    assert.throws(() => compare(baseline, candidate), /fields|invalid|missing|must|required/);
  }
});
