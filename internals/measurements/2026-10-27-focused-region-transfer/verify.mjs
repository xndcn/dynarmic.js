#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {ANALYSIS_SCHEMA, ANALYSIS_SCOPE, analyzeReports} from './analyze.mjs';

const FILES = ['baseline.json.gz', 'candidate.json.gz', 'analysis.json', 'commands.md'];
export const FROZEN_SHA256 = Object.freeze({
  'analysis.json': '138c7cf3a38209cb33d1bc1131e68c88018730ae1e7649407b35c96f13f41103',
  'baseline.json.gz': 'f2174c0f888ceba20acb7c7209883cf0a1ee760647401be247df06cbffd48747',
  'candidate.json.gz': '31ff55a0a14b7b739272090c1c1fa80a56d712130a73dc322b1092fba483de39',
  'commands.md': '52c15574642912feeca5e3eff028bbbf5298304b7f4f4f2c7e44ed29eb2d0118',
});
const sha256 = data => createHash('sha256').update(data).digest('hex');
const readReport = file => JSON.parse(gunzipSync(fs.readFileSync(file)).toString('utf8'));

function verifyInventory(directory) {
  const text = fs.readFileSync(path.join(directory, 'SHA256SUMS'), 'utf8');
  const entries = text.split('\n').filter(Boolean).map(line => {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    assert(match, 'invalid inventory row');
    const name = match[2];
    assert(!path.isAbsolute(name) && !name.split('/').includes('..'), 'unsafe inventory path');
    return {hash: match[1], name};
  });
  assert.equal(new Set(entries.map(entry => entry.name)).size, entries.length, 'duplicate inventory path');
  assert.deepEqual(entries.map(entry => entry.name).sort(), [...FILES].sort(), 'inventory files must be exact');
  for (const entry of entries) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, entry.name))), entry.hash,
      `inventory hash: ${entry.name}`);
  }
}

function verifyFrozen(directory, frozenSha256) {
  const names = Object.keys(frozenSha256);
  assert(names.length > 0, 'frozen evidence pins pending; real evidence is not installed');
  assert.deepEqual([...names].sort(), [...FILES].sort(), 'frozen pin inventory must be exact');
  for (const name of FILES) {
    const expected = frozenSha256[name];
    assert(typeof expected === 'string' && /^[a-f0-9]{64}$/.test(expected), `invalid frozen pin: ${name}`);
    assert.equal(sha256(fs.readFileSync(path.join(directory, name))), expected,
      `pinned evidence bytes: ${name}`);
  }
}

const ZERO = '0x0000000000000000';
const ENTRY = '0x000000000001202c';
const SOURCE = '0x0000000000012370';
const TARGET = '0x000000000001237c';
const RETURNS = ['0x0000000000012418', '0x0000000000012424', '0x0000000000012428'];
const EXPECTED_TOTALS = {
  baseline: {
    regionCount: 111,
    entryCount: '0x0000000011c62cab',
    transferCount: '0x0000000011c62c00',
    externalDirectCallCount: '0x0000000008b0bb52',
    rsbReturnCount: '0x000000000915709e',
    otherTailTransferCount: '0x0000000000000010',
  },
  candidate: {
    regionCount: 111,
    entryCount: '0x000000000ddd32ab',
    transferCount: '0x000000000ddd3200',
    externalDirectCallCount: '0x0000000006bc3e52',
    rsbReturnCount: '0x000000000720f39e',
    otherTailTransferCount: '0x0000000000000010',
  },
  delta: {
    regionCount: '0', entryCount: '-65600000', transferCount: '-65600000',
    externalDirectCallCount: '-32800000', rsbReturnCount: '-32800000', otherTailTransferCount: '0',
  },
};
const EXPECTED_NEWLY_EXTERNALIZED = [
  ['0x00000000000122f8', '0x0000000000012398', '0x0000000001ab3eff'],
  ['0x000000000001233c', '0x0000000000012398', '0x0000000000493dff'],
  ['0x000000000001233c', '0x0000000000012350', '0x0000000000493dff'],
].map(([exitBlockDescriptor, staticTargetDescriptor, candidateCount]) => ({
  entryDescriptor: ENTRY, exitBlockDescriptor, staticTargetDescriptor, candidateCount,
  baselineCount: null, baselineObservation: 'not-measured-internal-edge',
}));

function verifyRealFocus(analysis) {
  const baselineEdges = analysis.runs.baseline.transferRegions.filter(region =>
    region.callerEntryDescriptor === ENTRY).flatMap(region => region.exits.filter(exit =>
    exit.kind === 'external-direct-call' && exit.exitBlockDescriptor === SOURCE &&
    exit.staticTargetDescriptor === TARGET));
  assert.equal(baselineEdges.length, 1, 'real focused baseline edge count');
  assert.equal(baselineEdges[0].count, '0x0000000002d036ff', 'real focused baseline edge');
  const required = [SOURCE, TARGET, ...RETURNS];
  const focused = analysis.runs.candidate.generations.filter(region => region.entryDescriptor === ENTRY &&
    required.every(descriptor => region.blocks.some(block => block.descriptor === descriptor &&
      block.emitted && block.entryReachable)));
  assert.equal(focused.length, 1, 'real focused candidate generation');
  const transfer = analysis.runs.candidate.transferRegions.find(region => region.regionId === focused[0].regionId);
  assert(transfer && !transfer.exits.some(exit => exit.kind === 'external-direct-call' &&
    exit.exitBlockDescriptor === SOURCE && exit.staticTargetDescriptor === TARGET),
  'real focused candidate external edge must be absent');
}

