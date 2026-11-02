#!/usr/bin/env node
// Combine two preregistered CoreMark cohorts without changing their raw reports.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {summarizeCoremark, validateCoremarkReport} from '../coremark_protocol.mjs';
import {compareCoremark} from './compare.mjs';

const identityFields = [
  'node', 'v8', 'runtime', 'host', 'git', 'build', 'artifact', 'workload',
  'workloadSha256', 'methodology', 'collection',
];

function poolReports(original, followup, side) {
  for (const field of identityFields) {
    assert.deepEqual(followup[field], original[field], `followup ${side} ${field} changed`);
  }
  assert.deepEqual(followup.native.metrics.crc, original.native.metrics.crc,
    `followup ${side} native CRC changed`);
  const cases = original.cases.map((originalCase, index) => {
    const followupCase = followup.cases[index];
    assert.equal(followupCase.name, originalCase.name, `followup ${side} case order changed`);
    assert.equal(originalCase.runs, 5, 'pooling requires five-process cohorts');
    assert.equal(followupCase.runs, 5, 'pooling requires five-process cohorts');
    const laterSamples = followupCase.samples.map(sample => ({...sample, process: sample.process + 5}));
    const samples = [...originalCase.samples, ...laterSamples];
    return {...originalCase, runs: 10, samples,
      summary: summarizeCoremark(samples, original.build.profile)};
  });
  const report = {...original, cases, durationQualified: true};
  validateCoremarkReport(report, true);
  return report;
}

export function evaluateNoiseFollowup(original, followup) {
  const originalComparison = compareCoremark(original.before, original.after, true);
  const followupComparison = compareCoremark(followup.before, followup.after, true);
  const pooledReports = Object.fromEntries(['before', 'after'].map(side =>
    [side, poolReports(original[side], followup[side], side)]));
  const pooledComparison = compareCoremark(pooledReports.before, pooledReports.after, true);
  return {originalComparison, followupComparison, pooledReports, pooledComparison,
    pass: followupComparison.pass && pooledComparison.pass};
}

function loadCohort(directory) {
  const reports = Object.fromEntries(['before', 'after'].map(side =>
    [side, JSON.parse(fs.readFileSync(path.join(directory, `${side}.json`), 'utf8'))]));
  const comparison = JSON.parse(fs.readFileSync(path.join(directory, 'comparison.json'), 'utf8'));
  return {reports, comparison};
}

function main() {
  const [originalArg, followupArg, outputArg, ...extra] = process.argv.slice(2);
  assert(originalArg && followupArg && outputArg && !extra.length,
    'usage: noise_followup.mjs <original-dir> <followup-dir> <new-output-dir>');
  assert(!fs.existsSync(outputArg), `refusing to overwrite ${outputArg}`);
  const original = loadCohort(originalArg), followup = loadCohort(followupArg);
  const result = evaluateNoiseFollowup(original.reports, followup.reports);
  assert.deepEqual(original.comparison, result.originalComparison, 'original comparison does not match raw reports');
  assert.deepEqual(followup.comparison, result.followupComparison, 'followup comparison does not match raw reports');
  fs.mkdirSync(outputArg);
  const write = (name, value) => fs.writeFileSync(path.join(outputArg, name),
    JSON.stringify(value, null, 2) + '\n', {flag: 'wx'});
  write('original-comparison.json', result.originalComparison);
  write('followup-comparison.json', result.followupComparison);
  write('pooled-before.json', result.pooledReports.before);
  write('pooled-after.json', result.pooledReports.after);
  write('pooled-comparison.json', result.pooledComparison);
  write('result.json', {schema: 'dynarmic.js/coremark-noise-followup-v1',
    originalPass: result.originalComparison.pass, followupPass: result.followupComparison.pass,
    pooledPass: result.pooledComparison.pass, pass: result.pass});
  console.log(`CoreMark noise follow-up ${result.pass ? 'PASS' : 'FAIL'} ` +
    `original=${result.originalComparison.pass ? 'PASS' : 'FAIL'} ` +
    `followup=${result.followupComparison.pass ? 'PASS' : 'FAIL'} ` +
    `pooled=${result.pooledComparison.pass ? 'PASS' : 'FAIL'}`);
  process.exitCode = result.pass ? 0 : 1;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try { main(); } catch (error) {
    console.error(`Cannot evaluate CoreMark noise follow-up: ${error.message}`);
    process.exitCode = 2;
  }
}
