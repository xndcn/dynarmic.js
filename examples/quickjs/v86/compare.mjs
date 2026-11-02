#!/usr/bin/env node
// Sequential browser Workers, alternating emulator order, with complete
// upstream workloads and immutable, resumable per-observation reports.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {withChromium, pause} from '../../../tests/browser/chromium.mjs';
import {CASES, acquireLock, recoverResultTemps} from '../tune-arm.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const hash = filename => {const b = fs.readFileSync(filename); return {bytes: b.length, sha256: createHash('sha256').update(b).digest('hex')};};
const options = {dist: 'build-v86-quickjs/browser-dist', browser: '/usr/bin/google-chrome', repeats: 3,
  timeout: 600000, cases: CASES, resume: false};
for (let i = 2; i < process.argv.length; i++) {
  const arg = process.argv[i];
  if (arg === '--resume') {options.resume = true; continue;}
  assert(['--dist','--browser','--repeats','--timeout','--cases','--output'].includes(arg) && process.argv[i + 1], 'Invalid option ' + arg);
  const key = arg.slice(2), value = process.argv[++i];
  options[key] = ['repeats','timeout'].includes(key) ? Number(value) : key === 'cases' ? value === 'all' ? CASES : value.split(',') : value;
}
assert(options.output, '--output is required');
assert(Number.isSafeInteger(options.repeats) && options.repeats >= 1 && options.repeats <= 10);
assert(Number.isSafeInteger(options.timeout) && options.timeout >= 1000 && options.timeout <= 3600000);
assert(options.cases.length && new Set(options.cases).size === options.cases.length && options.cases.every(c => CASES.includes(c)));
const dist = path.resolve(options.dist), output = path.resolve(options.output);
const inputs = JSON.parse(fs.readFileSync(path.join(dist, 'comparison-inputs.json')));
assert.equal(inputs.schema, 'dynarmic.js/v86-quickjs-inputs-v1');
const identities = () => Object.fromEntries(Object.keys(inputs.artifacts).map(n => [n, hash(path.join(dist, n))]));
assert.deepEqual(identities(), inputs.artifacts, 'Prepared artifacts changed');
const readManifest = name => JSON.parse(fs.readFileSync(path.join(dist, name), 'utf8'));
const arm = readManifest('quickjs-manifest.json'), host = readManifest('arm-bench-build-manifest.json');
const x86 = readManifest('quickjs-x86-manifest.json');
assert.equal(arm.configuration.targets.arm.optimization, 'O2');
assert.equal(arm.configuration.targets.arm.dispatch, 'switch');
assert.equal(arm.configuration.targets.arm.isa, 'arm');
for (const option of ['lto', 'noGcse', 'noCrossjumping']) assert.equal(arm.configuration.targets.arm[option], false);
assert.equal(x86.configuration.optimization, 'O2'); assert.equal(x86.configuration.dispatch, 'switch');
assert.equal(x86.configuration.lto, false); assert.equal(x86.configuration.officialMemoryLimitBytes, 192 * 1024 * 1024);
assert.equal(x86.quickjs.version, arm.upstream.version); assert.equal(x86.quickjs.archive.sha256, arm.upstream.sha256);
assert.equal(x86.derivedSources.find(f => path.basename(f.path) === 'quickjs.c').sha256,
  arm.configuration.targets.arm.engineSource.sha256, 'QuickJS interpreter source differs');
for (const [manifest, names] of [[arm, ['quickjs-arm.bin', 'official-v8.js', 'official-v8-adapter.js']],
  [x86, ['quickjs-x86.elf', 'official-v8.js', 'official-v8-adapter.js']]]) {
  for (const name of names) {
    const entry = manifest.artifacts.find(f => path.basename(f.path) === name);
    assert(entry, 'Manifest omits ' + name);
    assert.deepEqual(inputs.artifacts[name], {bytes: entry.bytes, sha256: entry.sha256}, 'Stale artifact ' + name);
  }
}
assert.equal(host.build.type, 'Release'); assert.equal(host.build.profile, false);
assert.equal(host.configuration.regionDiagnostics, false);
for (const [kind, extension] of [['js','.js'], ['wasm','.wasm']])
  assert.deepEqual(inputs.artifacts['arm_bench_jit' + extension], {bytes: host.artifacts[kind].bytes, sha256: host.artifacts[kind].sha256});
