#!/usr/bin/env node
// Compile-boundary coverage evidence only; never a throughput comparison.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {analyzeRegionDiagnostics, projectRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {classifyRegionAliasCoverage} from '../../../tests/bench/region_alias_coverage.mjs';
import {coremarkIdentity, parseCoremark} from '../../../tests/bench/coremark_protocol.mjs';

const REPORT_SCHEMA = 'dynarmic.js/coremark-region-diagnostics-v1';
const EVIDENCE_SCHEMA = 'dynarmic.js/region-alias-evidence-v1';

function validateReport(report, diagnosticSchema) {
  assert.equal(report.schema, REPORT_SCHEMA, 'CoreMark report schema');
  assert.equal(report.regionDiagnostics?.schema, diagnosticSchema, 'region diagnostic schema');
  assert.equal(report.architecture, 'a64');
  assert.equal(report.memoryMode, '2');
  assert.equal(report.workload?.iterations, 200000);
  assert.equal(report.workloadSha256, coremarkIdentity(report.workload));
  assert.equal(report.sample?.haltReason, 0x01000000, 'guest halt was not normal');
  assert.equal(report.sample.profile, null, 'diagnostic Run must not be presented as a profiled benchmark');
  assert.equal(report.build?.profile, false);
  for (const hash of [report.artifact?.jsSha256, report.artifact?.wasmSha256]) {
    assert(typeof hash === 'string' && /^[0-9a-f]{64}$/.test(hash), 'invalid artifact hash');
  }
  const native = parseCoremark(report.native?.stdout, report.workload.iterations);
  const guest = parseCoremark(report.sample.stdout, report.workload.iterations);
  assert.deepEqual(report.native.metrics, native, 'native metrics differ from retained stdout');
  assert.deepEqual(report.sample.metrics, guest, 'guest metrics differ from retained stdout');
  assert.deepEqual(guest.crc, native.crc, 'native/guest CRC mismatch');
  const duplication = analyzeRegionDiagnostics(report.regionDiagnostics);
  assert.deepEqual(report.duplication, duplication, 'embedded duplication is stale');
  assert.equal(duplication.totals.regionCount, report.sample.compile.blocks, 'compile region total');
  assert.equal(duplication.totals.moduleBytes, report.sample.compile.bytes, 'compile byte total');
  return duplication;
}

export function analyzeReports(baseline, current) {
  const before = validateReport(baseline, 'dynarmic.js/region-diagnostics-v1');
  const after = validateReport(current, 'dynarmic.js/region-diagnostics-v2');
  assert.deepEqual(projectRegionDiagnostics(current.regionDiagnostics), baseline.regionDiagnostics,
    'v2-to-v1 projection differs from the complete retained history');
  assert.deepEqual(after, before, 'v1/v2 duplication attribution differs');
  assert.equal(current.workloadSha256, baseline.workloadSha256, 'workload identity changed');
  assert.equal(current.git?.upstream, baseline.git?.upstream, 'Dynarmic upstream revision changed');
  const coverage = classifyRegionAliasCoverage(current.regionDiagnostics);
  assert.equal(coverage.totals.regionCount, 111, 'registered region count');
  assert.equal(coverage.totals.laterEntryCount, 82, 'registered later-entry cohort');
  return {
    schema: EVIDENCE_SCHEMA,
    workloadSha256: current.workloadSha256,
    baselineArtifact: baseline.artifact,
    currentArtifact: current.artifact,
    projectionMatchesV1: true,
    coverage,
  };
}

function main(args) {
  assert(args.length === 3 && args.every(Boolean),
    'usage: analyze.mjs <baseline-v1.json> <current-v2.json> <new-analysis.json>');
  const [baselinePath, currentPath, outputPath] = args;
  assert(!fs.existsSync(outputPath), `refusing to overwrite ${outputPath}`);
  const result = analyzeReports(
    JSON.parse(fs.readFileSync(baselinePath, 'utf8')),
    JSON.parse(fs.readFileSync(currentPath, 'utf8')));
  fs.writeFileSync(outputPath, `${JSON.stringify(result, null, 2)}\n`, {flag: 'wx'});
  console.log(`region alias evidence ${result.coverage.decision} ` +
    `covered=${result.coverage.totals.coveredLaterEntryCount}/${result.coverage.totals.laterEntryCount}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    console.error(`Cannot analyze region alias evidence: ${error.message}`);
    process.exitCode = 2;
  }
}
