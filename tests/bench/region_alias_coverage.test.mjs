import assert from 'node:assert/strict';
import test from 'node:test';
import {classifyRegionAliasCoverage} from './region_alias_coverage.mjs';

const A = '0x0000000000001000';
const B = '0x8000000000001004';
const C = '0x0000000000001008';
const D = '0x0000000000001100';

function block(descriptor, returnSite = false) {
  return {descriptor, emitted: true, entryReachable: true, returnSite};
}

function region(regionId, blocks, liveRegionIdsBeforeCompile = [], layout = 'fallback') {
  return {
    regionId,
    frontend: 'a64',
    entryDescriptor: blocks[0].descriptor,
    entryPc: blocks[0].descriptor,
    entryIndex: 0,
    moduleBytes: 100,
    entryBodyBytes: 80,
    layout,
    blocks,
    liveRegionIdsBeforeCompile,
  };
}

function history() {
  return {
    schema: 'dynarmic.js/region-diagnostics-v2',
    regions: [
      region(1, [block(A), block(B, true), block(C, true)]),
      region(2, [block(D), block(B, true)], [1]),
      region(3, [block(A), block(B, true)], [1, 2], 'structured'),
      region(4, [block(B)], [1, 2, 3], 'single'),
      region(5, [block(C)], [2], 'single'),
      region(6, [block(D)], [], 'single'),
    ],
  };
}

test('coverage requires a live eligible fallback return-site owner and counts each entry once', () => {
  const result = classifyRegionAliasCoverage(history(), {minimumCovered: 1});
  assert.equal(result.schema, 'dynarmic.js/region-alias-coverage-v1');
  assert.deepEqual(result.totals, {
    regionCount: 6,
    laterEntryCount: 4,
    coveredLaterEntryCount: 1,
  });
  assert.equal(result.decision, 'GO');
  assert.deepEqual(result.laterEntries.map(row => ({
    regionId: row.regionId,
    previous: row.previouslySelectedRegionIds,
    eligible: row.staticallyEligibleRegionIds,
    live: row.liveEligibleOwnerRegionIds,
    covered: row.covered,
  })), [
    {regionId: 3, previous: [1], eligible: [], live: [], covered: false},
    {regionId: 4, previous: [1, 2, 3], eligible: [1, 2], live: [1, 2], covered: true},
    {regionId: 5, previous: [1], eligible: [1], live: [], covered: false},
    {regionId: 6, previous: [2], eligible: [], live: [], covered: false},
  ]);
});

test('the pre-registered default threshold stops at eight and permits nine', () => {
  const coveredHistory = count => ({
    schema: 'dynarmic.js/region-diagnostics-v2',
    regions: [
      region(1, [block(A), block(B, true)]),
      ...Array.from({length: count}, (_, index) =>
        region(index + 2, [block(B)], [1], 'single')),
    ],
  });
  assert.equal(classifyRegionAliasCoverage(coveredHistory(8)).totals.coveredLaterEntryCount, 8);
  assert.equal(classifyRegionAliasCoverage(coveredHistory(8)).decision, 'STOP');
  assert.equal(classifyRegionAliasCoverage(coveredHistory(9)).totals.coveredLaterEntryCount, 9);
  assert.equal(classifyRegionAliasCoverage(coveredHistory(9)).decision, 'GO');
  assert.throws(() => classifyRegionAliasCoverage(history(), {minimumCovered: 0}), /minimumCovered/);
});

test('an A64 owner cannot cover a later A32 entry', () => {
  const input = {
    schema: 'dynarmic.js/region-diagnostics-v2',
    regions: [
      region(1, [block(A), block(B, true)]),
      region(2, [block(B)], [1], 'single'),
    ],
  };
  input.regions[1].frontend = 'a32';
  const result = classifyRegionAliasCoverage(input, {minimumCovered: 1});
  assert.equal(result.totals.coveredLaterEntryCount, 0);
  assert.equal(result.decision, 'STOP');
});

test('non-A64, original-entry and return-site filters are fail closed', () => {
  const input = history();
  input.regions = input.regions.slice(0, 4);
  input.regions[0].frontend = 'a32';
  input.regions[1].blocks[1].returnSite = false;
  const result = classifyRegionAliasCoverage(input, {minimumCovered: 1});
  assert.deepEqual(result.laterEntries.at(-1).liveEligibleOwnerRegionIds, []);
  assert.equal(result.decision, 'STOP');
});

test('coverage rejects invalid live-generation evidence', () => {
  const input = history();
  input.regions[3].liveRegionIdsBeforeCompile = [4];
  assert.throws(() => classifyRegionAliasCoverage(input), /live|region/i);
});
