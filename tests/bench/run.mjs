#!/usr/bin/env node
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {readBuildInfo, sourceInfo, systemInfo, hashFile as hash} from './environment.mjs';
import {cases, methodology, parseHostOutput, summarize, validateReport} from './protocol.mjs';

if (process.env.DYN_DUMP_DIR) throw new Error('disable DYN_DUMP_DIR during benchmark measurement');
const args = process.argv.slice(2);
const hostDir = path.resolve(args.shift() || 'build-em/tests');
let runs = 5, output = null;
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--runs') runs = Number(args[++i]);
  else if (args[i] === '--output' && args[i + 1]) output = args[++i];
  else throw new Error(`unknown or incomplete argument: ${args[i]}`);
}
if (!Number.isInteger(runs) || runs < 1 || runs > 100) throw new Error('--runs must be 1..100');
if (output) for (const file of [output, `${output}.failure.json`]) {
  if (fs.existsSync(file)) throw new Error(`report already exists: ${file}`);
}
const {sourceRoot, build} = readBuildInfo(path.dirname(hostDir));
const results = cases.map(spec => ({name: spec.name, architecture: spec.architecture, memoryMode: spec.memoryMode, runs, samples: [],
  workloadSha256: createHash('sha256').update(fs.readFileSync(path.join(sourceRoot, 'tests/hosts', spec.file.replace(/\.js$/, '.cpp'))))
    .update(fs.readFileSync(path.join(sourceRoot, 'tests/hosts/benchmark.h'))).digest('hex'),
  artifact: {jsSha256: hash(path.join(hostDir, spec.file)),
    wasmSha256: hash(path.join(hostDir, spec.file.replace(/\.js$/, '.wasm')))}}));
// Hashes identify the tested files; reject concurrent rebuilds instead of
// attributing a mixture of binaries to one experiment.
const verifyArtifacts = () => results.forEach((item, i) => {
  const spec = cases[i];
  if (item.artifact.jsSha256 !== hash(path.join(hostDir, spec.file)) ||
      item.artifact.wasmSha256 !== hash(path.join(hostDir, spec.file.replace(/\.js$/, '.wasm')))) {
    throw new Error('benchmark artifacts changed during measurement');
  }
});
const observations = [];
let last = null;
try {
  // Interleave case order across fresh processes to reduce systematic drift.
  for (let round = 0; round < runs; ++round) {
    for (let j = 0; j < cases.length; ++j) {
      const index = (j + round) % cases.length;
      const spec = cases[index];
      const result = spawnSync(process.execPath, [path.join(hostDir, spec.file), ...spec.argv], {
        encoding: 'utf8', timeout: 60000, maxBuffer: 16 * 1024 * 1024,
      });
      last = {case: spec.name, process: round, status: result.status, signal: result.signal,
        error: result.error?.message || null, stdout: result.stdout || '', stderr: result.stderr || ''};
      observations.push(last);
      if (result.error || result.status !== 0) throw new Error(`${spec.name}: ${result.error || result.stderr || result.signal}`);
      results[index].samples.push(...parseHostOutput(result.stdout).map(s => ({...s, process: round})));
    }
  }
  verifyArtifacts();
  results.forEach(item => { item.summary = summarize(item.samples); });
  const report = {
    schema: 'dynarmic.js/benchmark-v2', timestamp: new Date().toISOString(),
    ...systemInfo(), git: sourceInfo(sourceRoot),
    build, methodology,
    cases: results,
  };
  validateReport(report);
  const json = JSON.stringify(report, null, 2) + '\n';
  if (output) fs.writeFileSync(output, json, {flag: 'wx'}); else process.stdout.write(json);
} catch (error) {
  if (output) fs.writeFileSync(`${output}.failure.json`, JSON.stringify({
    schema: 'dynarmic.js/benchmark-collection-failure-v1', suite: 'fixed',
    timestamp: new Date().toISOString(), error: error.message, last, observations, completedSamples: results,
  }, null, 2) + '\n', {flag: 'wx'});
  throw error;
}
