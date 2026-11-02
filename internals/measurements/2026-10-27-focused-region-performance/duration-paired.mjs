#!/usr/bin/env node
// Preregistered ten-pair A64 CoreMark performance experiment. No sample replacement.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {hashFile, readBuildInfo, sourceInfo, systemInfo} from '../../../tests/bench/environment.mjs';
import {coremarkIdentity, parseCoremark, validateCoremarkBuild} from '../../../tests/bench/coremark_protocol.mjs';
import {analyzeFocusedCoremark, BASELINE_FOCUS, CANDIDATE_FOCUS, FOCUSED_METHODOLOGY, PAIRS, RAW_SCHEMA} from './analyze.mjs';

const SIDES = ['baseline', 'candidate'];
const orderFor = pair => pair % 2 ? ['candidate', 'baseline'] : ['baseline', 'candidate'];
const sampleCollector = fileURLToPath(new URL('../../../tests/bench/coremark/sample.mjs', import.meta.url));

function readConfiguration(buildDir) {
  const cache = Object.fromEntries(fs.readFileSync(path.join(buildDir, 'CMakeCache.txt'), 'utf8')
    .split('\n').map(line => /^([^#/:][^:]*):[^=]+=(.*)$/.exec(line)).filter(Boolean)
    .map(([, key, value]) => [key, value]));
  assert.equal(cache.DYNARMIC_JS_REGION_DIAGNOSTICS, 'OFF', 'region diagnostics must be OFF');
  assert.equal(cache.DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS, 'OFF', 'focus test exceptions must be OFF');
  assert.equal(cache.DYNARMIC_JS_EXAMPLES, 'ON', 'browser examples must be ON');
  return {regionDiagnostics: false, focusTestExceptions: false, examples: true};
}

function validateBuilds(infos, configurations, sources) {
  for (let index = 0; index < 2; ++index) {
    assert.equal(infos[index].build.type, 'Release', `${SIDES[index]} must be Release`);
    assert.equal(infos[index].build.profile, false, `${SIDES[index]} profiling must be OFF`);
    assert.equal(sources[index].dirty, false, `${SIDES[index]} source must be clean`);
  }
  assert.equal(infos[0].build.a64RegionFocus, BASELINE_FOCUS, 'baseline focus must be empty');
  assert.equal(infos[1].build.a64RegionFocus, CANDIDATE_FOCUS, 'candidate focus must be exact');
  const normalized = infos.map(info => {
    const {a64RegionFocus, ...build} = info.build;
    return build;
  });
  assert.deepEqual(normalized[1], normalized[0], 'only a64RegionFocus may differ between builds');
  assert.deepEqual(sources[1], sources[0], 'source identity changed');
  assert.deepEqual(configurations[1], configurations[0], 'build feature configuration changed');
}

function collect() {
  const [baselineArg, candidateArg, workloadArg, outputArg, ...extra] = process.argv.slice(2);
  assert(baselineArg && candidateArg && workloadArg && outputArg && !extra.length,
    'usage: duration-paired.mjs <baseline-browser-dir> <candidate-browser-dir> <workload-dir> <new-output-dir>');
  assert(!process.env.DYN_DUMP_DIR, 'disable DYN_DUMP_DIR');
  assert.equal(process.env.NODE_OPTIONS || '', '', 'NODE_OPTIONS must be empty');
  const dirs = [baselineArg, candidateArg].map(value => path.resolve(value));
  const workloadDir = path.resolve(workloadArg), output = path.resolve(outputArg);
  fs.mkdirSync(output);
  const observationsFile = path.join(output, 'observations.jsonl');
  const schedule = Array.from({length: PAIRS}, (_, pair) => ({pair, order: orderFor(pair)}));
  fs.writeFileSync(path.join(output, 'schedule.json'), JSON.stringify({pairs: PAIRS, schedule}, null, 2) + '\n', {flag: 'wx'});
  let last = null;
  const pairs = [];
  try {
    const workload = JSON.parse(fs.readFileSync(path.join(workloadDir, 'manifest.json'), 'utf8'));
    validateCoremarkBuild(workload);
    const infos = dirs.map(dir => readBuildInfo(path.resolve(dir, '../..')));
    const configurations = dirs.map(dir => readConfiguration(path.resolve(dir, '../..')));
    const sources = infos.map(info => sourceInfo(info.sourceRoot));
    validateBuilds(infos, configurations, sources);
    const system = systemInfo();
    const artifacts = dirs.map(dir => ({
      jsSha256: hashFile(path.join(dir, 'dist/arm_jit.js')),
      wasmSha256: hashFile(path.join(dir, 'dist/arm_jit.wasm')),
    }));
    assert.equal(artifacts[0].jsSha256, artifacts[1].jsSha256, 'JS artifacts must match');
    assert.notEqual(artifacts[0].wasmSha256, artifacts[1].wasmSha256, 'focused Wasm artifacts must differ');
    const verify = () => {
      assert.deepEqual(systemInfo(), system, 'runtime/host changed');
      assert.deepEqual(JSON.parse(fs.readFileSync(path.join(workloadDir, 'manifest.json'), 'utf8')), workload,
        'workload manifest changed');
      for (const artifact of Object.values(workload.artifacts)) {
        assert.equal(hashFile(path.join(workloadDir, artifact.file)), artifact.sha256, 'workload executable changed');
      }
      dirs.forEach((dir, index) => {
        assert.equal(hashFile(path.join(dir, 'dist/arm_jit.js')), artifacts[index].jsSha256, 'JIT JS changed');
        assert.equal(hashFile(path.join(dir, 'dist/arm_jit.wasm')), artifacts[index].wasmSha256, 'JIT Wasm changed');
        assert.deepEqual(readBuildInfo(path.resolve(dir, '../..')), infos[index], 'build configuration changed');
        assert.deepEqual(readConfiguration(path.resolve(dir, '../..')), configurations[index], 'feature configuration changed');
        assert.deepEqual(sourceInfo(infos[index].sourceRoot), sources[index], 'source observation changed');
      });
    };
    verify();
    const nativeResult = spawnSync(path.join(workloadDir, workload.artifacts.native.file), [],
      {encoding: 'utf8', timeout: 300000, maxBuffer: 1024 * 1024});
    last = {case: 'native', status: nativeResult.status, signal: nativeResult.signal,
      error: nativeResult.error?.message, stdout: nativeResult.stdout, stderr: nativeResult.stderr};
    fs.writeFileSync(path.join(output, 'native.json'), JSON.stringify(last, null, 2) + '\n', {flag: 'wx'});
    assert.equal(nativeResult.status, 0, String(nativeResult.error || nativeResult.stderr || nativeResult.signal));
    const native = {stdout: nativeResult.stdout, metrics: parseCoremark(nativeResult.stdout, workload.iterations)};
    for (let pair = 0; pair < PAIRS; ++pair) {
      const current = {pair, order: orderFor(pair)};
      for (const side of current.order) {
        const index = side === 'baseline' ? 0 : 1;
        const argv = [sampleCollector, path.join(dirs[index], 'dist/arm_jit.js'),
          path.join(workloadDir, workload.artifacts.a64.file), 'a64', '0'];
        const started = new Date().toISOString();
        const child = spawnSync(process.execPath, argv,
          {encoding: 'utf8', timeout: 300000, maxBuffer: 4 * 1024 * 1024});
        const observation = {pair, side, argv, started, ended: new Date().toISOString(),
          status: child.status, signal: child.signal, error: child.error?.message,
          stdout: child.stdout, stderr: child.stderr};
        fs.appendFileSync(observationsFile, JSON.stringify(observation) + '\n');
        last = observation;
        assert.equal(child.status, 0, `${side} pair ${pair} child status ${child.status}: ${child.error || child.stderr || child.signal}`);
        const rows = child.stdout.split('\n').filter(line => line.startsWith('COREMARK_SAMPLE '));
        assert.equal(rows.length, 1, 'missing/duplicate guest sample');
        const decoded = JSON.parse(rows[0].slice('COREMARK_SAMPLE '.length));
        const metrics = parseCoremark(decoded.stdout, workload.iterations);
        const sample = {...decoded, process: pair, metrics};
        assert.deepEqual(sample.metrics.crc, native.metrics.crc, 'native/guest CRC mismatch');
        assert(sample.metrics.durationValid, 'guest run below 10 seconds');
        current[side] = sample;
        fs.writeFileSync(path.join(output, 'progress.json'), JSON.stringify({
          complete: pair * 2 + current.order.indexOf(side) + 1, total: PAIRS * 2,
          pair, side, kernelSeconds: sample.metrics.seconds,
        }, null, 2) + '\n');
        console.log(`${pair * 2 + current.order.indexOf(side) + 1}/${PAIRS * 2} pair=${pair} ${side} ` +
          `kernel=${sample.metrics.seconds.toFixed(6)}s run=${(sample.runMs / 1000).toFixed(6)}s`);
      }
      pairs.push(current);
    }
    verify();
    const raw = {schema: RAW_SCHEMA, timestamp: new Date().toISOString(), ...system,
      git: {baseline: sources[0], candidate: sources[1]},
      build: {baseline: infos[0].build, candidate: infos[1].build},
      configuration: {baseline: configurations[0], candidate: configurations[1]},
      artifact: {baseline: artifacts[0], candidate: artifacts[1]}, workload,
      workloadSha256: coremarkIdentity(workload), methodology: FOCUSED_METHODOLOGY,
      architecture: 'a64', memoryMode: '0', declaredPairs: PAIRS, native, pairs};
    const comparison = analyzeFocusedCoremark(raw);
    fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(raw, null, 2) + '\n', {flag: 'wx'});
    fs.writeFileSync(path.join(output, 'comparison.json'), JSON.stringify(comparison, null, 2) + '\n', {flag: 'wx'});
    console.log(`focused CoreMark ${comparison.pass ? 'PASS' : 'FAIL'} ` +
      `kernelSpeedup=${comparison.headline.kernel.speedup.toFixed(6)} ` +
      `runSpeedup=${comparison.headline.run.speedup.toFixed(6)}`);
    process.exitCode = comparison.pass ? 0 : 1;
  } catch (error) {
    fs.writeFileSync(path.join(output, 'failure.json'), JSON.stringify({error: error.message, last, pairs}, null, 2) + '\n', {flag: 'wx'});
    throw error;
  }
}

try { collect(); } catch (error) {
  console.error(`Cannot collect focused CoreMark: ${error.message}`);
  process.exitCode = 2;
}