assert.equal(host.configuration.guestRamBytes, 256 * 1024 * 1024);
const collectorFiles = [fileURLToPath(import.meta.url), path.resolve(HERE, '../../../tests/browser/chromium.mjs'), path.resolve(HERE, '../tune-arm.mjs')];
const collector = () => Object.fromEntries(collectorFiles.map(f => [path.relative(HERE, f), hash(f)]));
const plan = [];
for (let round = 0; round < options.repeats; round++) for (let offset = 0; offset < options.cases.length; offset++) {
  const index = (round + offset) % options.cases.length, order = (round + index) % 2 ? ['v86','dynarmic'] : ['dynarmic','v86'];
  order.forEach((runtime, position) => plan.push({id: `r${round}-c${index}-${runtime}`, round, caseId: options.cases[index], runtime, position}));
}
const identity = {schema: 'dynarmic.js/v86-quickjs-browser-comparison-v1', cases: options.cases, repeats: options.repeats,
  timeoutMs: options.timeout, browser: path.resolve(options.browser), dist, plan, inputs, collector: collector(),
  quickjsArm: arm, dynarmicHost: host, quickjsX86: x86,
  environment: {node: process.version, platform: process.platform, arch: process.arch, os: os.release(),
    cpu: os.cpus()[0]?.model, cpuCount: os.cpus().length, memoryBytes: os.totalmem(),
    affinity: process.platform === 'linux' ? fs.readFileSync('/proc/self/status','utf8').match(/^Cpus_allowed_list:\s*(.*)$/m)?.[1] : null},
  methodology: {round: 'One new Chromium process per round; one fresh Worker and emulator per case/runtime',
    timing: 'Unchanged official V8 v7 warmup/measurement batches, minimum 32 calls; host monotonic clock in both guests',
    order: 'Rotate case order; alternate dynarmic/v86 order within each case across rounds',
    ratio: 'v86 per-iteration time / dynarmic time; above 1 favors dynarmic',
    aggregate: 'Per round combine Crypto/EarleyBoyer cases geometrically, then eight suites equally; summarize round ratios',
    scope: 'Complete QuickJS execution paths: ARM and x86 ISA, compilers, libc builds, JITs and scheduling differ'}};
