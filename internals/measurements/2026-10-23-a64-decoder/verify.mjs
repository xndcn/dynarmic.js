// Audit the retained experiment, including intentionally failing cohorts.
// This validates evidence consistency; it does not rerun the JIT or attest a build.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {hashFile} from '../../../tests/bench/environment.mjs';
import {compareReports} from '../../../tests/bench/compare.mjs';
import {comparePopulationReports} from '../../../tests/bench/cache_population_compare.mjs';
import {compareCoremark} from '../../../tests/bench/coremark/compare.mjs';
import {coremarkCases, parseCoremark} from '../../../tests/bench/coremark_protocol.mjs';
import {parseHostOutput} from '../../../tests/bench/protocol.mjs';
import {summarizeCpuProfile} from '../../../tests/bench/cpu_profile.mjs';

const [directory, ...extra] = process.argv.slice(2);
assert(directory && !extra.length, 'usage: verify.mjs <extracted-evidence-directory>');
const read = file => fs.readFileSync(path.join(directory, file), 'utf8');
const json = file => JSON.parse(read(file));
assert.equal(fs.readFileSync(new URL('./duration-paired.mjs', import.meta.url), 'utf8'), read('duration-paired.mjs'));
for (const [file, hash] of Object.entries(json('hashes.json'))) {
  assert.equal(hashFile(path.join(directory, file)), hash, file);
}
function comparison(folder, prefix, compare, duration = false) {
  const before = json(`${folder}/${prefix}before.json`), after = json(`${folder}/${prefix}after.json`);
  const result = compare(before, after, duration);
  assert.deepEqual(json(`${folder}/${prefix}comparison.json`), result, `comparison differs: ${folder}`);
  console.log(`${folder}: ${result.pass ? 'PASS' : 'FAIL (retained)'}`);
  return [before, after];
}
comparison('paired-fixed', 'fixed-', compareReports);
for (const dir of ['paired-population', 'paired-population-profile', 'population-aa', 'population-matched']) {
  const [before, after] = comparison(dir, 'population-', comparePopulationReports);
  // These executable identities are an important negative control: the A64
  // adapter changes no code in the A32-only population host.
  for (const c of before.cases) assert.deepEqual(c.artifact, after.cases.find(a => a.name === c.name).artifact);
}
const first = comparison('paired-coremark', '', compareCoremark);
const repeat = comparison('paired-coremark-repeat', '', compareCoremark);
const combined = comparison('coremark-combined', '', compareCoremark);
for (let side = 0; side < 2; ++side) for (const c of combined[side].cases) {
  const a = first[side].cases.find(v => v.name === c.name), b = repeat[side].cases.find(v => v.name === c.name);
  assert.equal(c.runs, 35);
  assert.equal(c.runs, a.runs + b.runs);
  assert.deepEqual(c.samples, [...a.samples, ...b.samples.map(s => ({...s, process: s.process + a.runs}))],
    'combined cohort discarded or changed samples');
}

const duration = comparison('duration-paired', '', compareCoremark, true);
const commands = json('duration-paired/commands.json');
assert.equal(commands.length, 60);
const keys = new Set();
for (const command of commands) {
  assert.equal(command.status, 0);
  assert.equal(command.signal, null);
  const key = `${command.side}/${command.case}/${command.round}`;
  assert(!keys.has(key), 'duplicate child observation'); keys.add(key);
  const side = ['before', 'after'].indexOf(command.side); assert(side >= 0);
  const report = duration[side], c = report.cases.find(v => v.name === command.case);
  assert(c && c.runs === 5);
  const spec = coremarkCases.find(v => v.name === c.name);
  assert.deepEqual(command.argv.slice(-2), [spec.architecture, String(spec.mode)]);
  const lines = command.stdout.split('\n').filter(l => l.startsWith('COREMARK_SAMPLE '));
  assert.equal(lines.length, 1);
  const sample = JSON.parse(lines[0].slice(16)); sample.process = command.round;
  sample.metrics = parseCoremark(sample.stdout, report.workload.iterations);
  assert.deepEqual(sample, c.samples.find(s => s.process === command.round));
}

const observations = json('cpu-after/observations.json').observations;
assert.equal(observations.length, 10);
for (const o of observations) {
  const stem = `cpu-after/${o.caseName}-${o.round}`;
  assert.equal(hashFile(path.join(directory, stem + '.cpuprofile')), o.profileSha256);
  const raw = json(stem + '.cpuprofile'), summary = summarizeCpuProfile(raw);
  assert.deepEqual(summary, json(stem + '.summary.json'));
  const compiler = summarizeCpuProfile(raw, 'AddressSpace::GetOrEmit');
  assert.deepEqual(compiler, json(stem + '.compiler.json'));
  assert.equal(summary.totalSamples, o.totalSamples);
  assert.equal(compiler.selectedSamples, o.compilerSamples);
  assert(!raw.nodes.some(n => n.callFrame.functionName.includes('GetDecodeTable')));
  if (o.caseName === 'fixed') assert.deepEqual(parseHostOutput(read(stem + '.stdout')), o.samples);
  else {
    const lines = read(stem + '.stdout').split('\n').filter(l => l.startsWith('COREMARK_SAMPLE '));
    assert.equal(lines.length, 1);
    const sample = JSON.parse(lines[0].slice(16));
    sample.metrics = parseCoremark(sample.stdout, 4000);
    assert.deepEqual([sample], o.samples);
  }
}
console.log('PASS archive audit: hashes, recomputed comparisons, all combined samples, 60 duration children and 10 CPU profiles');
