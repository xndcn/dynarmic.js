import assert from 'node:assert/strict';
import {gunzipSync} from 'node:zlib';
import fs from 'node:fs';
import test from 'node:test';
import {replayAliasHistory} from './alias_replay.mjs';

const A = '0x0000000000001000';
const B = '0x0000000000001004';
const C = '0x0000000000001008';
const D = '0x000000000000100c';
const STEP_A = '0x0200000000001000';
const STEP_B = '0x0200000000001004';
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

test('publication replay preserves live owners on alias conflicts', () => {
  const result = replayAliasHistory(history([
    region(1, A, [block(A), block(B, true)]),
    region(2, C, [block(C), block(B, true), block(D, true)]),
  ]));
  assert.equal(result.ownerByDescriptor.get(A), 1);
  assert.equal(result.ownerByDescriptor.get(B), 1);
  assert.equal(result.ownerByDescriptor.get(C), 2);
  assert.equal(result.ownerByDescriptor.get(D), 2);
  assert.deepEqual(result.regions[1].aliases, [D]);
  assert.deepEqual(result.regions[1].conflictingAliases,
    [{descriptor: B, ownerRegionId: 1}]);
});

test('single-step fallback regions publish no aliases', () => {
  const result = replayAliasHistory(history([
    region(1, STEP_A, [block(STEP_A), block(STEP_B, true)]),
  ]));
  assert.deepEqual(result.regions[0].aliases, []);
  assert.equal(result.ownerByDescriptor.get(STEP_A), 1);
  assert.equal(result.ownerByDescriptor.has(STEP_B), false);
});

test('an original-entry miss cannot have an already published live alias', () => {
  assert.throws(() => replayAliasHistory(history([
    region(1, A, [block(A), block(B, true)]),
    region(2, B, [block(B)], 'single'),
  ])), /original entry.*already published/i);
});

test('this no-invalidation experiment rejects owner-generation loss', () => {
  const input = history([
    region(1, A, [block(A), block(B, true)]),
    region(2, C, [block(C), block(D, true)]),
  ]);
  input.regions[1].liveRegionIdsBeforeCompile = [];
  assert.throws(() => replayAliasHistory(input), /invalidation|live.*earlier/i);
});

test('real raw return-site exposures are not misreported as published aliases', () => {
  const report = JSON.parse(gunzipSync(fs.readFileSync(
    new URL('./after-a64-flat.json.gz', import.meta.url))));
  const result = replayAliasHistory(report.regionDiagnostics);
  for (const descriptor of ['0x00000000000100c8', '0x0000000000010af8']) {
    assert(result.regions.some(region => region.conflictingAliases
      .some(conflict => conflict.descriptor === descriptor)), `missing raw conflict ${descriptor}`);
    const owner = result.ownerByDescriptor.get(descriptor);
    assert(owner !== undefined, `missing final owner ${descriptor}`);
    assert(result.regions.some(region => region.entryDescriptor === descriptor && region.regionId === owner),
      `${descriptor} must retain its original owner`);
  }
  assert.equal(result.regions.length, 99);
});
