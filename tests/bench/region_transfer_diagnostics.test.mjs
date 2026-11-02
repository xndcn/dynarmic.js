import assert from 'node:assert/strict';
import test from 'node:test';
import {
  analyzeRegionTransferDiagnostics,
  checkRunTransferConservation,
} from './region_transfer_diagnostics.mjs';

const SCHEMA = 'dynarmic.js/region-transfer-diagnostics-v1';
const A = '0x0000000000001000';
const B = '0x8000000000001004';
const ZERO = '0x0000000000000000';
const MAX = '0xffffffffffffffff';
const hex = value => '0x' + BigInt(value).toString(16).padStart(16, '0');

function exit(exitSite, kind, count, staticTargetDescriptor = null) {
  return {
    exitSite,
    exitBlockDescriptor: exitSite % 2 === 0 ? A : B,
    kind,
    staticTargetDescriptor,
    count: hex(count),
    lastDynamicTargetDescriptor: BigInt(count) === 0n ? ZERO : B,
    lastDynamicTargetPc: BigInt(count) === 0n ? ZERO : A,
  };
}

function region(regionId, entryCount, exits = []) {
  return {
    regionId,
    frontend: regionId % 2 === 0 ? 'a32' : 'a64',
    callerEntryDescriptor: A,
    callerEntryPc: B,
    entryCount: hex(entryCount),
    exits,
  };
}

function snapshot(regions = []) {
  return {schema: SCHEMA, regions};
}

function fixture() {
  return snapshot([
    region(1, '0x20000000000001', [
      exit(0, 'external-direct-call', '0x10000000000001', B),
      exit(1, 'rsb-return', 2),
      exit(2, 'other-tail-transfer', 0),
    ]),
    region(2, 4, [
      exit(0, 'external-direct-call', 3, A),
      exit(1, 'other-tail-transfer', 5),
    ]),
  ]);
}

