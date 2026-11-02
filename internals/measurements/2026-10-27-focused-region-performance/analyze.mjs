#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {validateEnvironment} from '../../../tests/bench/environment.mjs';
import {compileTimers, median} from '../../../tests/bench/protocol.mjs';
import {coremarkIdentity, parseCoremark, summarizeCoremark, validateCoremarkBuild} from '../../../tests/bench/coremark_protocol.mjs';

export const BASELINE_FOCUS = '';
export const CANDIDATE_FOCUS =
  '0x000000000001202c,0x0000000000012370,0x000000000001237c,' +
  '0x0000000000012418,0x0000000000012424,0x0000000000012428';
export const RAW_SCHEMA = 'dynarmic.js/focused-coremark-raw-v1';
export const COMPARISON_SCHEMA = 'dynarmic.js/focused-coremark-comparison-v1';
export const PAIRS = 10;
export const FOCUSED_METHODOLOGY = Object.freeze({
  workload: 'A64 CoreMark, numeric memory mode 0 (callbacks/thunks), one context, fixed iterations',
  schedule: 'exactly 10 predeclared pairs; baseline first in even pairs and candidate first in odd pairs',
  process: 'one fresh process, module and JIT per sample; exactly one Run; no replacement or discarded samples',
  kernel: 'guest timer around upstream iterate; kernelSpeedup is the ratio of baseline/candidate median kernel times',
  run: 'Run wall time includes cold compilation and publication; excludes module loading, RAM setup and JIT construction',
  duration: 'every guest sample must meet the upstream 10-second minimum; this is not CoreMark certification',
  native: 'native output is a CRC oracle only; native timing is not compared with JIT timing',
  dispersion: 'retain every sample; report median, MAD, range, paired ratios and execution-order strata; MAD is not a confidence interval',
});

const sha256 = value => assert(typeof value === 'string' && /^[0-9a-f]{64}$/.test(value), 'invalid SHA256');
const kernelMs = sample => sample.metrics.ticks / 1000;
const roundOrder = pair => pair % 2 ? ['candidate', 'baseline'] : ['baseline', 'candidate'];

function validateProvenance(raw) {
  assert.equal(raw.schema, RAW_SCHEMA, 'focused raw schema');
  validateCoremarkBuild(raw.workload);
  assert.equal(raw.workloadSha256, coremarkIdentity(raw.workload), 'workload identity');
  assert.deepEqual(raw.methodology, FOCUSED_METHODOLOGY, 'methodology');
  assert.equal(raw.architecture, 'a64', 'architecture must be a64');
  assert.equal(raw.memoryMode, '0', 'memoryMode must be string 0');
  assert.equal(raw.runtime.nodeOptions, '', 'NODE_OPTIONS must be empty');
  assert(typeof raw.runtime.cpuAffinity === 'string' && raw.runtime.cpuAffinity.length > 0, 'missing CPU affinity');
  for (const side of ['baseline', 'candidate']) {
    validateEnvironment({timestamp: raw.timestamp, node: raw.node, v8: raw.v8, runtime: raw.runtime,
      host: raw.host, git: raw.git[side], build: raw.build[side]});
    assert.equal(raw.git[side].dirty, false, `${side} source must be clean`);
    assert.equal(raw.build[side].type, 'Release', `${side} must be Release`);
    assert.equal(raw.build[side].profile, false, `${side} profiling must be off`);
    assert.deepEqual(raw.configuration[side], {regionDiagnostics: false, focusTestExceptions: false, examples: true},
      `${side} diagnostics/exceptions/examples configuration`);
    sha256(raw.artifact[side]?.jsSha256);
    sha256(raw.artifact[side]?.wasmSha256);
  }
  assert.equal(raw.build.baseline.a64RegionFocus, BASELINE_FOCUS, 'baseline focus must be empty');
  assert.equal(raw.build.candidate.a64RegionFocus, CANDIDATE_FOCUS, 'candidate focus must be exact');
  const normalized = Object.fromEntries(['baseline', 'candidate'].map(side => {
    const {a64RegionFocus, ...build} = raw.build[side];
    return [side, build];
  }));
  assert.deepEqual(normalized.candidate, normalized.baseline, 'normalized build identity');
  assert.deepEqual(raw.git.candidate, raw.git.baseline, 'source identity');
  assert.equal(raw.artifact.candidate.jsSha256, raw.artifact.baseline.jsSha256, 'JS artifacts must match');
  assert.notEqual(raw.artifact.candidate.wasmSha256, raw.artifact.baseline.wasmSha256,
    'Wasm artifacts must differ for this focus experiment');
}

