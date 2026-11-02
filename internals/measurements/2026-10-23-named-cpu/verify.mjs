// Run from any directory after extracting evidence.tar.gz into a new directory.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {hashFile} from '../../../tests/bench/environment.mjs';
import {summarizeCpuProfile} from '../../../tests/bench/cpu_profile.mjs';
import {parseHostOutput} from '../../../tests/bench/protocol.mjs';
import {validatePopulationSample} from '../../../tests/bench/cache_population_protocol.mjs';
import {parseCoremark, validateCoremarkBuild} from '../../../tests/bench/coremark_protocol.mjs';

const [directory, ...extra] = process.argv.slice(2);
assert(directory && !extra.length, 'usage: verify.mjs <extracted-evidence-directory>');
const read = file => fs.readFileSync(path.join(directory, file), 'utf8');
const json = file => JSON.parse(read(file));
const hashes = json('hashes.json');
for (const [file, hash] of Object.entries(hashes)) assert.equal(hashFile(path.join(directory, file)), hash, file);
const commands = json('cohort/commands.json');
assert.equal(commands.commands.length, 30);
assert(commands.commands.every(c => c.status === 0));
const observations = json('cohort/observations.json');
validateCoremarkBuild(observations.workload);
const native = parseCoremark(read('cohort/coremark-native.stdout'), observations.workload.iterations);
assert.equal(observations.cases.length, 30);
const expected = new Set(['a32', 'thumb', 'a64', 'population', 'coremark-a32', 'coremark-a64']
  .flatMap(name => Array.from({length: 5}, (_, i) => `${name}-${i}`)));
for (const c of observations.cases) {
  const stem = `${c.name}-${c.process}`;
  assert(expected.delete(stem), 'unexpected or duplicate case');
  const raw = json(`cohort/${stem}.cpuprofile`), summary = summarizeCpuProfile(raw);
  const saved = json(`cohort/${stem}.summary.json`);
  assert.equal(saved.input.sha256, hashFile(path.join(directory, `cohort/${stem}.cpuprofile`)));
  delete saved.input;
  assert.deepEqual(saved, summary);
  assert.equal(c.totalSamples, summary.totalSamples);
  const compiler = summarizeCpuProfile(raw, 'AddressSpace::GetOrEmit');
  assert.deepEqual(json(`cohort/${stem}.compiler.json`), compiler);
  assert.equal(c.compilerSamples, compiler.selectedSamples);
  const decode = summary.frames.find(f => f.functionName.startsWith('std::__2::array<')
    && f.functionName.endsWith('Dynarmic::A64::GetDecodeTable<Dynarmic::A64::TranslatorVisitor>()'));
  assert.equal(c.decoderSamples, decode?.inclusiveSamples ?? 0);
  assert.deepEqual(c.sampling, summary.sampling);
  const stdout = read(`cohort/${stem}.stdout`);
  let samples;
  if (c.name.startsWith('coremark-')) {
    const lines = stdout.split('\n').filter(l => l.startsWith('COREMARK_SAMPLE '));
    assert.equal(lines.length, 1);
    const s = JSON.parse(lines[0].slice(16));
    assert.equal(s.haltReason, 0x01000000);
    s.metrics = parseCoremark(s.stdout, observations.workload.iterations);
    assert.deepEqual(s.metrics.crc, native.crc);
    samples = [s];
  } else if (c.name === 'population') {
    assert(stdout.split('\n').includes('PASS cache population'));
    samples = stdout.split('\n').filter(l => l.startsWith('CACHE_POPULATION ')).map(l => JSON.parse(l.slice(17)));
    assert.equal(samples.length, 2);
    samples.forEach(s => validatePopulationSample(s, 4096, false));
  } else samples = parseHostOutput(stdout);
  assert.deepEqual(samples, c.samples);
}
assert.equal(expected.size, 0);
console.log('PASS named CPU archive: hashes, 30 profiles, subtrees and workload outputs');
