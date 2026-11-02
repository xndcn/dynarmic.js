#!/usr/bin/env node
// Static mechanism qualification only; diagnostic timing is never compared.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {analyzeRegionDiagnostics, projectRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {classifyRegionAliasCoverage} from '../../../tests/bench/region_alias_coverage.mjs';
import {coremarkIdentity, parseCoremark} from '../../../tests/bench/coremark_protocol.mjs';
import {validateEnvironment} from '../../../tests/bench/environment.mjs';
import {replayAliasHistory} from './alias_replay.mjs';

const ANALYSIS_SCHEMA = 'dynarmic.js/region-alias-static-gate-v1';
const REPORT_SCHEMA = 'dynarmic.js/coremark-region-diagnostics-v1';
const WORKLOAD_SHA256 = 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa';
const EXPECTED_CRC = {seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '4983'};
const EXPECTED = {
  before: {
    revision: 'f96fe7a0b4bc7a098ef9badf64919930e2cf8a71',
    js: '247964ce64d6c4de94eb09b865faffb7f0737343e30e6787b4cb6009835e94e6',
    wasm: '45125719cde0ae5ef9e8964321aaf88b88ffa108293859638316ecd72d36a76e',
    regions: 111,
    bytes: 708917,
  },
  after: {
    revision: '6a6c6d79b2c21d6590ebda65848d8883f94f8a31',
    js: '247964ce64d6c4de94eb09b865faffb7f0737343e30e6787b4cb6009835e94e6',
    wasm: '157d310d3b1d442105359d4f99db43cce9854518c2a202e66a02da0c76a0c05e',
    regions: 99,
    bytes: 576602,
  },
};

function validateReport(report, side) {
  const expected = EXPECTED[side];
  validateEnvironment(report);
  assert.equal(report.schema, REPORT_SCHEMA, `${side}: report schema`);
  assert.equal(report.regionDiagnostics?.schema, 'dynarmic.js/region-diagnostics-v2', `${side}: diagnostic schema`);
  assert.equal(report.git.revision, expected.revision, `${side}: source revision`);
  assert.equal(report.git.dirty, false, `${side}: dirty source`);
  assert.equal(report.build.type, 'Release', `${side}: build type`);
  assert.equal(report.build.profile, false, `${side}: profiling build`);
  assert.equal(report.architecture, 'a64', `${side}: architecture`);
  assert.equal(report.memoryMode, '2', `${side}: memory mode`);
  assert.equal(report.workload?.iterations, 200000, `${side}: workload iterations`);
  assert.equal(report.workloadSha256, WORKLOAD_SHA256, `${side}: pinned workload identity`);
  assert.equal(report.workloadSha256, coremarkIdentity(report.workload), `${side}: workload manifest hash`);
  assert.equal(report.runtime.nodeOptions, '', `${side}: NODE_OPTIONS`);
  assert.equal(report.runtime.cpuAffinity, '0,2,4,6', `${side}: CPU affinity`);
  assert.equal(report.artifact?.jsSha256, expected.js, `${side}: JavaScript artifact`);
  assert.equal(report.artifact?.wasmSha256, expected.wasm, `${side}: Wasm artifact`);
  assert.equal(report.sample?.haltReason, 0x01000000, `${side}: guest halt was not normal`);
  assert.equal(report.sample.profile, null, `${side}: diagnostic Run is not a profile sample`);

  const native = parseCoremark(report.native?.stdout, 200000);
  const guest = parseCoremark(report.sample.stdout, 200000);
  assert.deepEqual(report.native.metrics, native, `${side}: native metrics differ from stdout`);
  assert.deepEqual(report.sample.metrics, guest, `${side}: guest metrics/CRC differ from stdout`);
  assert.equal(native.compilerFlags, report.workload.flags.native,
    `${side}: native compiler flags differ from workload`);
  assert.equal(guest.compilerFlags, report.workload.flags.a64,
    `${side}: guest compiler flags differ from workload`);
  assert.deepEqual(guest.crc, native.crc, `${side}: native/guest CRC mismatch`);
  assert.deepEqual(guest.crc, EXPECTED_CRC, `${side}: pinned CRC set`);

  const duplication = analyzeRegionDiagnostics(report.regionDiagnostics);
  assert.deepEqual(report.duplication, duplication, `${side}: embedded duplication is stale`);
  assert.equal(duplication.totals.regionCount, report.sample.compile.blocks, `${side}: compile region total`);
  assert.equal(duplication.totals.moduleBytes, report.sample.compile.bytes, `${side}: compile byte total`);
  assert.equal(duplication.totals.regionCount, expected.regions, `${side}: pinned region total`);
  assert.equal(duplication.totals.moduleBytes, expected.bytes, `${side}: pinned byte total`);
  return duplication;
}

