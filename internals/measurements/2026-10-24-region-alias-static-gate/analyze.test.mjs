import assert from 'node:assert/strict';
import {gunzipSync} from 'node:zlib';
import fs from 'node:fs';
import test from 'node:test';
import {analyzeReports, compareHistories} from './analyze.mjs';

const readGzip = name => JSON.parse(gunzipSync(fs.readFileSync(new URL(name, import.meta.url))));
const before = () => readGzip('./before-a64-flat.json.gz');
const after = () => readGzip('./after-a64-flat.json.gz');
const descriptors = rows => rows.map(row => row.entryDescriptor);

const A = '0x0000000000001000';
const B = '0x0000000000001004';
const C = '0x0000000000001008';
const D = '0x000000000000100c';
const E = '0x0000000000001010';
const block = (descriptor, returnSite = false) =>
  ({descriptor, emitted: true, entryReachable: true, returnSite});
function region(regionId, entryDescriptor, blocks, layout = 'fallback') {
  return {
    regionId, frontend: 'a64', entryDescriptor, entryPc: entryDescriptor,
    entryIndex: blocks.findIndex(row => row.descriptor === entryDescriptor),
    moduleBytes: 100, entryBodyBytes: 80, layout, blocks,
    liveRegionIdsBeforeCompile: Array.from({length: regionId - 1}, (_, index) => index + 1),
  };
}
const history = regions => ({schema: 'dynarmic.js/region-diagnostics-v2', regions});
function syntheticHistories() {
  const baseline = history([
    region(1, A, [block(A), block(B, true)]),
    region(2, B, [block(B), block(C, true)]),
    region(3, C, [block(C)], 'single'),
    region(4, D, [block(D)], 'single'),
  ]);
  const candidate = history([
    region(1, A, [block(A), block(B, true), block(D)]),
    region(2, C, [block(C)], 'single'),
    region(3, E, [block(E)], 'single'),
  ]);
  return {baseline, candidate};
}

test('synthetic mechanism distinguishes reuse, owner dependency, ordinary absorption and new entries', () => {
  const {baseline, candidate} = syntheticHistories();
  const result = compareHistories(baseline, candidate);
  assert.deepEqual(descriptors(result.diagnosis.aliasReusedEntries), [B]);
  assert.deepEqual(descriptors(result.diagnosis.ownerDependencySurvivors), [C]);
  assert.deepEqual(descriptors(result.diagnosis.nonAliasAbsorbedEntries), [D]);
  assert.deepEqual(descriptors(result.diagnosis.newEntries), [E]);
  assert.equal(result.comparison.predictedCoveredLaterEntryCount, 2);
  assert.equal(result.comparison.expectedCandidateRegionCount, 2);
  assert.equal(result.comparison.candidateRegionCount, 3);
  assert.equal(result.comparison.reconciledCandidateRegionCount, 3);
});

test('a live actual alias at original-entry compilation is not an owner-dependency excuse', () => {
  const {baseline, candidate} = syntheticHistories();
  candidate.regions[0].blocks.push(block(C, true));
  assert.throws(() => compareHistories(baseline, candidate), /original entry.*already published/i);
});

test('frozen real evidence fails the registered gate with exact categorized sets', () => {
  const result = analyzeReports(before(), after());
  assert.equal(result.schema, 'dynarmic.js/region-alias-static-gate-v1');
  assert.equal(result.decision, 'STOP_STATIC_GATE');
  assert.deepEqual(result.registeredGate, {
    baselineRegionCount: 111,
    predictedCoveredLaterEntryCount: 16,
    expectedCandidateRegionCount: 95,
  });
  assert.deepEqual(result.comparison, {
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
  });
  assert.deepEqual(descriptors(result.diagnosis.aliasReusedEntries), [
    '0x0000000000010370', '0x000000000001154c', '0x0000000000011700',
    '0x0000000000011288', '0x00000000000100ac', '0x000000000001202c',
    '0x000000000001209c', '0x0000000000010084', '0x0000000000010464',
    '0x00000000000105b8', '0x0000000000010b10', '0x0000000000010b18',
    '0x0000000000013ca4', '0x0000000000012704', '0x000000000001287c',
  ]);
  assert.deepEqual(result.diagnosis.ownerDependencySurvivors.map(row => ({
    entryDescriptor: row.entryDescriptor,
    predictedOwnerRegionIds: row.predictedOwnerRegionIds,
    absorbedOwnerEntryDescriptors: row.absorbedOwnerEntryDescriptors,
  })), [{
    entryDescriptor: '0x0000000000010380',
    predictedOwnerRegionIds: [22],
    absorbedOwnerEntryDescriptors: ['0x0000000000010370'],
  }]);
  assert.deepEqual(descriptors(result.diagnosis.nonAliasAbsorbedEntries), [
    '0x000000000001135c', '0x000000000001238c', '0x00000000000123f8',
    '0x000000000001229c', '0x0000000000012174',
  ]);
  assert.deepEqual(descriptors(result.diagnosis.newEntries), [
    '0x0000000000011698', '0x0000000000011980', '0x000000000001125c',
    '0x0000000000012398', '0x0000000000012350', '0x0000000000012144',
    '0x0000000000010098', '0x0000000000010518',
  ]);
});

test('report validation rejects stale compile totals and duplication', () => {
  const changed = after();
  changed.sample.compile.blocks += 1;
  assert.throws(() => analyzeReports(before(), changed), /compile region total/i);
  const stale = after();
  stale.duplication.totals.moduleBytes += 1;
  assert.throws(() => analyzeReports(before(), stale), /duplication.*stale/i);
});

test('report validation rejects CRC and workload changes even when both sides are coordinated', () => {
  const left = before(), right = after();
  for (const report of [left, right]) {
    report.sample.stdout = report.sample.stdout.replace('crcfinal      : 0x4983', 'crcfinal      : 0xffff');
    report.native.stdout = report.native.stdout.replace('crcfinal      : 0x4983', 'crcfinal      : 0xffff');
  }
  assert.throws(() => analyzeReports(left, right), /guest metrics|CRC/i);
  const otherLeft = before(), otherRight = after();
  otherLeft.workload.iterations = otherRight.workload.iterations = 120000;
  assert.throws(() => analyzeReports(otherLeft, otherRight), /iterations/i);
});

test('report validation binds native and guest compiler flags to the frozen workload', () => {
  for (const side of ['native', 'sample']) {
    const left = before(), right = after();
    for (const report of [left, right]) {
      const output = report[side];
      const oldFlags = output.metrics.compilerFlags;
      output.stdout = output.stdout.replaceAll(oldFlags, '-coordinated-tamper');
      output.metrics.compilerFlags = '-coordinated-tamper';
    }
    assert.throws(() => analyzeReports(left, right), /compiler flags.*workload/i);
  }
});

test('report validation rejects swapped native and guest output', () => {
  const left = before(), right = after();
  for (const report of [left, right]) {
    const {native, sample} = report;
    [native.stdout, sample.stdout] = [sample.stdout, native.stdout];
    [native.metrics, sample.metrics] = [sample.metrics, native.metrics];
  }
  assert.throws(() => analyzeReports(left, right), /compiler flags.*workload/i);
});
