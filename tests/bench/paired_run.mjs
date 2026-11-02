#!/usr/bin/env node
// Interleave two unchanged builds using validated report templates from the
// ordinary runners. Each case still starts a fresh process and retains all
// samples. Source/build provenance is retained from the templates; executable,
// workload, host and runtime identities are verified again before and after.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {cases, parseHostOutput, summarize, validateReport} from './protocol.mjs';
import {compareReports} from './compare.mjs';
import {populationCases, summarizePopulation, validatePopulationReport, validatePopulationSample} from './cache_population_protocol.mjs';
import {comparePopulationReports} from './cache_population_compare.mjs';

function collect() {
  const args = process.argv.slice(2);
  const [suite, beforeDir, afterDir, beforeFile, afterFile, outputDir, option, count] = args;
  assert(['fixed', 'population'].includes(suite) && beforeDir && afterDir && beforeFile && afterFile && outputDir &&
    (args.length === 6 || (args.length === 8 && option === '--runs')),
    'usage: paired_run.mjs <fixed|population> <before-host-dir> <after-host-dir> <before.json> <after.json> <output-dir> [--runs 25]');
  assert(!process.env.DYN_DUMP_DIR, 'disable DYN_DUMP_DIR');
  const runs = count === undefined ? 25 : Number(count);
  assert(Number.isInteger(runs) && runs >= 5 && runs <= 100, '--runs must be 5..100');
  const fixed = suite === 'fixed';
  const specs = fixed ? cases : populationCases.map(spec => ({...spec, file: 'jit_cachepopbench.js'}));
  const validate = fixed ? validateReport : validatePopulationReport;
  const compare = fixed ? compareReports : comparePopulationReports;
  const reports = [beforeFile, afterFile].map(file => JSON.parse(fs.readFileSync(file, 'utf8')));
  reports.forEach(r => validate(r));
  // Check compatibility, not performance: a failing candidate is still useful
  // evidence and must not be excluded from collection.
  compare(...reports);
  assert.equal(reports[0].git.upstream, reports[1].git.upstream, 'upstream revision changed');
  const outputFiles = ['before', 'after', 'comparison'].map(name => path.join(outputDir, `${suite}-${name}.json`));
  const failureFile = path.join(outputDir, `${suite}-failure.json`);
  for (const file of [...outputFiles, failureFile]) assert(!fs.existsSync(file), `refusing to overwrite ${file}`);
  const dirs = [beforeDir, afterDir].map(dir => path.resolve(dir));
  const roots = dirs.map(dir => {
    const cache = fs.readFileSync(path.join(dir, '../CMakeCache.txt'), 'utf8');
    const root = /^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$/m.exec(cache)?.[1];
    assert(root, 'missing configured source directory');
    return root;
  });
  const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  const verify = () => reports.forEach((report, i) => {
    assert.equal(report.node, process.version, 'Node changed');
    assert.equal(report.v8, process.versions.v8, 'V8 changed');
    assert.deepEqual(report.host, {platform: process.platform, release: os.release(), arch: process.arch,
      cpu: os.cpus()[0]?.model, cpus: os.cpus().length}, 'host changed');
    assert.equal(report.runtime.nodeOptions, process.env.NODE_OPTIONS || '', 'NODE_OPTIONS changed');
    assert.equal(report.runtime.cpuAffinity, process.platform === 'linux'
      ? /^Cpus_allowed_list:\s*(.*)$/m.exec(fs.readFileSync('/proc/self/status', 'utf8'))?.[1] || 'unknown'
      : 'unavailable', 'CPU affinity changed');
    for (const spec of specs) {
      const item = report.cases.find(c => c.name === spec.name);
      assert.equal(item.artifact.jsSha256, hash(path.join(dirs[i], spec.file)), `${spec.name}: JS changed`);
      assert.equal(item.artifact.wasmSha256, hash(path.join(dirs[i], spec.file.replace(/\.js$/, '.wasm'))), `${spec.name}: Wasm changed`);
      const workload = createHash('sha256').update(fs.readFileSync(path.join(roots[i], 'tests/hosts', spec.file.replace(/\.js$/, '.cpp'))));
      if (fixed) workload.update(fs.readFileSync(path.join(roots[i], 'tests/hosts/benchmark.h')));
      assert.equal(item.workloadSha256, workload.digest('hex'), `${spec.name}: workload changed`);
    }
  });
  verify();
  reports.forEach(r => {
    r.timestamp = new Date().toISOString();
    r.collection = 'paired builds per case, alternating build order each round; all samples retained';
    r.cases.forEach(c => { c.runs = runs; c.samples = []; });
  });
  for (let round = 0; round < runs; ++round) {
    for (let j = 0; j < specs.length; ++j) {
      const spec = specs[(j + round) % specs.length];
      for (const i of round % 2 === 0 ? [0, 1] : [1, 0]) {
        const result = spawnSync(process.execPath, [path.join(dirs[i], spec.file), ...spec.argv],
          {encoding: 'utf8', timeout: 60000, maxBuffer: 16 * 1024 * 1024});
        let samples;
        try {
          assert.equal(result.status, 0, `${spec.name}: ${result.error || result.stderr || result.signal}`);
          if (fixed) samples = parseHostOutput(result.stdout);
          else {
            assert(!/FAIL|WRONG/.test(result.stdout), 'guest correctness failure');
            assert(result.stdout.split('\n').includes('PASS cache population'), 'missing PASS');
            samples = result.stdout.split('\n').filter(line => line.startsWith('CACHE_POPULATION '))
              .map(line => JSON.parse(line.slice('CACHE_POPULATION '.length)));
            assert.deepEqual(samples.map(s => s.phase), ['cold', 'warm'], 'missing/reordered population samples');
            samples.forEach(s => validatePopulationSample(s, spec.population, reports[i].build.profile));
          }
        } catch (error) {
          fs.mkdirSync(outputDir, {recursive: true});
          fs.writeFileSync(failureFile, JSON.stringify({suite, round, case: spec.name,
            build: i === 0 ? 'before' : 'after', argv: [path.join(dirs[i], spec.file), ...spec.argv],
            artifact: reports[i].cases.find(c => c.name === spec.name).artifact,
            status: result.status, signal: result.signal, error: error.message,
            completedSamples: reports.map(r => r.cases.map(c => ({name: c.name, samples: c.samples}))),
            stdout: result.stdout, stderr: result.stderr}, null, 2) + '\n', {flag: 'wx'});
          throw error;
        }
        reports[i].cases.find(c => c.name === spec.name).samples.push(...samples.map(s => ({...s, process: round})));
      }
    }
  }
  verify();
  reports.forEach(r => {
    r.cases.forEach(c => { c.summary = fixed ? summarize(c.samples) : summarizePopulation(c.samples, r.build.profile); });
    validate(r);
  });
  fs.mkdirSync(outputDir, {recursive: true});
  reports.forEach((r, i) => fs.writeFileSync(outputFiles[i], JSON.stringify(r, null, 2) + '\n', {flag: 'wx'}));
  const comparison = compare(...reports);
  fs.writeFileSync(outputFiles[2], JSON.stringify(comparison, null, 2) + '\n', {flag: 'wx'});
  console.log(`${suite} paired collection complete; comparison ${comparison.pass ? 'PASS' : 'FAIL'}`);
  process.exitCode = comparison.pass ? 0 : 1;
}

try {
  collect();
} catch (error) {
  console.error(`Cannot collect paired reports: ${error.message}`);
  process.exitCode = 2;
}
