import assert from 'node:assert/strict';
import test from 'node:test';
import {analyzeRegionDiagnostics, projectRegionDiagnostics} from './region_diagnostics.mjs';

const A = '0x0000000000001000';
const B = '0x0000000000001004';
const C = '0x8000000000001000';

function region(regionId, selectedDescriptors, entryIndex = 0) {
  return {
    regionId,
    frontend: 'a64',
    entryDescriptor: selectedDescriptors[entryIndex],
    entryPc: '0x0000000000001000',
    entryIndex,
    moduleBytes: 100 + regionId,
    entryBodyBytes: 80 + regionId,
    selectedDescriptors,
  };
}

function snapshot(regions) {
  return {schema: 'dynarmic.js/region-diagnostics-v1', regions};
}

function freeze(value) {
  if (value && typeof value === 'object') {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}

test('single region has no duplicated selections', () => {
  const input = freeze(snapshot([region(1, [A])]));
  const result = analyzeRegionDiagnostics(input);
  assert.equal(result.schema, 'dynarmic.js/region-duplication-v1');
  assert.deepEqual(result.totals, {
    regionCount: 1,
    selectedBlockCount: 1,
    uniqueDescriptorCount: 1,
    duplicatedDescriptorCount: 0,
    duplicateSelectionCount: 0,
    moduleBytes: 101,
    entryBodyBytes: 81,
  });
  assert.deepEqual(result.descriptors, [{
    descriptor: A,
    selectedRegionIds: [1],
    entryRegionIds: [1],
    duplicateSelectionCount: 0,
  }]);
  assert.deepEqual(result.regions, [{regionId: 1, duplicateBlockCount: 0, duplicateDescriptors: []}]);
});

test('ordered attribution distinguishes selected and emitted-entry duplication', () => {
  const input = freeze(snapshot([
    region(1, [A, B]),
    region(2, [B, C]),
    region(3, [B]),
  ]));
  const result = analyzeRegionDiagnostics(input);
  assert.deepEqual(result.totals, {
    regionCount: 3,
    selectedBlockCount: 5,
    uniqueDescriptorCount: 3,
    duplicatedDescriptorCount: 1,
    duplicateSelectionCount: 2,
    moduleBytes: 306,
    entryBodyBytes: 246,
  });
  const b = result.descriptors.find(row => row.descriptor === B);
  assert.deepEqual(b, {
    descriptor: B,
    selectedRegionIds: [1, 2, 3],
    entryRegionIds: [2, 3],
    duplicateSelectionCount: 2,
  });
  assert.deepEqual(result.regions[1], {
    regionId: 2,
    duplicateBlockCount: 1,
    duplicateDescriptors: [{descriptor: B, firstSelectedRegionId: 1, previousSelectedRegionId: 1}],
  });
  assert.deepEqual(result.regions[2], {
    regionId: 3,
    duplicateBlockCount: 1,
    duplicateDescriptors: [{descriptor: B, firstSelectedRegionId: 1, previousSelectedRegionId: 2}],
  });
  assert.equal(result.descriptors.find(row => row.descriptor === C).duplicateSelectionCount, 0,
    'same PC bits with a different full descriptor are not a duplicate');
});

test('empty compilation history and invalidation recompilation have explicit totals', () => {
  assert.deepEqual(analyzeRegionDiagnostics(snapshot([])).totals, {
    regionCount: 0,
    selectedBlockCount: 0,
    uniqueDescriptorCount: 0,
    duplicatedDescriptorCount: 0,
    duplicateSelectionCount: 0,
    moduleBytes: 0,
    entryBodyBytes: 0,
  });
  const result = analyzeRegionDiagnostics(snapshot([region(1, [A]), region(2, [A])]));
  assert.equal(result.totals.duplicateSelectionCount, 1);
  assert.deepEqual(result.descriptors[0].entryRegionIds, [1, 2]);
});

for (const [name, mutate] of [
  ['schema', value => value.schema = 'wrong'],
  ['region array', value => value.regions = {}],
  ['region sequence gap', value => value.regions[0].regionId = 2],
  ['fractional region id', value => value.regions[0].regionId = 1.5],
  ['frontend', value => value.regions[0].frontend = 'thumb'],
  ['numeric descriptor', value => value.regions[0].entryDescriptor = 0x20000000000001],
  ['short descriptor', value => value.regions[0].selectedDescriptors[0] = '0x1000'],
  ['uppercase descriptor', value => value.regions[0].selectedDescriptors[0] = '0X0000000000001000'],
  ['entry pc', value => value.regions[0].entryPc = '4096'],
  ['entry index', value => value.regions[0].entryIndex = 1],
  ['entry membership', value => value.regions[0].entryDescriptor = B],
  ['duplicate descriptor within region', value => {
    value.regions[0].selectedDescriptors = [A, A];
    value.regions[0].entryIndex = 0;
  }],
  ['unsafe module bytes', value => value.regions[0].moduleBytes = Number.MAX_SAFE_INTEGER + 1],
  ['zero body bytes', value => value.regions[0].entryBodyBytes = 0],
  ['body exceeds module', value => value.regions[0].entryBodyBytes = 102],
]) test(`reject ${name}`, () => {
  const input = snapshot([region(1, [A])]);
  mutate(input);
  assert.throws(() => analyzeRegionDiagnostics(input), /region|descriptor|schema|frontend|entry|module|body/i);
});

test('rejects duplicate region IDs even when sequence positions differ', () => {
  const input = snapshot([region(1, [A]), region(2, [B])]);
  input.regions[1].regionId = 1;
  assert.throws(() => analyzeRegionDiagnostics(input), /regionId|sequence/);
});

function block(descriptor, emitted = true, entryReachable = true, returnSite = false) {
  return {descriptor, emitted, entryReachable, returnSite};
}

function v2Region(regionId, blocks, {
  entryIndex = 0, layout = 'fallback', liveRegionIdsBeforeCompile = [],
} = {}) {
  return {
    regionId,
    frontend: 'a64',
    entryDescriptor: blocks[entryIndex].descriptor,
    entryPc: '0x0000000000001000',
    entryIndex,
    moduleBytes: 100 + regionId,
    entryBodyBytes: 80 + regionId,
    layout,
    blocks,
    liveRegionIdsBeforeCompile,
  };
}

test('v2 projects every v1 field exactly and retains high-bit descriptor identity', () => {
  const v1 = snapshot([
    region(1, [A, B, C]),
    region(2, [B], 0),
  ]);
  const v2 = {
    schema: 'dynarmic.js/region-diagnostics-v2',
    regions: [
      v2Region(1, [block(A), block(B, true, true, true), block(C)]),
      v2Region(2, [block(B)], {layout: 'single', liveRegionIdsBeforeCompile: [1]}),
    ],
  };
  assert.deepEqual(projectRegionDiagnostics(v2), v1);
  assert.deepEqual(analyzeRegionDiagnostics(v2), analyzeRegionDiagnostics(v1));
  assert.deepEqual(v2.regions[1].liveRegionIdsBeforeCompile, [1]);
});

for (const [name, mutate] of [
  ['block list', value => value.regions[0].blocks = []],
  ['layout', value => value.regions[0].layout = 'unknown'],
  ['single layout with multiple blocks', value => value.regions[0].layout = 'single'],
  ['missing block flag', value => delete value.regions[0].blocks[0].emitted],
  ['non-boolean return-site flag', value => value.regions[0].blocks[0].returnSite = 1],
  ['unemitted entry', value => value.regions[0].blocks[0].emitted = false],
  ['unreachable entry', value => value.regions[0].blocks[0].entryReachable = false],
  ['structured emission mismatch', value => {
    value.regions[0].layout = 'structured';
    value.regions[0].blocks[1].emitted = false;
  }],
  ['fallback unemitted block', value => value.regions[0].blocks[1].emitted = false],
  ['unsorted live IDs', value => value.regions[1].liveRegionIdsBeforeCompile = [1, 1]],
  ['current live ID', value => value.regions[1].liveRegionIdsBeforeCompile = [2]],
  ['unsafe live ID', value => value.regions[1].liveRegionIdsBeforeCompile = [Number.MAX_SAFE_INTEGER + 1]],
]) test(`reject v2 ${name}`, () => {
  const input = {
    schema: 'dynarmic.js/region-diagnostics-v2',
    regions: [
      v2Region(1, [block(A), block(B)]),
      v2Region(2, [block(B)], {layout: 'single', liveRegionIdsBeforeCompile: [1]}),
    ],
  };
  mutate(input);
  assert.throws(() => analyzeRegionDiagnostics(input), /region|block|layout|live|entry|emitted/i);
});