export function compareHistories(before, after) {
  projectRegionDiagnostics(before);
  projectRegionDiagnostics(after);
  const coverage = classifyRegionAliasCoverage(before);
  const replay = replayAliasHistory(after);
  const beforeById = new Map(before.regions.map(region => [region.regionId, region]));
  const beforeEntries = new Set(before.regions.map(region => region.entryDescriptor));
  const afterByEntry = new Map(after.regions.map(region => [region.entryDescriptor, region]));
  const coveredById = new Map(coverage.laterEntries.filter(row => row.covered).map(row => [row.regionId, row]));
  const aliasOwner = new Map();
  for (const region of replay.regions) {
    for (const descriptor of region.aliases) aliasOwner.set(descriptor, region.regionId);
  }

  const aliasReusedEntries = [];
  const ownerDependencySurvivors = [];
  const nonAliasAbsorbedEntries = [];
  const unanticipatedAliasReusedEntries = [];
  const unexplainedSurvivors = [];
  const unexplainedDisappearedEntries = [];
  for (const region of before.regions) {
    const descriptor = region.entryDescriptor;
    const covered = coveredById.get(region.regionId);
    const candidateOriginal = afterByEntry.get(descriptor);
    const aliasOwnerRegionId = aliasOwner.get(descriptor);
    if (candidateOriginal) {
      if (!covered) continue;
      const absorbedOwnerEntryDescriptors = covered.liveEligibleOwnerRegionIds
        .map(id => beforeById.get(id).entryDescriptor)
        .filter(ownerDescriptor => !afterByEntry.has(ownerDescriptor));
      if (aliasOwnerRegionId === undefined &&
          absorbedOwnerEntryDescriptors.length === covered.liveEligibleOwnerRegionIds.length) {
        ownerDependencySurvivors.push({
          baselineRegionId: region.regionId,
          candidateRegionId: candidateOriginal.regionId,
          entryDescriptor: descriptor,
          predictedOwnerRegionIds: covered.liveEligibleOwnerRegionIds,
          absorbedOwnerEntryDescriptors,
        });
      } else {
        unexplainedSurvivors.push({baselineRegionId: region.regionId, entryDescriptor: descriptor});
      }
      continue;
    }
    if (aliasOwnerRegionId !== undefined) {
      const row = {baselineRegionId: region.regionId, entryDescriptor: descriptor, aliasOwnerRegionId};
      aliasReusedEntries.push(row);
      if (!covered) unanticipatedAliasReusedEntries.push(row);
      continue;
    }
    const emittingCandidateRegionIds = after.regions.filter(candidate =>
      candidate.blocks.some(block => block.descriptor === descriptor && block.emitted))
      .map(candidate => candidate.regionId);
    if (emittingCandidateRegionIds.length > 0) {
      nonAliasAbsorbedEntries.push({
        baselineRegionId: region.regionId,
        entryDescriptor: descriptor,
        emittingCandidateRegionIds,
      });
    } else {
      unexplainedDisappearedEntries.push({baselineRegionId: region.regionId, entryDescriptor: descriptor});
    }
  }
  const newEntries = after.regions.filter(region => !beforeEntries.has(region.entryDescriptor))
    .map(region => ({candidateRegionId: region.regionId, entryDescriptor: region.entryDescriptor}));
  assert.deepEqual(unexplainedSurvivors, [], `unexplained predicted survivors: ${JSON.stringify(unexplainedSurvivors)}`);
  assert.deepEqual(unexplainedDisappearedEntries, [],
    `disappeared entries without alias or emitted selection: ${JSON.stringify(unexplainedDisappearedEntries)}`);

  const beforeTotals = analyzeRegionDiagnostics(before).totals;
  const afterTotals = analyzeRegionDiagnostics(after).totals;
  const predictedCoveredLaterEntryCount = coverage.totals.coveredLaterEntryCount;
  const expectedCandidateRegionCount = before.regions.length - predictedCoveredLaterEntryCount;
  const reconciledCandidateRegionCount = before.regions.length - aliasReusedEntries.length -
    nonAliasAbsorbedEntries.length + newEntries.length;
  assert.equal(reconciledCandidateRegionCount, after.regions.length, 'candidate region reconciliation');
  return {
    comparison: {
      baselineRegionCount: before.regions.length,
      candidateRegionCount: after.regions.length,
      netEliminatedRegionCount: before.regions.length - after.regions.length,
      eliminatedBaselineEntryCount: aliasReusedEntries.length + nonAliasAbsorbedEntries.length,
      addedCandidateEntryCount: newEntries.length,
      predictedCoveredLaterEntryCount,
      expectedCandidateRegionCount,
      reconciledCandidateRegionCount,
      moduleBytesBefore: beforeTotals.moduleBytes,
      moduleBytesAfter: afterTotals.moduleBytes,
      moduleBytesDelta: afterTotals.moduleBytes - beforeTotals.moduleBytes,
    },
    diagnosis: {
      aliasReusedEntries,
      ownerDependencySurvivors,
      nonAliasAbsorbedEntries,
      newEntries,
      unanticipatedAliasReusedEntries,
      publicationReplay: {
        publishedAliasCount: replay.regions.reduce((sum, region) => sum + region.aliases.length, 0),
        conflictingAliasExposureCount: replay.regions.reduce((sum, region) => sum + region.conflictingAliases.length, 0),
        liveOriginalEntryConflictCount: 0,
      },
    },
  };
}

