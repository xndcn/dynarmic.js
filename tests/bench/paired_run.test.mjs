// Synthetic child processes only; none of these samples are performance data.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {report, samples} from './fixtures.mjs';
import {cases, validateReport} from './protocol.mjs';
import {populationCases, populationMethodology, summarizePopulation, validatePopulationReport} from './cache_population_protocol.mjs';

const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
function populationSamples(population) {
  return ['cold', 'warm'].map(phase => ({phase, population, ms: 1, profile: null,
    compile: {blocks: phase === 'cold' ? population : 0, bytes: phase === 'cold' ? population * 100 : 0,
      translateMs: 0, optMs: 0, emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0}}));
}
function setup(t, suite, exitCode = 0) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-paired-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const specs = suite === 'fixed' ? cases : populationCases.map(s => ({...s, file: 'jit_cachepopbench.js'}));
  const orderFile = path.join(root, 'order.jsonl');
  const dirs = [], files = [];
  for (const label of ['before', 'after']) {
    const build = path.join(root, label), hosts = path.join(build, 'tests'), source = path.join(build, 'source');
    fs.mkdirSync(hosts, {recursive: true}); fs.mkdirSync(path.join(source, 'tests/hosts'), {recursive: true});
    fs.writeFileSync(path.join(build, 'CMakeCache.txt'), `CMAKE_HOME_DIRECTORY:INTERNAL=${source}\n`);
    fs.writeFileSync(path.join(source, 'tests/hosts/benchmark.h'), 'synthetic measurement helper');
    for (const spec of specs) {
      const js = `const fs = require('node:fs');\n` +
        `fs.appendFileSync(${JSON.stringify(orderFile)}, JSON.stringify([${JSON.stringify(label)}, ${JSON.stringify(spec.file)}, process.argv.slice(2)])+'\\n');\n` +
        (suite === 'fixed'
          ? `for (const s of ${JSON.stringify(samples())}) console.log('BENCHMARK '+JSON.stringify(s)); console.log('PASS');\n`
          : `const make = ${populationSamples.toString()}; for (const s of make(Number(process.argv[3]))) { s.ms *= ${label === 'after' ? 2 : 1}; console.log('CACHE_POPULATION '+JSON.stringify(s)); } console.log('PASS cache population');\n`) +
        `process.exit(${label === 'after' ? exitCode : 0});\n`;
      fs.writeFileSync(path.join(hosts, spec.file), js);
      fs.writeFileSync(path.join(hosts, spec.file.replace(/\.js$/, '.wasm')), 'synthetic artifact');
      fs.writeFileSync(path.join(source, 'tests/hosts', spec.file.replace(/\.js$/, '.cpp')), 'synthetic workload');
    }
    const r = report();
    r.node = process.version; r.v8 = process.versions.v8;
    r.host = {platform: process.platform, release: os.release(), arch: process.arch, cpu: os.cpus()[0]?.model, cpus: os.cpus().length};
    r.runtime = {nodeOptions: process.env.NODE_OPTIONS || '', cpuAffinity: process.platform === 'linux'
      ? /^Cpus_allowed_list:\s*(.*)$/m.exec(fs.readFileSync('/proc/self/status', 'utf8'))?.[1] || 'unknown' : 'unavailable'};
    if (suite === 'population') {
      r.schema = 'dynarmic.js/cache-population-v1'; r.methodology = populationMethodology;
      r.cases = specs.map(spec => {
        const observations = Array.from({length: 5}, (_, process) => populationSamples(spec.population).map(s => ({...s, process}))).flat();
        return {...spec, runs: 5, samples: observations, summary: summarizePopulation(observations, false)};
      });
    }
    for (const item of r.cases) {
      const spec = specs.find(s => s.name === item.name);
      item.artifact = {jsSha256: hash(path.join(hosts, spec.file)), wasmSha256: hash(path.join(hosts, spec.file.replace(/\.js$/, '.wasm')))};
      const workload = createHash('sha256').update(fs.readFileSync(path.join(source, 'tests/hosts', spec.file.replace(/\.js$/, '.cpp'))));
      if (suite === 'fixed') workload.update(fs.readFileSync(path.join(source, 'tests/hosts/benchmark.h')));
      item.workloadSha256 = workload.digest('hex');
    }
    const file = path.join(root, label + '.json'); fs.writeFileSync(file, JSON.stringify(r));
    dirs.push(hosts); files.push(file);
  }
  const output = path.join(root, 'output');
  const run = () => spawnSync(process.execPath, [new URL('./paired_run.mjs', import.meta.url).pathname,
    suite, ...dirs, ...files, output, '--runs', '5'], {encoding: 'utf8', timeout: 30000});
  return {root, specs, dirs, files, output, orderFile, run};
}

for (const suite of ['fixed', 'population']) test(`paired ${suite} CLI retains samples and alternates order`, t => {
  const f = setup(t, suite), result = f.run();
  assert.equal(result.status, suite === 'fixed' ? 0 : 1, result.stderr);
  assert.equal(JSON.parse(fs.readFileSync(path.join(f.output, `${suite}-comparison.json`))).pass, suite === 'fixed');
  for (const side of ['before', 'after']) {
    const r = JSON.parse(fs.readFileSync(path.join(f.output, `${suite}-${side}.json`)));
    (suite === 'fixed' ? validateReport : validatePopulationReport)(r);
    assert.equal(r.cases.length, f.specs.length);
  }
  const expected = [];
  for (let round = 0; round < 5; ++round) for (let j = 0; j < f.specs.length; ++j) {
    const spec = f.specs[(j + round) % f.specs.length];
    for (const side of round % 2 === 0 ? ['before', 'after'] : ['after', 'before']) expected.push([side, spec.file, spec.argv]);
  }
  assert.deepEqual(fs.readFileSync(f.orderFile, 'utf8').trim().split('\n').map(JSON.parse), expected);
  const retained = fs.readFileSync(path.join(f.output, `${suite}-before.json`));
  assert.equal(f.run().status, 2, 'must not overwrite previous cohort');
  assert.deepEqual(fs.readFileSync(path.join(f.output, `${suite}-before.json`)), retained);
});
test('paired collector rejects changed executable before launching', t => {
  const f = setup(t, 'fixed'); fs.appendFileSync(path.join(f.dirs[0], 'jit_bench.js'), '\n');
  assert.equal(f.run().status, 2); assert(!fs.existsSync(f.orderFile));
});
test('paired collector rejects nonzero exit even after PASS', t => {
  const f = setup(t, 'population', 1);
  assert.equal(f.run().status, 2);
  assert(!fs.existsSync(path.join(f.output, 'population-before.json')));
  const file = path.join(f.output, 'population-failure.json');
  const retained = fs.readFileSync(file), failure = JSON.parse(retained);
  assert.equal(failure.status, 1);
  assert.equal(failure.build, 'after');
  assert.equal(failure.completedSamples[0][0].samples.length, 2);
  assert.equal(failure.completedSamples[1][0].samples.length, 0);
  assert(failure.stdout.includes('PASS cache population'));
  assert.equal(f.run().status, 2);
  assert.deepEqual(fs.readFileSync(file), retained);
});
test('paired collector rejects upstream mismatch before launching', t => {
  const f = setup(t, 'fixed');
  const after = JSON.parse(fs.readFileSync(f.files[1])); after.git.upstream = 'f'.repeat(40);
  fs.writeFileSync(f.files[1], JSON.stringify(after));
  assert.equal(f.run().status, 2); assert(!fs.existsSync(f.orderFile));
});
