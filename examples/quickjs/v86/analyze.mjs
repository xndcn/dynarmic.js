#!/usr/bin/env node
// Summaries preserve round pairing. Upstream iterations are not independent
// repeats, and adaptive whole-call durations are not fixed-work timings.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {isDeepStrictEqual} from 'node:util';
import {pathToFileURL} from 'node:url';

export const CASES = Object.freeze([
  'Richards/Richards', 'DeltaBlue/DeltaBlue', 'Crypto/Encrypt', 'Crypto/Decrypt',
  'RayTrace/RayTrace', 'EarleyBoyer/Earley', 'EarleyBoyer/Boyer', 'RegExp/RegExp',
  'Splay/Splay', 'NavierStokes/NavierStokes',
]);
const RUNTIMES = ['dynarmic', 'v86'];
const SCHEMA = 'dynarmic.js/v86-quickjs-browser-comparison-v1';
const key = task => JSON.stringify([task.round, task.caseId, task.runtime]);
const finite = value => typeof value === 'number' && Number.isFinite(value);

function statistics(samples) {
  if (!samples.length) return null;
  const sorted = [...samples].sort((a, b) => a - b), middle = sorted.length >> 1;
  return {samples, count: samples.length,
    median: sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2,
    min: sorted[0], max: sorted.at(-1)};
}
const geometricMean = values => Math.exp(values.reduce((sum, value) => sum + Math.log(value), 0) / values.length);

function validateSpec(spec) {
  assert.equal(spec?.schema, SCHEMA, 'Unsupported comparison schema');
  assert(Array.isArray(spec.cases) && spec.cases.length === CASES.length &&
    new Set(spec.cases).size === CASES.length && spec.cases.every(id => CASES.includes(id)),
  'Experiment must contain exactly the ten official cases');
  assert(Number.isSafeInteger(spec.repeats) && spec.repeats > 0, 'Invalid repeats');
  assert(Array.isArray(spec.plan) && spec.plan.length === spec.repeats * CASES.length * 2,
    'Plan must cover all cases, runtimes and rounds');
  const ids = new Set(), combinations = new Set();
  for (const task of spec.plan) {
    assert(task && typeof task.id === 'string' && /^[\w-]+$/.test(task.id), 'Invalid task id');
    assert(!ids.has(task.id), `Duplicate task id: ${task.id}`);
    assert(Number.isSafeInteger(task.round) && task.round >= 0 && task.round < spec.repeats, 'Invalid task round');
    assert(CASES.includes(task.caseId) && RUNTIMES.includes(task.runtime), 'Unknown task case or runtime');
    assert(Number.isSafeInteger(task.position) && task.position >= 0, 'Invalid task position');
    assert(!combinations.has(key(task)), `Duplicate task combination: ${task.id}`);
    ids.add(task.id); combinations.add(key(task));
  }
}

function measure(task, result) {
  assert.equal(result?.ok, true, 'Worker did not report ok');
  assert.equal(result.runtime, task.runtime, 'Worker runtime differs from task');
  assert.equal(result.caseId, task.caseId, 'Worker case differs from task');
  const official = result.official;
  assert.equal(official?.caseId, task.caseId, 'Official case differs from task');
  assert.equal(official.status, 'pass', 'Official result did not pass');
  assert.equal(official.mode, 'upstream', 'Expected upstream timing mode');
  assert.equal(official.warmupMs, 1000, 'Changed upstream warmup');
  assert.equal(official.measureMs, 1000, 'Changed upstream measurement interval');
  assert.equal(official.minimumRuns, 32, 'Changed upstream minimum runs');
  const {runs, elapsedMs, samplesMs} = official;
  assert(Array.isArray(runs) && runs.length > 0 && Array.isArray(elapsedMs) &&
    Array.isArray(samplesMs) && runs.length === elapsedMs.length && runs.length === samplesMs.length,
  'Missing or unequal official sample arrays');
  for (let index = 0; index < runs.length; index++) {
    assert(Number.isSafeInteger(runs[index]) && runs[index] >= 32, 'Incomplete upstream iteration count');
    assert(finite(elapsedMs[index]) && elapsedMs[index] >= 1000, 'Incomplete upstream measurement batch');
    assert(finite(samplesMs[index]) && samplesMs[index] > 0, 'Invalid normalized timing');
    const normalized = elapsedMs[index] / runs[index];
    assert(Math.abs(normalized - samplesMs[index]) <= Math.max(1, normalized) * 1e-9,
      'Official normalized timing differs from elapsedMs / runs');
  }
  assert(finite(result.loadMs) && result.loadMs >= 0, 'Invalid loadMs');
  assert(finite(result.runMs) && result.runMs > 0, 'Invalid runMs');
  for (const field of ['memoryBytes', 'quickjsMemoryLimitBytes']) {
    assert(Number.isSafeInteger(result[field]) && result[field] > 0, `Invalid ${field}`);
  }
  assert.equal(result.quickjsMemoryLimitBytes, 192 * 1024 * 1024, 'Changed QuickJS memory limit');
  if (task.runtime === 'v86') {
    assert.equal(result.exitCode, 0, 'v86 guest did not report a successful exit');
    assert(Number.isSafeInteger(result.stats?.compiledSlots) && result.stats.compiledSlots > 0,
      'v86 JIT did not report compiled code');
    assert.equal(result.guestRamBytes, 256 * 1024 * 1024, 'Changed v86 guest RAM');
  }
  return {msPerIteration: statistics(samplesMs).median, internalSamples: samplesMs.length,
    runMs: result.runMs, loadMs: result.loadMs};
}