export function analyzeReports(before, after) {
  validateReport(before, 'before');
  validateReport(after, 'after');
  assert.deepEqual(after.workload, before.workload, 'workload manifests differ');
  assert.equal(after.git.upstream, before.git.upstream, 'Dynarmic upstream changed');
  assert.deepEqual(after.build, before.build, 'build configuration changed');
  assert.deepEqual(after.host, before.host, 'host changed');
  assert.equal(after.node, before.node, 'Node changed');
  assert.equal(after.v8, before.v8, 'V8 changed');
  const {comparison, diagnosis} = compareHistories(before.regionDiagnostics, after.regionDiagnostics);
  return {
    schema: ANALYSIS_SCHEMA,
    decision: comparison.candidateRegionCount === comparison.expectedCandidateRegionCount
      ? 'PASS_STATIC_GATE' : 'STOP_STATIC_GATE',
    registeredGate: {
      baselineRegionCount: comparison.baselineRegionCount,
      predictedCoveredLaterEntryCount: comparison.predictedCoveredLaterEntryCount,
      expectedCandidateRegionCount: comparison.expectedCandidateRegionCount,
    },
    workloadSha256: before.workloadSha256,
    before: {git: before.git, artifact: before.artifact, duplicationTotals: before.duplication.totals},
    after: {git: after.git, artifact: after.artifact, duplicationTotals: after.duplication.totals},
    comparison,
    diagnosis,
  };
}

function main(args) {
  assert(args.length === 3 && args.every(Boolean),
    'usage: analyze.mjs <before.json> <after.json> <new-analysis.json>');
  const [beforePath, afterPath, outputPath] = args;
  assert(!fs.existsSync(outputPath), `refusing to overwrite ${outputPath}`);
  const result = analyzeReports(
    JSON.parse(fs.readFileSync(beforePath, 'utf8')),
    JSON.parse(fs.readFileSync(afterPath, 'utf8')));
  fs.writeFileSync(outputPath, `${JSON.stringify(result, null, 2)}\n`, {flag: 'wx'});
  console.log(`region-alias static gate ${result.decision} baseline=${result.comparison.baselineRegionCount} ` +
    `expected=${result.comparison.expectedCandidateRegionCount} actual=${result.comparison.candidateRegionCount}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    console.error(`Cannot analyze region alias static gate: ${error.message}`);
    process.exitCode = 2;
  }
}
