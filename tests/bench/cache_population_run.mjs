#!/usr/bin/env node
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {readBuildInfo, sourceInfo, systemInfo, hashFile as hash} from './environment.mjs';
import {populationCases, populationMethodology, summarizePopulation, validatePopulationReport} from './cache_population_protocol.mjs';

if (process.env.DYN_DUMP_DIR) throw new Error('disable DYN_DUMP_DIR during benchmark measurement');
const args = process.argv.slice(2);
const hostDir = path.resolve(args.shift() || 'build-em/tests');
let runs = 5, output = null;
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--runs') runs = Number(args[++i]);
  else if (args[i] === '--output' && args[i + 1]) output = args[++i];
  else throw new Error(`unknown argument: ${args[i]}`);
}
if (!Number.isInteger(runs) || runs < 1 || runs > 100) throw new Error('--runs must be 1..100');
if (output) for (const file of [output, `${output}.failure.json`]) {
  if (fs.existsSync(file)) throw new Error(`report already exists: ${file}`);
}

const {sourceRoot, build} = readBuildInfo(path.dirname(hostDir));
const profile = build.profile;
const hostFile = path.join(hostDir, 'jit_cachepopbench.js');
const wasmFile = path.join(hostDir, 'jit_cachepopbench.wasm');
const workloadFile = path.join(sourceRoot, 'tests/hosts/jit_cachepopbench.cpp');
const workloadSha256 = hash(workloadFile);
const results = populationCases.map(({argv, ...spec}) => ({...spec, runs, workloadSha256,
  artifact: {jsSha256: hash(hostFile), wasmSha256: hash(wasmFile)}, samples: []}));

const observations = [];
let last = null;
try {
  for (let processIndex = 0; processIndex < runs; ++processIndex) {
    for (let offset = 0; offset < populationCases.length; ++offset) {
      const index = (offset + processIndex) % populationCases.length;
      const spec = populationCases[index];
      const result = spawnSync(process.execPath, [hostFile, ...spec.argv], {encoding: 'utf8', timeout: 60000,
        maxBuffer: 8 * 1024 * 1024});
      last = {case: spec.name, process: processIndex, status: result.status, signal: result.signal,
        error: result.error?.message || null, stdout: result.stdout || '', stderr: result.stderr || ''};
      observations.push(last);
      if (result.error || result.status !== 0) throw new Error(`${spec.name}: ${result.error || result.stderr || result.signal}`);
      if (/WRONG|FAIL/.test(result.stdout)) throw new Error(`${spec.name}: guest correctness failure`);
      if (!result.stdout.split('\n').includes('PASS cache population')) throw new Error(`${spec.name}: missing PASS`);
      const lines = result.stdout.split('\n').filter(line => line.startsWith('CACHE_POPULATION '));
      if (lines.length !== 2) throw new Error(`${spec.name}: expected cold/warm samples`);
      results[index].samples.push(...lines.map(line => ({...JSON.parse(line.slice('CACHE_POPULATION '.length)), process: processIndex})));
    }
  }
  if (results[0].artifact.jsSha256 !== hash(hostFile) || results[0].artifact.wasmSha256 !== hash(wasmFile) ||
      workloadSha256 !== hash(workloadFile)) {
    throw new Error('benchmark artifacts or workload changed during measurement');
  }
  results.forEach(item => { item.summary = summarizePopulation(item.samples, profile); });
  const report = {
    schema: 'dynarmic.js/cache-population-v1', timestamp: new Date().toISOString(),
    ...systemInfo(), git: sourceInfo(sourceRoot), build,
    methodology: populationMethodology, cases: results,
  };
  validatePopulationReport(report);
  const json = JSON.stringify(report, null, 2) + '\n';
  if (output) fs.writeFileSync(output, json, {flag: 'wx'}); else process.stdout.write(json);
} catch (error) {
  if (output) fs.writeFileSync(`${output}.failure.json`, JSON.stringify({
    schema: 'dynarmic.js/benchmark-collection-failure-v1', suite: 'cache-population',
    timestamp: new Date().toISOString(), error: error.message, last, observations, completedSamples: results,
  }, null, 2) + '\n', {flag: 'wx'});
  throw error;
}
