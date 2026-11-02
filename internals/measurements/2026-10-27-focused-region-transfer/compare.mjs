import assert from 'node:assert/strict';
import {analyzeRegionDiagnostics, projectRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {analyzeRegionTransferDiagnostics} from '../../../tests/bench/region_transfer_diagnostics.mjs';

const GENERATION_FIELDS = ['regionId', 'frontend', 'entryDescriptor', 'entryPc', 'entryIndex', 'moduleBytes',
  'entryBodyBytes', 'layout', 'blocks', 'liveRegionIdsBeforeCompile'];
const BLOCK_FIELDS = ['descriptor', 'emitted', 'entryReachable', 'returnSite'];
const ownExactFields = (value, fields, label) => {
  assert(value && typeof value === 'object' && !Array.isArray(value), `${label} must be an object`);
  assert.deepEqual(Object.keys(value).sort(), [...fields].sort(), `${label} fields`);
};
const copy = value => structuredClone(value);

export function canonicalGeneration(region) {
  ownExactFields(region, GENERATION_FIELDS, 'generation');
  region.blocks.forEach(block => ownExactFields(block, BLOCK_FIELDS, 'generation block'));
  return {
    frontend: region.frontend,
    entryDescriptor: region.entryDescriptor,
    entryPc: region.entryPc,
    entryIndex: region.entryIndex,
    layout: region.layout,
    blocks: region.blocks.map(block => ({descriptor: block.descriptor, emitted: block.emitted,
      entryReachable: block.entryReachable, returnSite: block.returnSite})),
    moduleBytes: region.moduleBytes,
    entryBodyBytes: region.entryBodyBytes,
  };
}

const generationKey = region => JSON.stringify(canonicalGeneration(region));
const edgeKey = edge => JSON.stringify({
  callerGeneration: edge.callerGeneration,
  exitBlockDescriptor: edge.exitBlockDescriptor,
  kind: edge.kind,
  staticTargetDescriptor: edge.staticTargetDescriptor,
});

function validateHistories(regionSnapshot, transferSnapshot) {
  assert.equal(regionSnapshot?.schema, 'dynarmic.js/region-diagnostics-v2', 'region-diagnostics-v2 required');
  ownExactFields(regionSnapshot, ['schema', 'regions'], 'region diagnostics');
  analyzeRegionDiagnostics(regionSnapshot);
  analyzeRegionTransferDiagnostics(transferSnapshot);
  assert.equal(regionSnapshot.regions.length, transferSnapshot.regions.length,
    'compile/transfer generation counts differ');
  const transfers = new Map(transferSnapshot.regions.map(region => [region.regionId, region]));
  for (const region of regionSnapshot.regions) {
    const transfer = transfers.get(region.regionId);
    assert(transfer, `missing transfer generation ${region.regionId}`);
    assert.equal(transfer.frontend, region.frontend, 'compile/transfer frontend mismatch');
    assert.equal(transfer.callerEntryDescriptor, region.entryDescriptor, 'compile/transfer entry descriptor mismatch');
    assert.equal(transfer.callerEntryPc, region.entryPc, 'compile/transfer entry PC mismatch');
    const descriptors = new Set(region.blocks.map(block => block.descriptor));
    transfer.exits.forEach(exit => assert(descriptors.has(exit.exitBlockDescriptor),
      'exit block is absent from its compile generation'));
  }
}

function generationMap(regions, label) {
  const result = new Map();
  for (const region of regions) {
    const key = generationKey(region);
    assert(!result.has(key), `${label}: duplicate generation identity`);
    result.set(key, region);
  }
  return result;
}

function compareGenerations(baseline, candidate) {
  const before = generationMap(baseline, 'baseline');
  const after = generationMap(candidate, 'candidate');
  const result = {added: [], removed: [], unchanged: []};
  for (const key of new Set([...before.keys(), ...after.keys()])) {
    const left = before.get(key), right = after.get(key);
    const row = {baseline: left ? copy(left) : null, candidate: right ? copy(right) : null};
    if (!left) result.added.push(row);
    else if (!right) result.removed.push(row);
    else result.unchanged.push(row);
  }
  return result;
}

function changedEntries(baseline, candidate) {
  const group = regions => {
    const result = new Map();
    for (const region of regions) {
      if (!result.has(region.entryDescriptor)) result.set(region.entryDescriptor, []);
      result.get(region.entryDescriptor).push(region);
    }
    return result;
  };
  const before = group(baseline), after = group(candidate);
  const histories = [];
  for (const descriptor of new Set([...before.keys(), ...after.keys()])) {
    const left = before.get(descriptor) || [], right = after.get(descriptor) || [];
    const identities = rows => rows.map(generationKey).sort();
    if (JSON.stringify(identities(left)) !== JSON.stringify(identities(right))) {
      histories.push({entryDescriptor: descriptor, baseline: copy(left), candidate: copy(right)});
    }
  }
  return histories;
}

function edgeObservation(region, exit) {
  return {regionId: region.regionId, exitSite: exit.exitSite, count: exit.count,
    lastDynamicTargetDescriptor: exit.lastDynamicTargetDescriptor,
    lastDynamicTargetPc: exit.lastDynamicTargetPc};
}

function edgeMap(regions, transfers, label) {
  const transferById = new Map(transfers.map(region => [region.regionId, region]));
  const result = new Map();
  for (const region of regions) {
    const callerGeneration = canonicalGeneration(region);
    for (const exit of transferById.get(region.regionId).exits) {
      const identity = {callerGeneration, exitBlockDescriptor: exit.exitBlockDescriptor,
        kind: exit.kind, staticTargetDescriptor: exit.staticTargetDescriptor};
      const key = edgeKey(identity);
      assert(!result.has(key), `${label}: duplicate semantic edge identity`);
      result.set(key, {...identity, observation: edgeObservation(region, exit)});
    }
  }
  return result;
}

function compareEdges(baseline, candidate, baselineTransfers, candidateTransfers) {
  const before = edgeMap(baseline, baselineTransfers, 'baseline');
  const after = edgeMap(candidate, candidateTransfers, 'candidate');
  const result = {added: [], removed: [], unchanged: []};
  for (const key of new Set([...before.keys(), ...after.keys()])) {
    const left = before.get(key), right = after.get(key);
    const identity = left || right;
    const row = {callerGeneration: copy(identity.callerGeneration),
      exitBlockDescriptor: identity.exitBlockDescriptor, kind: identity.kind,
      staticTargetDescriptor: identity.staticTargetDescriptor,
      baseline: left ? copy(left.observation) : null, candidate: right ? copy(right.observation) : null};
    if (!left) result.added.push(row);
    else if (!right) result.removed.push(row);
    else {
      assert.equal(left.observation.exitSite, right.observation.exitSite,
        'matched edge on unchanged generation changed exitSite');
      result.unchanged.push({...row, countDelta: (BigInt(right.observation.count) -
        BigInt(left.observation.count)).toString()});
    }
  }
  return result;
}

function emittedBoth(region, source, target) {
  const emitted = new Set(region.blocks.filter(block => block.emitted).map(block => block.descriptor));
  return emitted.has(source) && emitted.has(target);
}

function hasExternalExit(region, transfers, source, target) {
  return transfers.get(region.regionId).exits.some(exit => exit.kind === 'external-direct-call' &&
    exit.exitBlockDescriptor === source && exit.staticTargetDescriptor === target);
}

function newlyExternalized(added, baseline, candidate, baselineTransfers) {
  const beforeTransfers = new Map(baselineTransfers.map(region => [region.regionId, region]));
  const result = [];
  for (const edge of added) {
    if (edge.kind !== 'external-direct-call') continue;
    const source = edge.exitBlockDescriptor, target = edge.staticTargetDescriptor;
    const sameEntry = baseline.filter(region =>
      region.entryDescriptor === edge.callerGeneration.entryDescriptor);
    const qualifying = sameEntry.filter(region => emittedBoth(region, source, target) &&
      !hasExternalExit(region, beforeTransfers, source, target));
    const hasMeasuredExternal = sameEntry.some(region => hasExternalExit(region, beforeTransfers, source, target));
    assert(!(hasMeasuredExternal && qualifying.length > 0), 'mixed baseline internal/external provenance');
    assert(qualifying.length <= 1, 'ambiguous formerly internal edge provenance');
    if (qualifying.length !== 1) continue;
    const candidateRegion = candidate.find(region => region.regionId === edge.candidate.regionId);
    result.push({callerGeneration: copy(edge.callerGeneration),
      exitBlockDescriptor: source, staticTargetDescriptor: target,
      baselineGeneration: copy(qualifying[0]), candidateGeneration: copy(candidateRegion),
      baselineCount: null, candidateCount: edge.candidate.count,
      baselineObservation: 'not-measured-internal-edge'});
  }
  return result;
}

export function compareHistories(baselineRegions, candidateRegions, baselineTransfers, candidateTransfers) {
  validateHistories(baselineRegions, baselineTransfers);
  validateHistories(candidateRegions, candidateTransfers);
  // Validate the v1 projection too; all subsequent identity uses the richer v2 leaves.
  projectRegionDiagnostics(baselineRegions);
  projectRegionDiagnostics(candidateRegions);
  const generations = compareGenerations(baselineRegions.regions, candidateRegions.regions);
  const exits = compareEdges(baselineRegions.regions, candidateRegions.regions,
    baselineTransfers.regions, candidateTransfers.regions);
  return {
    generations,
    changedEntryHistories: changedEntries(baselineRegions.regions, candidateRegions.regions),
    exits,
    newlyExternalizedDirectEdges: newlyExternalized(exits.added, baselineRegions.regions,
      candidateRegions.regions, baselineTransfers.regions),
  };
}
