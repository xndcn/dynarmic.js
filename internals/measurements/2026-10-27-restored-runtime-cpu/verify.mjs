#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {summarizeCpuProfile} from '../../../tests/bench/cpu_profile.mjs';
import {coremarkCases, coremarkIdentity, parseCoremark, validateCoremarkBuild} from '../../../tests/bench/coremark_protocol.mjs';

const root = path.resolve(process.argv[2] || '');
if (process.argv.length !== 3 || !fs.statSync(root, {throwIfNoEntry: false})?.isDirectory()) {
  throw new Error('usage: verify.mjs <extracted-evidence-dir>');
}
const readJson = name => JSON.parse(fs.readFileSync(path.join(root, name), 'utf8'));
const sha256 = data => createHash('sha256').update(data).digest('hex');
const median = values => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
const workload = readJson('manifest.json');
validateCoremarkBuild(workload);
const workloadSha256 = 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa';
assert.equal(coremarkIdentity(workload), workloadSha256, 'workload identity mismatch');
const nativeStdout = fs.readFileSync(path.join(root, 'native.txt'), 'utf8');
const native = parseCoremark(nativeStdout, workload.iterations);
assert.equal(native.compilerFlags, workload.flags.native, 'native compiler flags mismatch');
const validation = {schema: 'dynarmic.js/run-profile-validation-v1',
  source: '339939c59a3cbb84a2c3af4a7a415ed14625ef9d',
  workloadSha256,
  native: {stdout: nativeStdout, metrics: native}, cases: []};
const hotspots = {schema: 'dynarmic.js/run-profile-hotspots-v1',
  methodology: 'Per-process unweighted self-sample shares; missing names contribute zero. Medians across three diagnostic processes are not timings or speedups.',
  cases: []};

for (const spec of coremarkCases) {
  const processes = [], reports = [];
  for (let process = 0; process < 3; ++process) {
    const base = `${spec.name}-p${process}`;
    const rows = fs.readFileSync(path.join(root, `${base}-sample.txt`), 'utf8')
      .split('\n').filter(line => line.startsWith('COREMARK_SAMPLE '));
    assert.equal(rows.length, 1, `${base}: missing or duplicate sample`);
    assert.equal(fs.statSync(path.join(root, `${base}-stderr.txt`)).size, 0, `${base}: nonempty stderr`);
    const sample = JSON.parse(rows[0].slice('COREMARK_SAMPLE '.length));
    assert.equal(sample.haltReason, 0x01000000);
    assert(sample.runMs >= 10000, `${base}: short Run`);
    const metrics = parseCoremark(sample.stdout, workload.iterations);
    assert.deepEqual(metrics.crc, native.crc, `${base}: native/guest CRC mismatch`);
    assert.equal(metrics.compilerFlags, workload.flags[spec.architecture], `${base}: compiler flags mismatch`);
    assert(sample.runMs + 0.01 >= metrics.ticks / 1000, `${base}: kernel exceeds Run`);
    const raw = fs.readFileSync(path.join(root, `${base}.cpuprofile`));
    const profile = JSON.parse(raw);
    const profileSha256 = sha256(raw);
    const all = summarizeCpuProfile(profile);
    const guest = summarizeCpuProfile(profile, `^${spec.architecture}_`);
    for (const [scope, recomputed] of [['all', all], ['guest', guest]]) {
      const retained = readJson(`${base}-${scope}.json`);
      assert.equal(retained.input.sha256, profileSha256, `${base}/${scope}: raw hash`);
      const {input, ...summary} = retained;
      assert.equal(path.basename(input.path), `${base}.cpuprofile`);
      assert.deepEqual(summary, recomputed, `${base}/${scope}: summary mismatch`);
    }
    assert(guest.selectedSamples > 0 && guest.selectedSamples <= all.totalSamples);
    processes.push({process, runMs: sample.runMs, compile: sample.compile, profileSha256,
      totalSamples: all.totalSamples, guestSamples: guest.selectedSamples,
      guestShare: guest.selectedSamples / all.totalSamples, sampling: all.sampling});
    reports.push(guest);
  }
  validation.cases.push({name: spec.name, processes});
  const names = new Set(reports.flatMap(report => report.frames.map(frame => frame.functionName)));
  const frames = [...names].map(functionName => {
    const observations = reports.map(report => report.frames.find(frame => frame.functionName === functionName));
    const identities = [...new Set(observations.filter(Boolean)
      .map(frame => `${frame.url}:${frame.lineNumber}:${frame.columnNumber}`))];
    const shares = observations.map(frame => frame?.selfShare || 0);
    return {functionName, medianSelfShare: median(shares), minSelfShare: Math.min(...shares),
      maxSelfShare: Math.max(...shares), selfSamples: observations.map(frame => frame?.selfSamples || 0), identities};
  }).sort((a, b) => b.medianSelfShare - a.medianSelfShare || a.functionName.localeCompare(b.functionName));
  hotspots.cases.push({name: spec.name, selectedSamples: reports.map(report => report.selectedSamples), frames});
}
assert.deepEqual(readJson('validation.json'), validation, 'validation report mismatch');
assert.deepEqual(readJson('hotspots.json'), hotspots, 'hotspot report mismatch');

function filesBelow(directory, prefix = '') {
  return fs.readdirSync(directory, {withFileTypes: true}).flatMap(entry => {
    const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
    return entry.isDirectory() ? filesBelow(path.join(directory, entry.name), relative) : [relative];
  });
}
const expectedFiles = filesBelow(root).filter(name => name !== 'SHA256SUMS').sort();
const inventoryRows = fs.readFileSync(path.join(root, 'SHA256SUMS'), 'utf8').split('\n')
  .filter(Boolean).map(line => {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    assert(match && !path.isAbsolute(match[2]) && !match[2].split('/').includes('..'), 'invalid inventory row');
    return [match[2], match[1]];
  });
const inventory = new Map(inventoryRows);
assert.equal(inventoryRows.length, inventory.size, 'duplicate inventory path');
assert.equal(inventory.size, expectedFiles.length, 'inventory size');
assert.deepEqual([...inventory.keys()].sort(), expectedFiles, 'inventory file set');
for (const [name, hash] of inventory) assert.equal(sha256(fs.readFileSync(path.join(root, name))), hash, `${name}: inventory hash`);
console.log(`restored runtime CPU evidence PASS profiles=${validation.cases.length * 3} files=${inventory.size}`);
