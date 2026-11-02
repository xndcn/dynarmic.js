#!/usr/bin/env node
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {hashFile, readBuildInfo, sourceInfo, systemInfo} from '../environment.mjs';
import {coremarkCases, coremarkMethodology, coremarkIdentity, parseCoremark, summarizeCoremark, validateCoremarkBuild, validateCoremarkReport} from '../coremark_protocol.mjs';

function collect() {
  const args = process.argv.slice(2);
  const browserDir = path.resolve(args.shift() || 'build-em/examples/browser');
  const workloadDir = path.resolve(args.shift() || '');
  let runs = 5, output, requireDuration = false, timeout = 300000;
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--runs') runs = Number(args[++i]);
    else if (args[i] === '--output') output = args[++i];
    else if (args[i] === '--require-duration') requireDuration = true;
    else if (args[i] === '--timeout-ms') timeout = Number(args[++i]);
    else throw new Error(`unknown argument ${args[i]}`);
  }
  assert(output && Number.isInteger(runs) && runs >= 1 && runs <= 100,
    'usage: run.mjs <browser-build-dir> <workload-dir> --output report.json [--runs 5] [--require-duration] [--timeout-ms 300000]');
  assert(Number.isInteger(timeout) && timeout > 0 && timeout <= 3600000, 'timeout must be 1..3600000 ms');
  assert(!process.env.DYN_DUMP_DIR, 'disable DYN_DUMP_DIR');
  const failureFile = output + '.failure.json';
  for (const file of [output, failureFile]) assert(!fs.existsSync(file), `refusing to overwrite ${file}`);
  const workload = JSON.parse(fs.readFileSync(path.join(workloadDir, 'manifest.json'), 'utf8'));
  validateCoremarkBuild(workload);
  const {sourceRoot, build} = readBuildInfo(path.resolve(browserDir, '../..'));
  const system = systemInfo();
  const git = sourceInfo(sourceRoot);
  const jit = path.join(browserDir, 'dist/arm_jit.js'), wasm = path.join(browserDir, 'dist/arm_jit.wasm');
  const artifact = {jsSha256: hashFile(jit), wasmSha256: hashFile(wasm)};
  const verify = () => {
    assert.equal(hashFile(jit), artifact.jsSha256, 'JIT JS changed during measurement');
    assert.equal(hashFile(wasm), artifact.wasmSha256, 'JIT Wasm changed during measurement');
    for (const item of Object.values(workload.artifacts)) {
      assert.equal(hashFile(path.join(workloadDir, item.file)), item.sha256, 'workload executable changed');
    }
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(workloadDir, 'manifest.json'), 'utf8')), workload, 'manifest changed');
    assert.deepEqual(systemInfo(), system, 'runtime/host changed');
    assert.deepEqual(sourceInfo(sourceRoot), git, 'source identity changed');
    assert.deepEqual(readBuildInfo(path.resolve(browserDir, '../..')).build, build, 'build configuration changed');
  };
  verify();
  const cases = coremarkCases.map(({name, architecture, memoryMode}) => ({name, architecture, memoryMode, runs, samples: []}));
  let last = null;
  try {
    const nativeResult = spawnSync(path.join(workloadDir, workload.artifacts.native.file), [], {encoding: 'utf8', timeout, maxBuffer: 1024 * 1024});
    last = {case: 'native', status: nativeResult.status, signal: nativeResult.signal, stdout: nativeResult.stdout, stderr: nativeResult.stderr};
    assert.equal(nativeResult.status, 0, String(nativeResult.error || nativeResult.stderr || nativeResult.signal));
    const native = {stdout: nativeResult.stdout, metrics: parseCoremark(nativeResult.stdout, workload.iterations)};
    for (let round = 0; round < runs; ++round) for (let j = 0; j < coremarkCases.length; ++j) {
      const index = (round + j) % coremarkCases.length, spec = coremarkCases[index];
      const child = spawnSync(process.execPath, [fileURLToPath(new URL('./sample.mjs', import.meta.url)), jit,
        path.join(workloadDir, workload.artifacts[spec.architecture].file), spec.architecture, String(spec.mode)],
        {encoding: 'utf8', timeout, maxBuffer: 4 * 1024 * 1024});
      last = {case: spec.name, process: round, status: child.status, signal: child.signal, stdout: child.stdout, stderr: child.stderr};
      assert.equal(child.status, 0, String(child.error || child.stderr || child.signal));
      const rows = child.stdout.split('\n').filter(line => line.startsWith('COREMARK_SAMPLE '));
      assert.equal(rows.length, 1, 'missing/duplicate guest sample');
      const sample = JSON.parse(rows[0].slice('COREMARK_SAMPLE '.length));
      sample.process = round;
      sample.metrics = parseCoremark(sample.stdout, workload.iterations);
      assert.deepEqual(sample.metrics.crc, native.metrics.crc, 'native/guest CRC mismatch');
      cases[index].samples.push(sample);
    }
    verify();
    cases.forEach(c => { c.summary = summarizeCoremark(c.samples, build.profile); });
    const report = {schema: 'dynarmic.js/coremark-workload-v1', timestamp: new Date().toISOString(),
      ...system, git, build, artifact, workload, workloadSha256: coremarkIdentity(workload),
      methodology: coremarkMethodology, native, cases, durationQualified: cases.every(c => c.summary.durationQualified)};
    // Keep a valid short-run report even when the caller requires longer runs.
    validateCoremarkReport(report);
    fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
    validateCoremarkReport(report, requireDuration);
    console.log(`CoreMark workload PASS cases=${cases.length} durationQualified=${report.durationQualified}`);
  } catch (error) {
    fs.writeFileSync(failureFile, JSON.stringify({error: error.message, last, completedSamples: cases}, null, 2) + '\n', {flag: 'wx'});
    throw error;
  }
}
try { collect(); } catch (error) { console.error(`Cannot collect CoreMark workload: ${error.message}`); process.exitCode = 2; }