/**
 * observations is an array of persisted {task, status, result?, error?} records.
 * Invalid claimed passes become retained failures. Structural ambiguity (an
 * unknown/duplicate task or altered plan) throws instead of silently selecting.
 * Complete means every task has a record; verified additionally requires all
 * records to pass validation and every pair to share a nonempty attempt.
 * Overall summaries require every planned pair.
 */
export function summarize(spec, observations) {
  validateSpec(spec);
  assert(Array.isArray(observations), 'Observations must be an array');
  const planned = new Map(spec.plan.map(task => [task.id, task])), received = new Map();
  for (const observation of observations) {
    const task = planned.get(observation?.task?.id);
    assert(task, `Unknown observation task: ${observation?.task?.id}`);
    assert(!received.has(task.id), `Duplicate observation: ${task.id}`);
    assert(isDeepStrictEqual(task, observation.task), `Observation task differs from plan: ${task.id}`);
    assert(['pass', 'fail'].includes(observation.status), `Invalid observation status: ${task.id}`);
    const entry = {...observation};
    if (entry.status === 'pass') {
      try { entry.measurement = measure(task, entry.result); }
      catch (error) {
        entry.status = 'fail'; entry.reportedStatus = 'pass';
        entry.validationError = error.message;
      }
    }
    // Never trust metrics cached by the collector over the raw Worker report.
    if (entry.status !== 'pass') delete entry.measurement;
    received.set(task.id, entry);
  }
  const entries = spec.plan.map(task => received.get(task.id) || {task, status: 'missing'});
  const byCombination = new Map(entries.map(entry => [key(entry.task), entry]));
  const result = {
    schema: 'dynarmic.js/v86-quickjs-browser-summary-v1', experiment: spec,
    planned: spec.plan.length, completed: observations.length,
    passed: entries.filter(entry => entry.status === 'pass').length,
    failed: entries.filter(entry => entry.status === 'fail').length,
    missing: entries.filter(entry => entry.status === 'missing').length,
    complete: observations.length === spec.plan.length,
    verified: entries.every(entry => entry.status === 'pass'),
    ratio: 'v86 ms per iteration / dynarmic ms per iteration; above 1 favors dynarmic',
    aggregation: 'Compute paired case ratios and equal-weight eight-suite geometric means within each round, then summarize complete round ratios; do not divide unpaired time medians',
    cases: {}, rounds: [], pairingErrors: [], eightSuiteRatio: null, tenCaseRatio: null,
    observations: entries,
  };
  for (const caseId of spec.cases) {
    const item = {runtimes: {}, pairs: [], complete: false, pairedRatio: null};
    for (const runtime of RUNTIMES) {
      const successful = entries.filter(entry => entry.status === 'pass' && entry.task.caseId === caseId && entry.task.runtime === runtime);
      item.runtimes[runtime] = {completedSamples: successful.length, complete: successful.length === spec.repeats,
        ...Object.fromEntries(['msPerIteration', 'loadMs', 'runMs'].map(metric =>
          [metric, statistics(successful.map(entry => entry.measurement[metric]))]))};
    }
    for (let round = 0; round < spec.repeats; round++) {
      const [dynarmic, v86] = RUNTIMES.map(runtime => byCombination.get(key({round, caseId, runtime})));
      if (dynarmic.status === 'pass' && v86.status === 'pass') {
        if (![dynarmic, v86].every(entry => typeof entry.attempt === 'string' && entry.attempt.trim()) ||
            dynarmic.attempt !== v86.attempt) {
          result.pairingErrors.push({round, caseId, dynarmicAttempt: dynarmic.attempt ?? null,
            v86Attempt: v86.attempt ?? null, reason: 'Both observations must share the same nonempty attempt'});
          result.verified = false;
          continue;
        }
        const dynarmicMs = dynarmic.measurement.msPerIteration, v86Ms = v86.measurement.msPerIteration;
        item.pairs.push({round, dynarmicMs, v86Ms, ratio: v86Ms / dynarmicMs});
      }
    }
    item.complete = item.pairs.length === spec.repeats;
    if (item.complete) item.pairedRatio = statistics(item.pairs.map(pair => pair.ratio));
    result.cases[caseId] = item;
  }
  for (let round = 0; round < spec.repeats; round++) {
    const caseRatios = Object.fromEntries(spec.cases.flatMap(caseId => {
      const pair = result.cases[caseId].pairs.find(pair => pair.round === round);
      return pair ? [[caseId, pair.ratio]] : [];
    }));
    const complete = Object.keys(caseRatios).length === CASES.length;
    const summary = {round, complete, caseRatios, suiteRatios: {}, eightSuiteRatio: null, tenCaseRatio: null};
    if (complete) {
      for (const suite of new Set(CASES.map(id => id.split('/')[0]))) {
        summary.suiteRatios[suite] = geometricMean(CASES.filter(id => id.startsWith(suite + '/')).map(id => caseRatios[id]));
      }
      summary.eightSuiteRatio = geometricMean(Object.values(summary.suiteRatios));
      summary.tenCaseRatio = geometricMean(Object.values(caseRatios));
    }
    result.rounds.push(summary);
  }
  if (result.verified) {
    result.eightSuiteRatio = statistics(result.rounds.map(round => round.eightSuiteRatio));
    result.tenCaseRatio = statistics(result.rounds.map(round => round.tenCaseRatio));
  }
  return result;
}