function writeNew(filename, value) {
  const temporary = filename + `.tmp-${process.pid}-${Date.now()}`;
  fs.writeFileSync(temporary, JSON.stringify(value, null, 2) + '\n', {flag: 'wx'});
  try {fs.linkSync(temporary, filename);} finally {fs.unlinkSync(temporary);}
}
function processIdentity(pid) {
  const result = {pid, start: null};
  if (process.platform === 'linux') {
    const stat = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
    result.start = stat.slice(stat.lastIndexOf(')') + 2).split(' ')[19];
  }
  return result;
}
function validate(task, r) {
  assert.equal(r.ok, true, r.error); assert.equal(r.runtime, task.runtime); assert.equal(r.caseId, task.caseId);
  assert.equal(r.quickjsMemoryLimitBytes, 192 * 1024 * 1024);
  assert(Number.isFinite(r.loadMs) && r.loadMs >= 0 && Number.isFinite(r.runMs) && r.runMs > 0);
  const o = r.official; assert.equal(o.status, 'pass'); assert.equal(o.caseId, task.caseId); assert.equal(o.mode, 'upstream');
  assert.equal(o.warmupMs, 1000); assert.equal(o.measureMs, 1000); assert.equal(o.minimumRuns, 32);
  assert.equal(o.runs.length, 1); assert(o.runs[0] >= 32 && Number.isSafeInteger(o.runs[0]));
  assert.equal(o.samplesMs.length, 1); assert(o.elapsedMs[0] >= 1000);
  assert(Math.abs(o.samplesMs[0] - o.elapsedMs[0] / o.runs[0]) < 1e-7);
  if (task.runtime === 'v86') {assert.equal(r.exitCode, 0); assert(r.stats.compiledSlots > 0); assert.equal(r.guestRamBytes, 256 * 1024 * 1024);}
}
let spec;
if (options.resume) {
  spec = readExperiment(); const {createdAt, ...prior} = spec;
  assert.deepEqual(prior, identity, 'Resume inputs changed; select a new output directory');
} else {
  assert(!fs.existsSync(output), 'Output exists; use --resume with unchanged inputs');
  fs.mkdirSync(output, {recursive: true});
  fs.mkdirSync(path.join(output, 'results')); fs.mkdirSync(path.join(output, 'attempts'));
  spec = {...identity, createdAt: new Date().toISOString()};
  writeNew(path.join(output, 'experiment.json'), spec);
}
function readExperiment() {return JSON.parse(fs.readFileSync(path.join(output, 'experiment.json'), 'utf8'));}
const release = acquireLock(output);
try {
  recoverResultTemps(output, spec);
  const records = new Map();
  for (const task of plan) {
    const f = path.join(output, 'results', task.id + '.json');
    if (!fs.existsSync(f)) continue;
    const r = JSON.parse(fs.readFileSync(f)); assert.deepEqual(r.task, task);
    assert(['pass','fail'].includes(r.status)); if (r.status === 'pass') validate(task, r.result);
    records.set(task.id, r);
  }
  if (options.resume) {
    const pairs = new Map();
    for (const task of plan) {
      if (!records.has(task.id)) continue;
      const key = JSON.stringify([task.round, task.caseId]);
      if (!pairs.has(key)) pairs.set(key, []);
      pairs.get(key).push(task);
    }
    const singleSided = [...pairs.values()].filter(tasks => tasks.length === 1).map(tasks => tasks[0].id);
    assert(!singleSided.length,
      `Cannot resume single-sided recorded pairs: ${singleSided.join(', ')}. ` +
      'Preserve each existing observation under superseded/ and record the reason and SHA-256 hash, ' +
      'then remove it from active results and rerun the entire pair.');
  }
  for (let round = 0; round < options.repeats; round++) {
    const tasks = plan.filter(t => t.round === round && !records.has(t.id));
    if (!tasks.length) continue;
    const attempt = fs.mkdtempSync(path.join(output, 'attempts', `round-${round}-`));
    await withChromium({browser: options.browser, root: dist}, async ({browserPid, evaluate, navigate}) => {
      writeNew(path.join(attempt, 'process.json'), {...processIdentity(browserPid), parentPid: process.pid});
      await navigate();
      const browserEnvironment = await evaluate('({userAgent:navigator.userAgent,visibility:document.visibilityState,hardwareConcurrency:navigator.hardwareConcurrency})');
      assert.equal(browserEnvironment.visibility, 'visible', 'Benchmark page must remain visible');
      for (const prior of fs.readdirSync(path.join(output, 'attempts'))) {
        const f = path.join(output, 'attempts', prior, 'environment.json');
        if (fs.existsSync(f)) assert.equal(browserEnvironment.userAgent, JSON.parse(fs.readFileSync(f)).userAgent, 'Browser version changed between rounds');
      }
      writeNew(path.join(attempt, 'environment.json'), browserEnvironment);
      for (const task of tasks) {
        assert.deepEqual(identities(), inputs.artifacts, 'Runtime inputs changed');
        assert.deepEqual(collector(), identity.collector, 'Collector changed during measurement');
        const startedAt = new Date().toISOString(), started = performance.now(), loadAverageBefore = os.loadavg();
        console.log(`RUN ${task.id} ${task.caseId}`);
        await evaluate(`(() => {
          window.caseResult = null;
          const worker = new Worker(${JSON.stringify(task.runtime === 'dynarmic' ? 'official-benchmarks-worker.js' : 'v86-worker.js')});
          let done = false;
          const finish = result => {if(done)return;done=true;clearTimeout(timer);worker.terminate();window.caseResult=result;};
          const timer = setTimeout(() => finish({ok:false,error:'Browser Worker timeout'}), ${options.timeout});
          worker.onerror = event => finish({ok:false,error:event.message});
          worker.onmessage = event => {if(event.data.type!=='progress')finish(event.data);};
          worker.postMessage(${JSON.stringify({runtime: task.runtime, caseId: task.caseId, timeoutMs: options.timeout})});
        })()`);
        let result;
        while (!(result = await evaluate('window.caseResult'))) {
          assert(performance.now() - started < options.timeout + 30000, 'Browser result deadline exceeded');
          await pause(500);
        }
        const observation = {task, status: 'fail', startedAt, finishedAt: new Date().toISOString(),
          wallMs: performance.now() - started, loadAverageBefore, loadAverageAfter: os.loadavg(),
          attempt: path.relative(output, attempt), result};
        try {
          validate(task, result); assert.deepEqual(identities(), inputs.artifacts); assert.deepEqual(collector(), identity.collector);
          observation.status = 'pass';
        } catch (error) {observation.error = error.message;}
        writeNew(path.join(output, 'results', task.id + '.json'), observation); records.set(task.id, observation);
        console.log(`${observation.status.toUpperCase()} ${task.id}${observation.error ? ': ' + observation.error : ''}`);
      }
    });
    writeNew(path.join(attempt, 'completion.json'), {round, finishedAt: new Date().toISOString()});
  }
  const failed = [...records.values()].filter(r => r.status !== 'pass').length;
  const summary = {planned: plan.length, completed: records.size, failed, verified: records.size === plan.length && failed === 0};
  console.log(JSON.stringify(summary)); if (!summary.verified) process.exitCode = 1;
} finally {release();}
