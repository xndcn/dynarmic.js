#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {validateEnvironment} from '../../../tests/bench/environment.mjs';
import {coremarkIdentity, parseCoremark, validateCoremarkBuild} from '../../../tests/bench/coremark_protocol.mjs';
import {createCoremarkRegionDiagnosticReport} from '../../../tests/bench/coremark/region_diagnostics.mjs';

const SOURCE = 'ec4701f20d2e53a133367e4048f3f0fc2b981c82';
const UPSTREAM = 'a41c380246d3d9f9874f0f792d234dc0cc17c180';
const WORKLOAD = 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa';
const CASES = [{name: 'a64-flat-window', memoryMode: '0'}, {name: 'a64-page-table', memoryMode: '2'}];
const PROFILE_HOT_PCS = ['0x0000000000010b00', '0x0000000000010b18'];

function readJson(file) {
  const raw = fs.readFileSync(file);
  return JSON.parse(file.endsWith('.gz') ? gunzipSync(raw) : raw);
}

function canonicalReport(report) {
  const {timestamp, node, v8, runtime, host, git, build, workload, ...canonical} = report;
  return canonical;
}

function hotEdge(report) {
  const caller = report.regionTransferDiagnostics.regions.find(region =>
    region.callerEntryPc === '0x000000000001202c');
  assert(caller, 'missing hot direct-edge caller');
  const exits = caller.exits.filter(exit =>
    exit.exitBlockDescriptor === '0x0000000000012370' &&
    exit.staticTargetDescriptor === '0x000000000001237c' &&
    exit.kind === 'external-direct-call');
  assert.equal(exits.length, 1, 'missing or ambiguous hot direct edge');
  return {
    callerEntryPc: caller.callerEntryPc,
    exitBlockDescriptor: exits[0].exitBlockDescriptor,
    staticTargetDescriptor: exits[0].staticTargetDescriptor,
    count: exits[0].count,
  };
}

function analyzeCase(report, spec) {
  validateEnvironment(report);
  assert.equal(report.git.revision, SOURCE, 'source revision mismatch');
  assert.equal(report.git.upstream, UPSTREAM, 'upstream revision mismatch');
  assert.equal(report.git.dirty, false, 'clean source required');
  assert.equal(report.build.type, 'Release', 'non-Release diagnostic host');
  assert.equal(report.build.profile, false, 'unexpected CPU profiling configuration');
  assert.equal(report.architecture, 'a64', 'architecture mismatch');
  assert.equal(report.memoryMode, spec.memoryMode, `${spec.name}: memory mode mismatch`);
  validateCoremarkBuild(report.workload);
  assert.equal(coremarkIdentity(report.workload), WORKLOAD, 'workload identity mismatch');
  assert.equal(report.workloadSha256, WORKLOAD, 'report workload identity mismatch');
  for (const hash of Object.values(report.artifact)) assert(/^[0-9a-f]{64}$/.test(hash), 'invalid artifact hash');

  const nativeMetrics = parseCoremark(report.native.stdout, report.workload.iterations);
  const guestMetrics = parseCoremark(report.sample.stdout, report.workload.iterations);
  assert.deepEqual(report.native.metrics, nativeMetrics, 'native metrics differ');
  assert.deepEqual(report.sample.metrics, guestMetrics, 'guest metrics differ');
  assert.equal(nativeMetrics.compilerFlags, report.workload.flags.native, 'native flags differ');
  assert.equal(guestMetrics.compilerFlags, report.workload.flags.a64, 'guest flags differ');

  const sample = {...report.sample,
    regionDiagnostics: report.regionDiagnostics,
    regionTransferDiagnostics: report.regionTransferDiagnostics};
  const fresh = createCoremarkRegionDiagnosticReport({
    architecture: report.architecture, memoryMode: report.memoryMode,
    workloadSha256: report.workloadSha256, artifact: report.artifact,
    native: report.native, sample,
  });
  assert.deepEqual(canonicalReport(report), fresh, `${spec.name}: report validation differs`);

  const hotRegions = PROFILE_HOT_PCS.map(pc => {
    const summary = report.transferAnalysis.regions.find(region => region.callerEntryPc === pc);
    assert(summary, `${spec.name}: missing profile-hot region ${pc}`);
    const raw = report.regionTransferDiagnostics.regions.find(region => region.regionId === summary.regionId);
    return {...summary, exits: raw.exits};
  });
  const topTransferRegions = [...report.transferAnalysis.regions].sort((a, b) => {
    const left = BigInt(a.transferCount), right = BigInt(b.transferCount);
    return right > left ? 1 : right < left ? -1 : a.regionId - b.regionId;
  }).slice(0, 8);
  return {
    name: spec.name,
    architecture: report.architecture,
    memoryMode: report.memoryMode,
    artifact: report.artifact,
    compile: report.sample.compile,
    totals: report.transferAnalysis.totals,
    hotRegions,
    hotEdge: hotEdge(report),
    topTransferRegions,
  };
}

export function analyzeEvidence(reports) {
  assert.equal(reports.length, CASES.length, 'expected two reports');
  const cases = reports.map((report, index) => analyzeCase(report, CASES[index]));
  assert.deepEqual(cases[0].artifact, cases[1].artifact, 'diagnostic artifacts differ');
  assert.deepEqual(cases[0].totals, cases[1].totals, 'memory-mode transfer totals differ');
  assert.deepEqual(cases[0].hotRegions, cases[1].hotRegions, 'memory-mode hot regions differ');
  assert.deepEqual(cases[0].hotEdge, cases[1].hotEdge, 'memory-mode hot edge differs');
  return {
    schema: 'dynarmic.js/region-transfer-evidence-v1',
    source: SOURCE,
    upstream: UPSTREAM,
    workloadSha256: WORKLOAD,
    cases,
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [flat, page, output, ...extra] = process.argv.slice(2);
  assert(flat && page && output && !extra.length,
    'usage: analyze.mjs <flat-report.json[.gz]> <page-report.json[.gz]> <new-analysis.json>');
  assert(!fs.existsSync(output), `refusing to overwrite ${output}`);
  const analysis = analyzeEvidence([readJson(path.resolve(flat)), readJson(path.resolve(page))]);
  fs.writeFileSync(output, JSON.stringify(analysis, null, 2) + '\n', {flag: 'wx'});
  console.log(`region transfer analysis PASS cases=${analysis.cases.length}`);
}