const cell = value => `"${String(value ?? '').replaceAll('"', '""')}"`;
const fixed = value => value === null || value === undefined ? 'incomplete' : value.toFixed(3);
const range = stats => stats ? `${fixed(stats.median)} [${fixed(stats.min)}–${fixed(stats.max)}]` : 'incomplete';
const markdownText = value => String(value).replaceAll('|', '\\|').replaceAll('\n', ' ');

export function analyze(directory) {
  const spec = JSON.parse(fs.readFileSync(path.join(directory, 'experiment.json'), 'utf8'));
  validateSpec(spec);
  const results = path.join(directory, 'results'), expected = new Set(spec.plan.map(task => task.id + '.json'));
  if (fs.existsSync(results)) {
    for (const filename of fs.readdirSync(results)) {
      if (filename.endsWith('.json')) assert(expected.has(filename), `Unexpected result file: ${filename}`);
    }
  }
  const observations = [];
  for (const task of spec.plan) {
    const filename = path.join(results, task.id + '.json');
    try {
      const observation = JSON.parse(fs.readFileSync(filename, 'utf8'));
      assert.equal(observation?.task?.id, task.id, `Result filename differs from task: ${filename}`);
      observations.push(observation);
    }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  const summary = summarize(spec, observations);
  summary.recovery = {};
  for (const filename of fs.readdirSync(directory).sort()) {
    if (/^recovery.*\.json$/.test(filename) || filename === 'browser-crash-metadata.json') {
      summary.recovery[filename] = JSON.parse(fs.readFileSync(path.join(directory, filename), 'utf8'));
    }
  }
  const lines = ['# dynarmic.js and v86 QuickJS comparison', '',
    `${summary.completed}/${summary.planned} task records: ${summary.passed} passed, ${summary.failed} failed, ${summary.missing} missing. Verified: **${summary.verified}**.`, '',
    'Ratio = **v86 time / dynarmic time**; above 1 favors dynarmic. Timings are milliseconds per official invocation. Medians and min–max ranges describe independent paired rounds, not the upstream iterations inside each observation.', '',
    '| Official case | Paired rounds | dynarmic ms, median [min–max] | v86 ms, median [min–max] | Paired ratio, median [min–max] |',
    '| --- | ---: | ---: | ---: | ---: |'];
  for (const [caseId, item] of Object.entries(summary.cases)) {
    const timing = runtime => `${range(item.runtimes[runtime].msPerIteration)}${item.runtimes[runtime].complete ? '' : ' (partial)'}`;
    lines.push(`| ${caseId} | ${item.pairs.length}/${spec.repeats} | ${timing('dynarmic')} | ${timing('v86')} | ${range(item.pairedRatio)} |`);
  }
  lines.push('', '| Round | Complete | Eight-suite ratio | Ten-case ratio (secondary) |', '| --- | --- | ---: | ---: |');
  for (const round of summary.rounds) lines.push(`| ${round.round} | ${round.complete} | ${fixed(round.eightSuiteRatio)} | ${fixed(round.tenCaseRatio)} |`);
  lines.push('', `Eight-suite ratio, median [min–max]: **${range(summary.eightSuiteRatio)}**.`,
    `Ten-case equal-weight ratio, median [min–max]: ${range(summary.tenCaseRatio)}.`, '',
    'Each complete round first combines Crypto and EarleyBoyer case ratios geometrically, then weights all eight suites equally. The final summary requires every planned round and case to pass. A complete surviving round is shown separately if another round is incomplete; it is not substituted for the requested experiment.', '',
    'Paired ratios are computed before summarizing across rounds. Dividing the displayed unpaired timing medians can produce a different answer. Internal official samples are summarized within their observation and are not additional independent rounds.', '',
    'The JSON retains the experiment metadata and original Worker reports, including iteration counts, elapsed batches, memory and compilation statistics. JSON and CSV retain loadMs and runMs separately. Adaptive iteration counts make whole-call runMs unsuitable for a fixed-work throughput comparison. Preparation and execution durations are not added to official timing.');
  if (Object.keys(summary.recovery).length) {
    lines.push('', `Recovery metadata is retained in summary.recovery: ${Object.keys(summary.recovery).map(markdownText).join(', ')}. ` +
      'Interruptions and browser restarts are excluded from successful execution timings; recovery costs are not included in the throughput ratios.');
  }
  if (summary.pairingErrors.length) {
    lines.push('', 'Pairing errors prevent verification and overall summaries. Original observations remain available.', '',
      '| Round | Official case | dynarmic attempt | v86 attempt | Reason |', '| --- | --- | --- | --- | --- |');
    for (const error of summary.pairingErrors) lines.push(`| ${error.round} | ${error.caseId} | ${markdownText(error.dynarmicAttempt ?? '(missing)')} | ${markdownText(error.v86Attempt ?? '(missing)')} | ${error.reason} |`);
  }
  const unsuccessful = summary.observations.filter(entry => entry.status !== 'pass');
  if (unsuccessful.length) {
    lines.push('', '| Task | Status | Reason |', '| --- | --- | --- |');
    for (const entry of unsuccessful) lines.push(`| ${entry.task.id} | ${entry.status} | ${markdownText(entry.validationError || entry.error || (entry.status === 'missing' ? 'No result file' : 'Reported failure'))} |`);
  }
  const columns = ['id', 'round', 'position', 'caseId', 'runtime', 'status', 'reportedStatus', 'msPerIteration', 'pairedRatio',
    'runMs', 'loadMs', 'memoryBytes', 'quickjsMemoryLimitBytes', 'runs', 'elapsedMs', 'samplesMs', 'stats', 'error'];
  const rows = [columns.map(cell).join(',')];
  for (const entry of summary.observations) {
    const {task, result = {}} = entry, pair = summary.cases[task.caseId].pairs.find(pair => pair.round === task.round);
    rows.push([task.id, task.round, task.position, task.caseId, task.runtime, entry.status, entry.reportedStatus,
      entry.measurement?.msPerIteration, pair?.ratio, result.runMs, result.loadMs, result.memoryBytes, result.quickjsMemoryLimitBytes,
      ...['runs', 'elapsedMs', 'samplesMs'].map(field => JSON.stringify(result.official?.[field])), JSON.stringify(result.stats),
      entry.validationError || entry.error].map(cell).join(','));
  }
  return {summary, markdown: lines.join('\n') + '\n', csv: rows.join('\n') + '\n'};
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const [directory, option, prefix, ...extra] = process.argv.slice(2);
    assert(directory && option === '--output-prefix' && prefix && !extra.length,
      'Usage: node examples/quickjs/v86/analyze.mjs EXPERIMENT_DIRECTORY --output-prefix NEW_PREFIX');
    const targets = ['.json', '.md', '.csv'].map(extension => path.resolve(prefix + extension));
    assert(targets.every(filename => !fs.existsSync(filename)), 'Analysis output exists; choose a new prefix to retain previous evidence');
    const report = analyze(path.resolve(directory));
    fs.mkdirSync(path.dirname(targets[0]), {recursive: true});
    [JSON.stringify(report.summary, null, 2) + '\n', report.markdown, report.csv].forEach((text, index) =>
      fs.writeFileSync(targets[index], text, {flag: 'wx'}));
    console.log(`Wrote ${targets.join(', ')}`);
  } catch (error) { console.error(error.message); process.exitCode = 2; }
}
