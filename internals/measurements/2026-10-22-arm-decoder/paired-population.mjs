// Reproduce the follow-up experiment with unchanged benchmark executables.
// Templates must be valid reports collected from these exact builds by
// tests/bench/cache_population_run.mjs. They supply build/host provenance;
// artifact and workload hashes are verified again here, before and after.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {populationCases, summarizePopulation, validatePopulationReport} from '../../../tests/bench/cache_population_protocol.mjs';

const [beforeDir, afterDir, beforeFile, afterFile, outputDir, count = '25'] = process.argv.slice(2);
assert(beforeDir && afterDir && beforeFile && afterFile && outputDir,
  'usage: paired-population.mjs before-host-dir after-host-dir before.json after.json output-dir [runs]');
assert(!process.env.DYN_DUMP_DIR, 'disable artifact dumping');
const runs = Number(count);
assert(Number.isInteger(runs) && runs >= 5 && runs <= 100);
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const reports = [beforeFile, afterFile].map(file => JSON.parse(fs.readFileSync(file)));
const dirs = [beforeDir, afterDir].map(dir => path.resolve(dir));
function verify() {
  reports.forEach((report, i) => {
    assert.equal(report.node, process.version);
    assert.equal(report.v8, process.versions.v8);
    assert.deepEqual(report.host, {platform: process.platform, release: os.release(), arch: process.arch,
      cpu: os.cpus()[0]?.model, cpus: os.cpus().length});
    assert.equal(report.runtime.nodeOptions, process.env.NODE_OPTIONS || '');
    assert.equal(report.runtime.cpuAffinity,
      /^Cpus_allowed_list:\s*(.*)$/m.exec(fs.readFileSync('/proc/self/status', 'utf8'))?.[1]);
    for (const item of report.cases) {
      assert.equal(item.artifact.jsSha256, hash(path.join(dirs[i], 'jit_cachepopbench.js')));
      assert.equal(item.artifact.wasmSha256, hash(path.join(dirs[i], 'jit_cachepopbench.wasm')));
      assert.equal(item.workloadSha256, hash(new URL('../../../tests/hosts/jit_cachepopbench.cpp', import.meta.url)));
    }
  });
}
reports.forEach(r => validatePopulationReport(r));
verify();
reports.forEach(r => {
  r.timestamp = new Date().toISOString();
  r.collection = 'paired builds per case, alternating build order each round; all samples retained';
  r.cases.forEach(c => { c.runs = runs; c.samples = []; });
});
for (let round = 0; round < runs; ++round) {
  for (let j = 0; j < populationCases.length; ++j) {
    const spec = populationCases[(j + round) % populationCases.length];
    for (const i of round % 2 === 0 ? [0, 1] : [1, 0]) {
      const result = spawnSync(process.execPath, [path.join(dirs[i], 'jit_cachepopbench.js'), ...spec.argv],
        {encoding: 'utf8', timeout: 60000, maxBuffer: 8 * 1024 * 1024});
      assert.equal(result.status, 0, String(result.error || result.stderr));
      assert(!/FAIL|WRONG/.test(result.stdout));
      assert(result.stdout.split('\n').includes('PASS cache population'));
      const samples = result.stdout.split('\n').filter(line => line.startsWith('CACHE_POPULATION '))
        .map(line => ({...JSON.parse(line.slice('CACHE_POPULATION '.length)), process: round}));
      assert.equal(samples.length, 2);
      reports[i].cases.find(c => c.name === spec.name).samples.push(...samples);
    }
  }
}
verify();
fs.mkdirSync(outputDir, {recursive: true});
reports.forEach((r, i) => {
  r.cases.forEach(c => { c.summary = summarizePopulation(c.samples, r.build.profile); });
  validatePopulationReport(r);
  fs.writeFileSync(path.join(outputDir, `population-${i === 0 ? 'before' : 'after'}-paired.json`), JSON.stringify(r, null, 2) + '\n');
});