function validateSample(sample, pair, side, raw, native) {
  assert.equal(sample.process, pair, `${side} process index`);
  assert.equal(sample.haltReason, 0x01000000, `${side} abnormal halt`);
  const parsed = parseCoremark(sample.stdout, raw.workload.iterations);
  assert.deepEqual(sample.metrics, parsed, `${side} parsed output`);
  assert.deepEqual(parsed.crc, native.crc, `${side} native/guest CRC mismatch`);
  assert.equal(parsed.compilerFlags, raw.workload.flags.a64, `${side} compiler flags`);
  assert(parsed.durationValid, `${side} guest run below 10 seconds`);
  assert(Number.isFinite(sample.runMs) && sample.runMs > 0 && sample.runMs < 4294967.296,
    `${side} invalid Run time or timer-wrap ambiguity`);
  assert(sample.runMs + 0.01 >= kernelMs(sample), `${side} kernel time exceeds Run`);
  for (const key of ['blocks', 'bytes', ...compileTimers]) {
    assert(Number.isFinite(sample.compile?.[key]) && sample.compile[key] >= 0, `${side} invalid compile.${key}`);
  }
  for (const key of ['blocks', 'bytes']) {
    assert(Number.isSafeInteger(sample.compile[key]) && sample.compile[key] > 0, `${side} invalid compile.${key}`);
  }
  assert(compileTimers.reduce((sum, key) => sum + sample.compile[key], 0) <= sample.runMs + 0.01,
    `${side} compile phases exceed Run`);
  assert.equal(sample.profile, null, `${side} profile must be null`);
  assert(!Object.hasOwn(sample, 'regionDiagnostics') && !Object.hasOwn(sample, 'regionTransferDiagnostics'),
    `${side} diagnostic payload must be absent`);
}

function distribution(values) {
  const center = median(values);
  const mad = median(values.map(value => Math.abs(value - center)));
  return {median: center, mad, min: Math.min(...values), max: Math.max(...values),
    relativeMad: center === 0 ? null : mad / center};
}

function sideSummary(samples) {
  const summary = summarizeCoremark(samples, false);
  const kernel = distribution(samples.map(kernelMs));
  const run = distribution(samples.map(sample => sample.runMs));
  return {...summary,
    kernelMinMs: kernel.min, kernelMaxMs: kernel.max, kernelRelativeMad: kernel.relativeMad,
    runMinMs: run.min, runMaxMs: run.max, runRelativeMad: run.relativeMad};
}

function gate(metric, baseline, candidate, limit) {
  const latencyRatio = candidate / baseline;
  return {metric, baselineMedianMs: baseline, candidateMedianMs: candidate,
    latencyRatio, speedup: baseline / candidate, limit, pass: latencyRatio <= limit + 1e-12};
}

function pairedMetric(pairs, metric) {
  const value = sample => metric === 'kernel' ? kernelMs(sample) : sample.runMs;
  const observations = pairs.map(pair => {
    const baselineMs = value(pair.baseline), candidateMs = value(pair.candidate);
    return {pair: pair.pair, baselineMs, candidateMs, speedup: baselineMs / candidateMs};
  });
  const ratios = observations.map(item => item.speedup);
  const byOrder = first => observations.filter((item, index) => roundOrder(index)[0] === first).map(item => item.speedup);
  return {observations, ...distribution(ratios),
    outcomes: {
      candidateFaster: ratios.filter(ratio => ratio > 1).length,
      candidateSlower: ratios.filter(ratio => ratio < 1).length,
      equal: ratios.filter(ratio => ratio === 1).length,
    },
    baselineFirst: {ratios: byOrder('baseline'), ...distribution(byOrder('baseline'))},
    candidateFirst: {ratios: byOrder('candidate'), ...distribution(byOrder('candidate'))}};
}