export function verifyRealAnalysis(analysis) {
  const provenance = analysis.provenance;
  assert.deepEqual({source: provenance.source, upstream: provenance.upstream,
    workloadSha256: provenance.workloadSha256, architecture: provenance.architecture,
    memoryMode: provenance.memoryMode, memoryModeLabel: provenance.memoryModeLabel}, {
    source: 'b0a98606a748d6e4ba9f64bffaa30f823438846b',
    upstream: 'a41c380246d3d9f9874f0f792d234dc0cc17c180',
    workloadSha256: 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa',
    architecture: 'a64', memoryMode: '0', memoryModeLabel: 'callbacks/thunks',
  }, 'real provenance identity');
  assert.deepEqual(provenance.artifacts, {
    baseline: {jsSha256: 'ee7dc93082bcd47a59d2e486dfb8cf6946349d5f82fee78566ec138b3f4f6852',
      wasmSha256: 'a3797eb1111f405007937be4648a4c604b02d74539fa07b7eaf12cff0cc789d5'},
    candidate: {jsSha256: 'ee7dc93082bcd47a59d2e486dfb8cf6946349d5f82fee78566ec138b3f4f6852',
      wasmSha256: '8d6689b7226237a03ce462bdd98bea9f2bb9e85945163128083825a250f7a868'},
  }, 'real artifact identity');
  assert.deepEqual({added: analysis.generations.added.length, removed: analysis.generations.removed.length,
    unchanged: analysis.generations.unchanged.length, changedEntryHistories: analysis.changedEntryHistories.length},
  {added: 3, removed: 3, unchanged: 108, changedEntryHistories: 5}, 'real generation partition');
  const zero = row => row.count === ZERO;
  assert.deepEqual({added: analysis.exits.added.length, removed: analysis.exits.removed.length,
    unchanged: analysis.exits.unchanged.length, newlyExternalized: analysis.newlyExternalizedDirectEdges.length,
    zeroAdded: analysis.exits.added.filter(row => zero(row.candidate)).length,
    zeroRemoved: analysis.exits.removed.filter(row => zero(row.baseline)).length,
    zeroUnchanged: analysis.exits.unchanged.filter(row => zero(row.baseline) && zero(row.candidate)).length},
  {added: 33, removed: 29, unchanged: 439, newlyExternalized: 3,
    zeroAdded: 25, zeroRemoved: 21, zeroUnchanged: 376}, 'real exit partition');
  assert.deepEqual(analysis.wholeRunTotals, EXPECTED_TOTALS, 'real whole-Run totals');
  assert.deepEqual(analysis.newlyExternalizedDirectEdges.map(row => ({
    entryDescriptor: row.callerGeneration.entryDescriptor,
    exitBlockDescriptor: row.exitBlockDescriptor,
    staticTargetDescriptor: row.staticTargetDescriptor,
    candidateCount: row.candidateCount,
    baselineCount: row.baselineCount,
    baselineObservation: row.baselineObservation,
  })), EXPECTED_NEWLY_EXTERNALIZED, 'real newly externalized edges');
  verifyRealFocus(analysis);
}

export function verifyEvidenceContent(directory, {realEvidence = true} = {}) {
  verifyInventory(directory);
  const baseline = readReport(path.join(directory, 'baseline.json.gz'));
  const candidate = readReport(path.join(directory, 'candidate.json.gz'));
  const fresh = analyzeReports(baseline, candidate);
  const retained = JSON.parse(fs.readFileSync(path.join(directory, 'analysis.json'), 'utf8'));
  assert.deepEqual(retained, fresh, 'fresh analysis differs from retained analysis');
  if (realEvidence) verifyRealAnalysis(fresh);
  return {schema: ANALYSIS_SCHEMA, files: FILES.length, scope: ANALYSIS_SCOPE};
}

export function verifyEvidence(directory, frozenSha256 = FROZEN_SHA256, options = {}) {
  verifyFrozen(directory, frozenSha256);
  return verifyEvidenceContent(directory, options);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    assert.equal(process.argv.length, 2, 'usage: verify.mjs');
    const directory = path.join(path.dirname(fileURLToPath(import.meta.url)), 'evidence');
    const result = verifyEvidence(directory);
    console.log(`focused region transfer evidence PASS files=${result.files}; performance not assessed`);
  } catch (error) {
    console.error(`Cannot verify focused region transfer evidence: ${error.message}`);
    process.exitCode = 2;
  }
}
