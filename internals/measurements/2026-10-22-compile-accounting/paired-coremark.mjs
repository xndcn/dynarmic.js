#!/usr/bin/env node
// Experiment driver: alternate complete six-case collections between builds.
// The ordinary collector validates every child and retains failed observations.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {validateCoremarkReport, summarizeCoremark} from '../../../tests/bench/coremark_protocol.mjs';
import {compareCoremark} from '../../../tests/bench/coremark/compare.mjs';

function collect() {
const [before, after, workload, output, count = '5', option, ...extra] = process.argv.slice(2);
assert(before && after && workload && output && !extra.length && (!option || option === '--require-duration'),
  'usage: paired-coremark.mjs <before-browser-dir> <after-browser-dir> <workload-dir> <fresh-output-dir> [rounds=5] [--require-duration]');
const runs = Number(count), requireDuration = option === '--require-duration';
assert(Number.isInteger(runs) && runs >= 5 && runs <= 100);
fs.mkdirSync(output); // Refuse any existing cohort, including incomplete attempts.
const collected = [[], []];
const names = ['before', 'after'];
for (let round = 0; round < runs; ++round) {
  for (const side of round % 2 ? [1, 0] : [0, 1]) {
    const file = path.join(output, `${names[side]}-${round}.json`);
    const result = spawnSync(process.execPath, [fileURLToPath(new URL('../../../tests/bench/coremark/run.mjs', import.meta.url)),
      [before, after][side], workload, '--runs', '1', '--output', file, ...(requireDuration ? ['--require-duration'] : [])],
      {encoding: 'utf8', timeout: 3600000, maxBuffer: 4 * 1024 * 1024});
    fs.writeFileSync(file + '.process.json', JSON.stringify({round, side: names[side], status: result.status,
      signal: result.signal, error: result.error?.message, stdout: result.stdout, stderr: result.stderr}, null, 2) + '\n', {flag: 'wx'});
    assert.equal(result.status, 0, result.stderr || result.error?.message);
    const report = JSON.parse(fs.readFileSync(file, 'utf8'));
    validateCoremarkReport(report, requireDuration);
    if (collected[side].length) {
      const first = collected[side][0];
      for (const key of ['node', 'v8', 'runtime', 'host', 'build', 'git', 'artifact', 'methodology', 'workloadSha256']) {
        assert.deepEqual(report[key], first[key], `${names[side]} ${key} changed between rounds`);
      }
      assert.deepEqual(report.native.metrics.crc, first.native.metrics.crc);
    }
    collected[side].push(report);
    console.log(`round ${round + 1}/${runs} ${names[side]} complete`);
  }
}
const reports = collected.map(rounds => {
  const report = structuredClone(rounds[0]);
  report.collection = 'alternating complete six-case build cohorts; raw per-round reports retained';
  for (const c of report.cases) {
    c.runs = runs;
    c.samples = rounds.map((r, process) => {
      const item = r.cases.find(item => item.name === c.name);
      assert.equal(item.samples.length, 1);
      return {...item.samples[0], process};
    });
    c.summary = summarizeCoremark(c.samples, report.build.profile);
  }
  report.durationQualified = report.cases.every(c => c.summary.durationQualified);
  validateCoremarkReport(report, requireDuration);
  return report;
});
reports.forEach((r, i) => fs.writeFileSync(path.join(output, `${names[i]}.json`), JSON.stringify(r, null, 2) + '\n', {flag: 'wx'}));
const comparison = compareCoremark(...reports, requireDuration);
fs.writeFileSync(path.join(output, 'comparison.json'), JSON.stringify(comparison, null, 2) + '\n', {flag: 'wx'});
console.log(`CoreMark paired comparison ${comparison.pass ? 'PASS' : 'FAIL'}`);
process.exitCode = comparison.pass ? 0 : 1;

}
try { collect(); } catch (error) { console.error(error.message); process.exitCode = 2; }