export function analyzeFocusedCoremark(raw) {
  validateProvenance(raw);
  assert.equal(raw.declaredPairs, PAIRS, 'declared pair count');
  assert(Array.isArray(raw.pairs) && raw.pairs.length === PAIRS, 'pair count');
  const native = parseCoremark(raw.native.stdout, raw.workload.iterations);
  assert.deepEqual(raw.native.metrics, native, 'native parsed output');
  assert.equal(native.compilerFlags, raw.workload.flags.native, 'native compiler flags');
  raw.pairs.forEach((pair, index) => {
    assert.equal(pair.pair, index, 'pair index');
    assert.deepEqual(pair.order, roundOrder(index), 'execution order');
    for (const side of ['baseline', 'candidate']) validateSample(pair[side], index, side, raw, native);
  });
  const samples = Object.fromEntries(['baseline', 'candidate'].map(side => [side, raw.pairs.map(pair => pair[side])]));
  const summaries = Object.fromEntries(['baseline', 'candidate'].map(side => [side, sideSummary(samples[side])]));
  const headline = {
    kernel: gate('kernelMedianMs', summaries.baseline.kernelMedianMs, summaries.candidate.kernelMedianMs, 1 / 0.95),
    run: gate('runMedianMs', summaries.baseline.runMedianMs, summaries.candidate.runMedianMs, 1.15),
  };
  const compileBaseline = summaries.baseline.compileMedianMs, compileCandidate = summaries.candidate.compileMedianMs;
  const compileRatio = compileBaseline === 0 ? (compileCandidate === 0 ? 1 : null) : compileCandidate / compileBaseline;
  const bytesRatio = summaries.candidate.emittedBytesMedian / summaries.baseline.emittedBytesMedian;
  const secondary = {
    compile: {metric: 'compileMedianMs', baseline: compileBaseline, candidate: compileCandidate,
      ratio: compileRatio, limit: 1.15, pass: compileRatio !== null && compileRatio <= 1.15 + 1e-12},
    emittedBytes: {metric: 'emittedBytesMedian', baseline: summaries.baseline.emittedBytesMedian,
      candidate: summaries.candidate.emittedBytesMedian, ratio: bytesRatio, limit: 1.10, pass: bytesRatio <= 1.10 + 1e-12},
  };
  return {schema: COMPARISON_SCHEMA, workloadSha256: raw.workloadSha256,
    architecture: raw.architecture, memoryMode: raw.memoryMode, pairs: PAIRS, durationQualified: true,
    baseline: summaries.baseline, candidate: summaries.candidate, headline, secondary,
    paired: {kernel: pairedMetric(raw.pairs, 'kernel'), run: pairedMetric(raw.pairs, 'run')},
    pass: [...Object.values(headline), ...Object.values(secondary)].every(item => item.pass)};
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const [input, output, ...extra] = process.argv.slice(2);
    assert(input && output && !extra.length, 'usage: analyze.mjs <raw.json> <new-comparison.json>');
    assert(!fs.existsSync(output), `refusing to overwrite ${output}`);
    const result = analyzeFocusedCoremark(JSON.parse(fs.readFileSync(input, 'utf8')));
    fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n', {flag: 'wx'});
    console.log(`focused CoreMark ${result.pass ? 'PASS' : 'FAIL'} ` +
      `kernelSpeedup=${result.headline.kernel.speedup.toFixed(6)} ` +
      `runSpeedup=${result.headline.run.speedup.toFixed(6)}`);
    process.exitCode = result.pass ? 0 : 1;
  } catch (error) {
    console.error(`Cannot analyze focused CoreMark: ${error.message}`);
    process.exitCode = 2;
  }
}
