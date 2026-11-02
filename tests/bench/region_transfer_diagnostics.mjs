const DIAGNOSTIC_SCHEMA = 'dynarmic.js/region-transfer-diagnostics-v1';
const ANALYSIS_SCHEMA = 'dynarmic.js/region-transfer-analysis-v1';
const U64_PATTERN = /^0x[0-9a-f]{16}$/;
const ZERO = '0x0000000000000000';
const MAX_U64 = (1n << 64n) - 1n;
const KINDS = ['external-direct-call', 'rsb-return', 'other-tail-transfer'];
const KIND_FIELDS = {
  'external-direct-call': 'externalDirectCallCount',
  'rsb-return': 'rsbReturnCount',
  'other-tail-transfer': 'otherTailTransferCount',
};
const SNAPSHOT_FIELDS = ['regions', 'schema'];
const REGION_FIELDS = ['callerEntryDescriptor', 'callerEntryPc', 'entryCount', 'exits', 'frontend', 'regionId'];
const EXIT_FIELDS = [
  'count', 'exitBlockDescriptor', 'exitSite', 'kind', 'lastDynamicTargetDescriptor',
  'lastDynamicTargetPc', 'staticTargetDescriptor',
];

function requireCondition(condition, message) {
  if (!condition) throw new Error(`region transfer diagnostics: ${message}`);
}

function requireObject(value, fields, label) {
  requireCondition(value !== null && typeof value === 'object' && !Array.isArray(value), `${label} must be an object`);
  const actual = Object.keys(value).sort();
  requireCondition(actual.length === fields.length && actual.every((field, index) => field === fields[index]),
    `${label} fields must be exactly ${fields.join(', ')}`);
}

function validateU64(value, field) {
  requireCondition(typeof value === 'string' && U64_PATTERN.test(value),
    `${field} must be fixed-width lowercase hexadecimal`);
}

function validateExit(exit, expectedSite) {
  requireObject(exit, EXIT_FIELDS, 'exit');
  requireCondition(Number.isSafeInteger(exit.exitSite) && exit.exitSite === expectedSite,
    `exitSite sequence must be contiguous from 0 at ${expectedSite}`);
  validateU64(exit.exitBlockDescriptor, 'exitBlockDescriptor');
  requireCondition(KINDS.includes(exit.kind), 'exit kind is invalid');
  if (exit.kind === 'external-direct-call') {
    validateU64(exit.staticTargetDescriptor, 'staticTargetDescriptor');
  } else {
    requireCondition(exit.staticTargetDescriptor === null,
      'staticTargetDescriptor must be null unless kind is external-direct-call');
  }
  validateU64(exit.count, 'exit count');
  validateU64(exit.lastDynamicTargetDescriptor, 'lastDynamicTargetDescriptor');
  validateU64(exit.lastDynamicTargetPc, 'lastDynamicTargetPc');
  if (exit.count === ZERO) {
    requireCondition(exit.lastDynamicTargetDescriptor === ZERO && exit.lastDynamicTargetPc === ZERO,
      'zero-count exit must have zero last-target metadata');
  }
}

function validateRegion(region, expectedId) {
  requireObject(region, REGION_FIELDS, 'region');
  requireCondition(Number.isSafeInteger(region.regionId) && region.regionId === expectedId,
    `regionId sequence must be contiguous from 1 at ${expectedId}`);
  requireCondition(region.frontend === 'a32' || region.frontend === 'a64', 'frontend must be a32 or a64');
  validateU64(region.callerEntryDescriptor, 'callerEntryDescriptor');
  validateU64(region.callerEntryPc, 'callerEntryPc');
  validateU64(region.entryCount, 'entryCount');
  requireCondition(Array.isArray(region.exits), 'exits must be an array');
  region.exits.forEach((exit, index) => validateExit(exit, index));
}

function validateSnapshot(snapshot) {
  requireObject(snapshot, SNAPSHOT_FIELDS, 'snapshot');
  requireCondition(snapshot.schema === DIAGNOSTIC_SCHEMA, 'schema mismatch');
  requireCondition(Array.isArray(snapshot.regions), 'regions must be an array');
  snapshot.regions.forEach((region, index) => validateRegion(region, index + 1));
}

function toHex(value) {
  return '0x' + BigInt.asUintN(64, value).toString(16).padStart(16, '0');
}

function sumCounts(values) {
  return toHex(values.reduce((total, value) => total + BigInt(value), 0n));
}

function kindCount(exits, kind) {
  return sumCounts(exits.filter(exit => exit.kind === kind).map(exit => exit.count));
}

function summarizeRegion(region) {
  const kindTotals = Object.fromEntries(KINDS.map(kind => [KIND_FIELDS[kind], kindCount(region.exits, kind)]));
  return {
    regionId: region.regionId,
    frontend: region.frontend,
    callerEntryDescriptor: region.callerEntryDescriptor,
    callerEntryPc: region.callerEntryPc,
    entryCount: region.entryCount,
    exitSiteCount: region.exits.length,
    transferCount: sumCounts(region.exits.map(exit => exit.count)),
    ...kindTotals,
  };
}

function summarizeTotals(regions) {
  return {
    regionCount: regions.length,
    entryCount: sumCounts(regions.map(region => region.entryCount)),
    transferCount: sumCounts(regions.map(region => region.transferCount)),
    ...Object.fromEntries(KINDS.map(kind => {
      const field = KIND_FIELDS[kind];
      return [field, sumCounts(regions.map(region => region[field]))];
    })),
  };
}

// Dynamic counters are published as strings. All arithmetic stays in BigInt and
// has the same modulo-2^64 semantics as the source counters.
export function analyzeRegionTransferDiagnostics(snapshot) {
  validateSnapshot(snapshot);
  const regions = snapshot.regions.map(summarizeRegion);
  return {schema: ANALYSIS_SCHEMA, totals: summarizeTotals(regions), regions};
}

function validateAnalysis(analysis) {
  requireCondition(analysis?.schema === ANALYSIS_SCHEMA, 'analysis schema mismatch');
  for (const field of ['entryCount', 'transferCount']) validateU64(analysis.totals?.[field], `analysis ${field}`);
}

function validateDispatchCount(dispatchCount) {
  const validNumber = typeof dispatchCount === 'number' && Number.isSafeInteger(dispatchCount) && dispatchCount >= 0;
  const validBigInt = typeof dispatchCount === 'bigint' && dispatchCount >= 0n && dispatchCount <= MAX_U64;
  requireCondition(validNumber || validBigInt, 'dispatchCount must be a nonnegative safe integer or uint64 BigInt');
  return toHex(BigInt(dispatchCount));
}

// This is a Run-only hypothesis check. Do not apply it to Step, whose entry and
// dispatcher-return semantics do not establish this conservation identity.
export function checkRunTransferConservation(analysis, dispatchCount) {
  validateAnalysis(analysis);
  const dispatch = validateDispatchCount(dispatchCount);
  const expectedEntryCount = sumCounts([dispatch, analysis.totals.transferCount]);
  return {
    scope: 'Run-only',
    conserved: analysis.totals.entryCount === expectedEntryCount,
    entryCount: analysis.totals.entryCount,
    dispatchCount: dispatch,
    transferCount: analysis.totals.transferCount,
    expectedEntryCount,
  };
}
