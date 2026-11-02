import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {analyze, CASES, summarize} from './analyze.mjs';

function fixture() {
  const plan = [];
  for (let round = 0; round < 3; round++) for (const [index, caseId] of CASES.entries()) {
    for (const runtime of round % 2 ? ['v86', 'dynarmic'] : ['dynarmic', 'v86']) {
      plan.push({id: `r${round}-c${index}-${runtime}`, round, caseId, runtime, position: plan.length});
    }
  }
  const spec = {schema: 'dynarmic.js/v86-quickjs-browser-comparison-v1', cases: [...CASES], repeats: 3, plan,
    environment: {browser: 'test browser'}, artifacts: {source: 'retained source hash'}};
  const observations = plan.map(task => {
    let ms = 10;
    if (task.caseId === 'Richards/Richards') ms = (task.runtime === 'dynarmic' ? [10, 100, 10] : [20, 100, 100])[task.round];
    if (task.runtime === 'v86' && task.caseId === 'Crypto/Encrypt') ms *= [1, 10, 10][task.round];
    if (task.runtime === 'v86' && task.caseId === 'Crypto/Decrypt') ms *= [10, 1, 10][task.round];
    const runs = Math.max(32, Math.ceil(1000 / ms));
    return {task, status: 'pass', attempt: `attempts/round-${task.round}-fixture`, result: {ok: true, runtime: task.runtime, caseId: task.caseId,
      loadMs: 17, runMs: task.runtime === 'dynarmic' ? 777 : 8888,
      memoryBytes: 256 * 1024 * 1024, quickjsMemoryLimitBytes: 192 * 1024 * 1024,
      ...(task.runtime === 'v86' ? {exitCode: 0, guestRamBytes: 256 * 1024 * 1024, stats: {compiledSlots: 123}} : {stats: {compiledBlocks: 123}}),
      official: {caseId: task.caseId, status: 'pass', mode: 'upstream',
        warmupMs: 1000, measureMs: 1000, minimumRuns: 32, runs: [runs], elapsedMs: [runs * ms], samplesMs: [ms]}}};
  });
  return {spec, observations};
}
const close = (actual, expected) => assert(Math.abs(actual - expected) < 1e-12, `${actual} != ${expected}`);

test('paired rounds preserve time pairing and equal-suite weighting before the final median', () => {
  const {spec, observations} = fixture(), summary = summarize(spec, observations);
  assert.equal(summary.verified, true);
  assert.equal(summary.completed, 60);
  const richards = summary.cases['Richards/Richards'];
  assert.equal(richards.pairedRatio.median, 2);
  assert.equal(richards.runtimes.v86.msPerIteration.median / richards.runtimes.dynarmic.msPerIteration.median, 10);
  assert.equal(richards.runtimes.v86.runMs.median, 8888);
  close(summary.rounds[0].suiteRatios.Crypto, Math.sqrt(10));
  close(summary.eightSuiteRatio.median, Math.pow(2 * Math.sqrt(10), 1 / 8));
  close(summary.eightSuiteRatio.min, Math.pow(Math.sqrt(10), 1 / 8));
  close(summary.eightSuiteRatio.max, Math.pow(100, 1 / 8));
  close(summary.tenCaseRatio.median, Math.pow(20, 1 / 10));
  assert.equal(summary.eightSuiteRatio.count, 3);
  assert.equal(summary.observations[0].result.official.runs.length, 1);
});

test('a missing case keeps partial evidence but prevents complete-round and overall summaries', () => {
  const {spec, observations} = fixture();
  const missing = observations.findIndex(entry => entry.task.round === 1 && entry.task.caseId === 'Crypto/Decrypt' && entry.task.runtime === 'v86');
  observations.splice(missing, 1);
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'quickjs-v86-analysis-'));
  try {
    fs.mkdirSync(path.join(directory, 'results'));
    fs.writeFileSync(path.join(directory, 'experiment.json'), JSON.stringify(spec));
    for (const entry of observations) fs.writeFileSync(path.join(directory, 'results', entry.task.id + '.json'), JSON.stringify(entry));
    const {summary, markdown, csv} = analyze(directory);
    assert.equal(summary.missing, 1);
    assert.equal(summary.complete, false);
    assert.equal(summary.verified, false);
    assert.equal(summary.rounds[1].eightSuiteRatio, null);
    assert(summary.rounds[0].eightSuiteRatio > 1);
    assert.equal(summary.eightSuiteRatio, null);
    assert.equal(summary.tenCaseRatio, null);
    assert.equal(summary.cases['Crypto/Decrypt'].pairedRatio, null);
    assert.equal(summary.cases['Crypto/Decrypt'].pairs.length, 2);
    assert.deepEqual(summary.experiment.artifacts, spec.artifacts);
    assert.match(markdown, /No result file/);
    assert.match(markdown, /whole-call runMs unsuitable/);
    assert.equal(csv.trim().split('\n').length, 61);
    assert.match(csv, /"8888","17"/);
    assert.match(csv, /"missing"/);
    assert.equal(fs.readdirSync(path.join(directory, 'results')).length, 59);
    fs.writeFileSync(path.join(directory, 'results', observations[0].task.id + '.json'), JSON.stringify(observations[1]));
    assert.throws(() => analyze(directory), /filename differs from task/);
  } finally { fs.rmSync(directory, {recursive: true, force: true}); }
});