function freeze(value) {
  if (value && typeof value === 'object') {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}

test('empty history has exact zero totals without Number conversion', () => {
  assert.deepEqual(analyzeRegionTransferDiagnostics(snapshot()), {
    schema: 'dynarmic.js/region-transfer-analysis-v1',
    totals: {
      regionCount: 0,
      entryCount: ZERO,
      transferCount: ZERO,
      externalDirectCallCount: ZERO,
      rsbReturnCount: ZERO,
      otherTailTransferCount: ZERO,
    },
    regions: [],
  });
});

test('analysis retains exact counts and useful per-region attribution', () => {
  const input = freeze(fixture());
  const result = analyzeRegionTransferDiagnostics(input);
  assert.deepEqual(result.totals, {
    regionCount: 2,
    entryCount: '0x0020000000000005',
    transferCount: '0x001000000000000b',
    externalDirectCallCount: '0x0010000000000004',
    rsbReturnCount: '0x0000000000000002',
    otherTailTransferCount: '0x0000000000000005',
  });
  assert.deepEqual(result.regions, [
    {
      regionId: 1,
      frontend: 'a64',
      callerEntryDescriptor: A,
      callerEntryPc: B,
      entryCount: '0x0020000000000001',
      exitSiteCount: 3,
      transferCount: '0x0010000000000003',
      externalDirectCallCount: '0x0010000000000001',
      rsbReturnCount: '0x0000000000000002',
      otherTailTransferCount: ZERO,
    },
    {
      regionId: 2,
      frontend: 'a32',
      callerEntryDescriptor: A,
      callerEntryPc: B,
      entryCount: '0x0000000000000004',
      exitSiteCount: 2,
      transferCount: '0x0000000000000008',
      externalDirectCallCount: '0x0000000000000003',
      rsbReturnCount: ZERO,
      otherTailTransferCount: '0x0000000000000005',
    },
  ]);
  assert.deepEqual(input, fixture());
});

test('aggregate counters wrap modulo 2^64 and never reject overflow', () => {
  const result = analyzeRegionTransferDiagnostics(snapshot([
    region(1, MAX, [exit(0, 'external-direct-call', MAX, B)]),
    region(2, 2, [exit(0, 'external-direct-call', 2, A)]),
  ]));
  assert.equal(result.totals.entryCount, hex(1));
  assert.equal(result.totals.externalDirectCallCount, hex(1));
  assert.equal(result.totals.transferCount, hex(1));
  assert.equal(result.regions[0].transferCount, MAX);
});

test('Run-only conservation reports exact expected and observed entry counts', () => {
  const analysis = analyzeRegionTransferDiagnostics(fixture());
  assert.deepEqual(checkRunTransferConservation(analysis, 0x000ffffffffffffan), {
    scope: 'Run-only',
    conserved: true,
    entryCount: '0x0020000000000005',
    dispatchCount: '0x000ffffffffffffa',
    transferCount: '0x001000000000000b',
    expectedEntryCount: '0x0020000000000005',
  });
  assert.equal(checkRunTransferConservation(analysis, 0x000ffffffffffffbn).conserved, false);
});

test('Run-only conservation handles wrapping and rejects invalid dispatch counts', () => {
  const analysis = analyzeRegionTransferDiagnostics(snapshot([
    region(1, 0, [exit(0, 'rsb-return', MAX)]),
  ]));
  assert.equal(checkRunTransferConservation(analysis, 1).conserved, true);
  for (const dispatchCount of [-1, 1.5, NaN, Infinity, '1', null, undefined,
    Number.MAX_SAFE_INTEGER + 1, 1n << 64n, (1n << 64n) + 1n]) {
    assert.throws(() => checkRunTransferConservation(analysis, dispatchCount), /dispatchCount/);
  }
});

for (const [name, mutate] of [
  ['null snapshot', value => null],
  ['array snapshot', value => []],
  ['schema', value => { value.schema = 'dynarmic.js/region-diagnostics-v2'; }],
  ['missing regions', value => { delete value.regions; }],
  ['non-array regions', value => { value.regions = {}; }],
  ['unknown snapshot field', value => { value.dispatchCount = 0; }],
  ['null region', value => { value.regions[0] = null; }],
  ['array region', value => { value.regions[0] = []; }],
  ['unknown region field', value => { value.regions[0].entryDescriptor = A; }],
  ['missing region field', value => { delete value.regions[0].frontend; }],
  ['region sequence gap', value => { value.regions[0].regionId = 2; }],
  ['duplicate region ID', value => {
    value.regions.push(region(2, 0));
    value.regions[1].regionId = 1;
  }],
  ['fractional region ID', value => { value.regions[0].regionId = 1.5; }],
  ['unsafe region ID', value => { value.regions[0].regionId = Number.MAX_SAFE_INTEGER + 1; }],
  ['frontend', value => { value.regions[0].frontend = 'thumb'; }],
  ['numeric caller descriptor', value => { value.regions[0].callerEntryDescriptor = 4096; }],
  ['uppercase caller PC', value => { value.regions[0].callerEntryPc = '0X0000000000001004'; }],
  ['short caller descriptor', value => { value.regions[0].callerEntryDescriptor = '0x1000'; }],
  ['Unicode caller PC', value => { value.regions[0].callerEntryPc = '0x000000000000100💥'; }],
  ['Number entry count', value => { value.regions[0].entryCount = 1; }],
  ['BigInt entry count', value => { value.regions[0].entryCount = 1n; }],
  ['uppercase entry count', value => { value.regions[0].entryCount = '0x000000000000000A'; }],
  ['oversized entry count', value => { value.regions[0].entryCount = '0x10000000000000000'; }],
  ['non-array exits', value => { value.regions[0].exits = null; }],
  ['null exit', value => { value.regions[0].exits = [null]; }],
  ['array exit', value => { value.regions[0].exits = [[]]; }],
  ['unknown exit field', value => { value.regions[0].exits[0].target = A; }],
  ['missing exit field', value => { delete value.regions[0].exits[0].count; }],
  ['exit site gap', value => { value.regions[0].exits[0].exitSite = 1; }],
  ['second exit site gap', value => { value.regions[0].exits[1].exitSite = 2; }],
  ['fractional exit site', value => { value.regions[0].exits[0].exitSite = 0.5; }],
  ['unsafe exit site', value => { value.regions[0].exits[0].exitSite = Number.MAX_SAFE_INTEGER + 1; }],
  ['exit block descriptor', value => { value.regions[0].exits[0].exitBlockDescriptor = '1000'; }],
  ['kind', value => { value.regions[0].exits[0].kind = 'direct-call'; }],
  ['direct call without static target', value => { value.regions[0].exits[0].staticTargetDescriptor = null; }],
  ['direct call with malformed static target', value => { value.regions[0].exits[0].staticTargetDescriptor = '0X8000000000001004'; }],
  ['RSB return with static target', value => { value.regions[0].exits[1].staticTargetDescriptor = A; }],
  ['other transfer with static target', value => { value.regions[0].exits[2].staticTargetDescriptor = A; }],
  ['Number exit count', value => { value.regions[0].exits[0].count = 1; }],
  ['negative exit count', value => { value.regions[0].exits[0].count = '-0x000000000000001'; }],
  ['last dynamic descriptor', value => { value.regions[0].exits[0].lastDynamicTargetDescriptor = null; }],
  ['last dynamic PC', value => { value.regions[0].exits[0].lastDynamicTargetPc = '0x000000000000100'; }],
  ['last descriptor on a zero-count exit', value => {
    value.regions[0].exits[2].lastDynamicTargetDescriptor = A;
  }],
  ['last PC on a zero-count exit', value => {
    value.regions[0].exits[2].lastDynamicTargetPc = A;
  }],
]) test(`strict validation rejects ${name}`, () => {
  let input = fixture();
  const replacement = mutate(input);
  if (replacement !== undefined) input = replacement;
  assert.throws(() => analyzeRegionTransferDiagnostics(input), /region|transfer|schema|entry|exit|frontend|descriptor|dispatch/i);
});

test('validation handles 10000 regions without approximate aggregation', () => {
  const input = snapshot(Array.from({length: 10000}, (_, index) => region(index + 1, 1)));
  const result = analyzeRegionTransferDiagnostics(input);
  assert.equal(result.totals.regionCount, 10000);
  assert.equal(result.totals.entryCount, hex(10000));
  assert.equal(result.regions.at(-1).regionId, 10000);
});
