#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {validateEnvironment} from '../../../tests/bench/environment.mjs';
import {coremarkIdentity, parseCoremark, validateCoremarkBuild} from '../../../tests/bench/coremark_protocol.mjs';
import {createCoremarkRegionDiagnosticReport} from '../../../tests/bench/coremark/region_diagnostics.mjs';
import {analyzeRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {analyzeRegionTransferDiagnostics, checkRunTransferConservation} from '../../../tests/bench/region_transfer_diagnostics.mjs';
import {compileTimers, profileCounters} from '../../../tests/bench/protocol.mjs';
import {compareHistories} from './compare.mjs';

export const BASELINE_FOCUS = '';
export const CANDIDATE_FOCUS = '0x000000000001202c,0x0000000000012370,0x000000000001237c,' +
  '0x0000000000012418,0x0000000000012424,0x0000000000012428';
export const ANALYSIS_SCHEMA = 'dynarmic.js/focused-region-transfer-analysis-v1';
export const ANALYSIS_SCOPE = 'one-Run mechanism accounting; no performance qualification';
const ENTRY = '0x000000000001202c';
const SOURCE = '0x0000000000012370';
const TARGET = '0x000000000001237c';
const RETURNS = ['0x0000000000012418', '0x0000000000012424', '0x0000000000012428'];
const REPORT_FIELDS = ['schema', 'architecture', 'memoryMode', 'workloadSha256', 'artifact', 'native', 'sample',
  'regionDiagnostics', 'duplication', 'regionTransferDiagnostics', 'transferAnalysis', 'transferConservation',
  'timestamp', 'node', 'v8', 'runtime', 'host', 'git', 'build', 'workload'];
const SAMPLE_FIELDS = ['haltReason', 'runMs', 'stdout', 'compile', 'profile', 'metrics'];
const BUILD_FIELDS = ['type', 'profile', 'a64RegionFocus', 'compilerId', 'compilerVersion', 'emscriptenVersion',
  'cxxFlags', 'configCxxFlags', 'linkFlags', 'configLinkFlags'];
const copy = value => structuredClone(value);
function exactFields(value, fields, label) {
  assert(value && typeof value === 'object' && !Array.isArray(value), `${label} must be an object`);
  assert.deepEqual(Object.keys(value).sort(), [...fields].sort(), `${label}: exact report schema required`);
}
function normalizedBuild(build) {
  const {a64RegionFocus, ...normalized} = build;
  return normalized;
}
function canonicalReport(report) {
  const {timestamp, node, v8, runtime, host, git, build, workload, ...canonical} = report;
  return canonical;
}

function validateProfile(profile) {
  exactFields(profile, ['schema', 'enabled', ...profileCounters], 'profile');
  assert.equal(profile.schema, 'dynarmic.js/profile-v1', 'profile-v1 required');
  assert.equal(profile.enabled, true, 'enabled profile-v1 required');
  for (const key of profileCounters) {
    assert(Number.isSafeInteger(profile[key]) && profile[key] >= 0, `invalid profile.${key}`);
  }
  assert.equal(profile.runCount, 1, 'exactly one Run required');
  assert.equal(profile.stepCount, 0, 'zero Steps required');
}

function validateSample(sample, iterations) {
  exactFields(sample, SAMPLE_FIELDS, 'sample');
  assert.equal(sample.haltReason, 0x01000000, 'normal halt required');
  assert(Number.isFinite(sample.runMs) && sample.runMs > 0 && sample.runMs < 4294967.296, 'invalid Run time');
  exactFields(sample.compile, ['blocks', 'bytes', ...compileTimers], 'compile');
  for (const key of ['blocks', 'bytes', ...compileTimers]) {
    assert(Number.isFinite(sample.compile[key]) && sample.compile[key] >= 0, `invalid compile.${key}`);
  }
  for (const key of ['blocks', 'bytes']) {
    assert(Number.isSafeInteger(sample.compile[key]) && sample.compile[key] > 0, `invalid compile.${key}`);
  }
  assert(compileTimers.reduce((sum, key) => sum + sample.compile[key], 0) <= sample.runMs + 0.01,
    'compile phases exceed Run');
  const metrics = parseCoremark(sample.stdout, iterations);
  assert.deepEqual(sample.metrics, metrics, 'guest metrics differ from reparsed stdout');
  validateProfile(sample.profile);
  return metrics;
}

export function validateSingleReport(report, expectedFocus) {
  exactFields(report, REPORT_FIELDS, 'report');
  assert.equal(report.schema, 'dynarmic.js/coremark-region-diagnostics-v1', 'report schema mismatch');
  validateEnvironment(report);
  exactFields(report.artifact, ['jsSha256', 'wasmSha256'], 'artifact');
  exactFields(report.native, ['stdout', 'metrics'], 'native');
  exactFields(report.runtime, ['nodeOptions', 'cpuAffinity'], 'runtime');
  exactFields(report.host, ['platform', 'release', 'arch', 'cpu', 'cpus'], 'host');
  exactFields(report.git, ['revision', 'upstream', 'dirty'], 'git');
  exactFields(report.build, BUILD_FIELDS, 'build');
  assert.equal(report.git.dirty, false, 'clean git required');
  assert.equal(report.build.type, 'Release', 'Release build required');
  assert.equal(report.build.profile, true, 'profile build required');
  assert(Object.hasOwn(report.build, 'a64RegionFocus') && report.build.a64RegionFocus === expectedFocus,
    'exact own focus provenance required');
  assert.equal(report.architecture, 'a64', 'architecture must be a64');
  assert.equal(report.memoryMode, '0', "memoryMode must be exact string '0' (callbacks/thunks)");
  assert.equal(report.runtime.nodeOptions, '', 'runtime nodeOptions must be empty');
  for (const hash of Object.values(report.artifact)) {
    assert(typeof hash === 'string' && /^[a-f0-9]{64}$/.test(hash), 'invalid artifact hash');
  }
  validateCoremarkBuild(report.workload);
  assert.equal(report.workloadSha256, coremarkIdentity(report.workload), 'workload identity mismatch');
  const nativeMetrics = parseCoremark(report.native.stdout, report.workload.iterations);
  assert.deepEqual(report.native.metrics, nativeMetrics, 'native metrics differ from reparsed stdout');
  assert.equal(nativeMetrics.compilerFlags, report.workload.flags.native, 'native compiler flags differ');
  const guestMetrics = validateSample(report.sample, report.workload.iterations);
  assert.equal(guestMetrics.compilerFlags, report.workload.flags.a64, 'guest compiler flags differ');
  assert.deepEqual(guestMetrics.crc, nativeMetrics.crc, 'native/guest CRC mismatch');
  assert.equal(report.regionDiagnostics?.schema, 'dynarmic.js/region-diagnostics-v2',
    'region-diagnostics-v2 required');
  assert(report.regionTransferDiagnostics, 'region transfer diagnostics required');

  const sample = {...report.sample, regionDiagnostics: report.regionDiagnostics,
    regionTransferDiagnostics: report.regionTransferDiagnostics};
  const fresh = createCoremarkRegionDiagnosticReport({architecture: report.architecture,
    memoryMode: report.memoryMode, workloadSha256: report.workloadSha256, artifact: report.artifact,
    native: report.native, sample});
  assert.deepEqual(canonicalReport(report), fresh, 'fresh derived report differs from serialized report');

  const duplication = analyzeRegionDiagnostics(report.regionDiagnostics);
  const transferAnalysis = analyzeRegionTransferDiagnostics(report.regionTransferDiagnostics);
  const transferConservation = checkRunTransferConservation(transferAnalysis, report.sample.profile.dispatchCount);
  assert.equal(transferConservation.conserved, true, 'whole-Run transfer conservation failed');
  assert.deepEqual(report.duplication, duplication, 'serialized duplication differs from fresh derivation');
  assert.deepEqual(report.transferAnalysis, transferAnalysis, 'serialized transfer analysis differs from fresh derivation');
  assert.deepEqual(report.transferConservation, transferConservation,
    'serialized conservation differs from fresh derivation');
  return {duplication, transferAnalysis, transferConservation};
}

function validatePair(baseline, candidate) {
  assert.equal(baseline.git.revision, candidate.git.revision, 'source identity mismatch');
  assert.equal(baseline.git.upstream, candidate.git.upstream, 'upstream identity mismatch');
  assert.deepEqual(baseline.workload, candidate.workload, 'workload identity mismatch');
  assert.equal(baseline.workloadSha256, candidate.workloadSha256, 'workload identity mismatch');
  assert.equal(baseline.architecture, candidate.architecture, 'architecture mismatch');
  assert.equal(baseline.memoryMode, candidate.memoryMode, 'memoryMode mismatch');
  assert.deepEqual(normalizedBuild(baseline.build), normalizedBuild(candidate.build), 'normalized build identity mismatch');
  assert.deepEqual(baseline.host, candidate.host, 'host identity mismatch');
  assert.equal(baseline.node, candidate.node, 'Node identity mismatch');
  assert.equal(baseline.v8, candidate.v8, 'V8 identity mismatch');
  assert.deepEqual(baseline.runtime, candidate.runtime, 'runtime identity/affinity mismatch');
  assert.notEqual(baseline.artifact.wasmSha256, candidate.artifact.wasmSha256,
    'candidate Wasm hash must differ from baseline');
}

function transferRegion(report, regionId) {
  const region = report.regionTransferDiagnostics.regions.find(row => row.regionId === regionId);
  assert(region, 'missing aligned transfer generation');
  return region;
}

function validateFocusMechanism(baseline, candidate) {
  const baselineEdges = baseline.regionTransferDiagnostics.regions.filter(region =>
    region.callerEntryDescriptor === ENTRY).flatMap(region => region.exits.filter(exit =>
    exit.kind === 'external-direct-call' && exit.exitBlockDescriptor === SOURCE &&
    exit.staticTargetDescriptor === TARGET));
  assert.equal(baselineEdges.length, 1, 'expected one baseline focused external direct edge');
  const focused = candidate.regionDiagnostics.regions.filter(region => region.entryDescriptor === ENTRY &&
    region.blocks.some(block => block.descriptor === SOURCE && block.emitted) &&
    region.blocks.some(block => block.descriptor === TARGET && block.emitted));
  assert.equal(focused.length, 1, 'exactly one candidate focused generation must emit source and target');
  const candidateExits = transferRegion(candidate, focused[0].regionId).exits;
  assert(!candidateExits.some(exit => exit.kind === 'external-direct-call' &&
    exit.exitBlockDescriptor === SOURCE && exit.staticTargetDescriptor === TARGET),
  'candidate focused external direct exit must be absent');
  for (const descriptor of RETURNS) {
    assert(focused[0].blocks.some(block => block.descriptor === descriptor &&
      block.emitted && block.entryReachable),
    `missing focused return descriptors: ${descriptor}`);
  }
}

function signedDeltas(baseline, candidate) {
  return Object.fromEntries(Object.keys(baseline).map(key => [key,
    (BigInt(candidate[key]) - BigInt(baseline[key])).toString()]));
}

export function analyzeReports(baseline, candidate) {
  const before = validateSingleReport(baseline, BASELINE_FOCUS);
  const after = validateSingleReport(candidate, CANDIDATE_FOCUS);
  validatePair(baseline, candidate);
  validateFocusMechanism(baseline, candidate);
  const comparison = compareHistories(baseline.regionDiagnostics, candidate.regionDiagnostics,
    baseline.regionTransferDiagnostics, candidate.regionTransferDiagnostics);
  const run = (report, derived) => ({generations: copy(report.regionDiagnostics.regions),
    transferRegions: copy(report.regionTransferDiagnostics.regions),
    transferConservation: copy(derived.transferConservation)});
  return {
    schema: ANALYSIS_SCHEMA,
    scope: ANALYSIS_SCOPE,
    provenance: {
      source: baseline.git.revision, upstream: baseline.git.upstream,
      workloadSha256: baseline.workloadSha256, architecture: baseline.architecture,
      memoryMode: baseline.memoryMode, memoryModeLabel: 'callbacks/thunks',
      node: baseline.node, v8: baseline.v8, host: copy(baseline.host), runtime: copy(baseline.runtime),
      build: copy(normalizedBuild(baseline.build)),
      artifacts: {baseline: copy(baseline.artifact), candidate: copy(candidate.artifact)},
    },
    focus: {baseline: BASELINE_FOCUS, candidate: CANDIDATE_FOCUS},
    runs: {baseline: run(baseline, before), candidate: run(candidate, after)},
    ...comparison,
    wholeRunTotals: {baseline: copy(before.transferAnalysis.totals), candidate: copy(after.transferAnalysis.totals),
      delta: signedDeltas(before.transferAnalysis.totals, after.transferAnalysis.totals)},
  };
}

function readJson(file) {
  const bytes = fs.readFileSync(file);
  return JSON.parse(file.endsWith('.gz') ? gunzipSync(bytes).toString('utf8') : bytes.toString('utf8'));
}

async function main() {
  const [baseline, candidate, output, ...extra] = process.argv.slice(2);
  assert(baseline && candidate && output && !extra.length,
    'usage: analyze.mjs <baseline.json[.gz]> <candidate.json[.gz]> <new-analysis.json>');
  assert(!fs.existsSync(output), `refusing to overwrite ${output}`);
  const analysis = analyzeReports(readJson(path.resolve(baseline)), readJson(path.resolve(candidate)));
  fs.writeFileSync(output, JSON.stringify(analysis, null, 2) + '\n', {flag: 'wx'});
  console.log(`focused region transfer analysis PASS schema=${analysis.schema}; performance not assessed`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    await main();
  } catch (error) {
    console.error(`Cannot analyze focused region transfer: ${error.message}`);
    process.exitCode = 2;
  }
}