test('failures and inconsistent Worker timing cannot become successful benchmark evidence', () => {
  const {spec, observations} = fixture();
  observations[0] = {...observations[0], status: 'fail', error: 'timeout after 600000 ms'};
  observations[1].result.official.samplesMs[0] *= 2;
  observations[2].result.ok = false;
  delete observations[3].result.exitCode;
  const summary = summarize(spec, observations);
  assert.equal(summary.complete, true);
  assert.equal(summary.verified, false);
  assert.equal(summary.failed, 4);
  assert.equal(summary.passed, 56);
  assert.equal(summary.eightSuiteRatio, null);
  assert.equal(summary.observations[0].error, 'timeout after 600000 ms');
  assert.equal(summary.observations[1].reportedStatus, 'pass');
  assert.match(summary.observations[1].validationError, /normalized timing/);
  assert.match(summary.observations[3].validationError, /successful exit/);
  assert.equal(summary.observations[1].measurement, undefined);
  assert.equal(observations[1].status, 'pass', 'Original evidence must not be mutated');
  assert.throws(() => summarize(spec, [...observations, observations[0]]), /Duplicate observation/);
  const altered = structuredClone(observations);
  altered[3].task.position += 1;
  assert.throws(() => summarize(spec, altered), /differs from plan/);
  assert.throws(() => summarize({...spec, plan: spec.plan.slice(1)}, []), /Plan must cover/);
});

test('different or missing attempts retain observations but prevent pairing and overall summaries', () => {
  for (const attempts of [['attempts/initial', 'attempts/restart'], [undefined, 'attempts/present'],
    [undefined, undefined], ['', ''], ['  ', '  '], [42, 42]]) {
    const {spec, observations} = fixture();
    for (const [index, attempt] of attempts.entries()) {
      if (attempt === undefined) delete observations[index].attempt;
      else observations[index].attempt = attempt;
    }
    const original = structuredClone(observations), summary = summarize(spec, observations);
    assert.equal(summary.complete, true);
    assert.equal(summary.verified, false);
    assert.equal(summary.passed, 60);
    assert.equal(summary.failed, 0);
    assert.equal(summary.pairingErrors.length, 1);
    assert.equal(summary.pairingErrors[0].round, 0);
    assert.equal(summary.pairingErrors[0].caseId, 'Richards/Richards');
    assert.equal(summary.cases['Richards/Richards'].pairs.length, 2);
    assert.equal(summary.cases['Richards/Richards'].pairedRatio, null);
    assert.equal(summary.rounds[0].eightSuiteRatio, null);
    assert.equal(summary.eightSuiteRatio, null);
    assert.equal(summary.tenCaseRatio, null);
    assert.deepEqual(observations, original, 'Original evidence must not be mutated');
    assert.deepEqual(summary.observations[0].result, original[0].result);
    assert.equal(summary.observations[0].attempt, original[0].attempt);
  }
});

test('analysis retains recovery metadata and explains interruption costs without changing valid ratios', () => {
  const {spec, observations} = fixture(), expected = summarize(spec, observations);
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'quickjs-v86-recovery-'));
  const recovery = {
    'recovery.json': {reason: 'Interrupted pair', sha256: 'retained observation hash'},
    'recovery-2.json': {reason: 'Restarted browser'},
    'browser-crash-metadata.json': {signal: 'SIGSEGV'},
  };
  try {
    fs.mkdirSync(path.join(directory, 'results'));
    fs.writeFileSync(path.join(directory, 'experiment.json'), JSON.stringify(spec));
    for (const entry of observations) fs.writeFileSync(path.join(directory, 'results', entry.task.id + '.json'), JSON.stringify(entry));
    for (const [filename, value] of Object.entries(recovery)) fs.writeFileSync(path.join(directory, filename), JSON.stringify(value));
    const {summary, markdown} = analyze(directory);
    assert.equal(summary.verified, true);
    assert.deepEqual(summary.recovery, recovery);
    assert.deepEqual(summary.eightSuiteRatio, expected.eightSuiteRatio);
    assert.deepEqual(summary.tenCaseRatio, expected.tenCaseRatio);
    assert.deepEqual(summary.cases, expected.cases);
    assert.match(markdown, /Interruptions and browser restarts are excluded from successful execution timings/);
    assert.match(markdown, /browser-crash-metadata\.json/);
    observations[0].attempt = 'attempts/different-browser';
    fs.writeFileSync(path.join(directory, 'results', observations[0].task.id + '.json'), JSON.stringify(observations[0]));
    const invalid = analyze(directory);
    assert.match(invalid.markdown, /Pairing errors prevent verification/);
    assert.match(invalid.markdown, /Both observations must share the same nonempty attempt/);
    assert.equal(invalid.summary.eightSuiteRatio, null);
  } finally { fs.rmSync(directory, {recursive: true, force: true}); }
});
