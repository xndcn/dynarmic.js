/* Run the original QuickJS extras V8 v7 harness. Benchmark bodies, the
 * one-second warmup, one-second measurement batches, minimum 32 runs, setup,
 * teardown and assertions all remain upstream-owned. The generated source
 * adapts only the harness clock to a monotonic host clock. */
(function (global) {
  'use strict';
  const source = global.OfficialV8Source;
  if (!source) throw new Error('official-v8.js must be loaded first');
  const lookup = new Map();
  for (const suite of BenchmarkSuite.suites) {
    for (const benchmark of suite.benchmarks) lookup.set(`${suite.name}/${benchmark.name}`, {suite, benchmark});
  }
  function summary(values) {
    const sorted = values.slice().sort((a, b) => a - b), mid = sorted.length >> 1;
    const meanMs = values.reduce((a, b) => a + b, 0) / values.length;
    return {minMs: sorted[0], maxMs: sorted[sorted.length - 1],
      medianMs: sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2,
      meanMs, stddevMs: Math.sqrt(values.reduce((a, b) => a + (b - meanMs) ** 2, 0) / values.length)};
  }
  function run(caseId, options, now) {
    options = options || {};
    if (!lookup.has(caseId)) throw new RangeError(`Unknown official V8 benchmark: ${caseId}`);
    for (const key of ['warmupMs', 'measureMs', 'minimumRuns']) {
      if (options[key] !== undefined) throw new RangeError('Official V8 mode does not permit timing overrides: ' + key);
    }
    const repeats = options.samples === undefined ? 1 : options.samples;
    if (!Number.isInteger(repeats) || repeats < 1 || repeats > 10) throw new RangeError('Samples must be 1..10 complete official runs');
    if (typeof now !== 'function') throw new TypeError('A monotonic clock is required');
    const {suite, benchmark} = lookup.get(caseId);
    const allBenchmarks = suite.benchmarks;
    const originalSingle = suite.RunSingleBenchmark;
    const previousClock = global.__officialV8Clock;
    const oldConsole = global.console;
    const logs = [], batches = [], samplesMs = [], runs = [], elapsedMs = [];
    let previousTime = -Infinity, failure = null, measured = null;
    const dependencies = caseId === 'Crypto/Decrypt' ? ['Crypto/Encrypt'] : [];
    global.__officialV8Clock = () => {
      const value = now();
      if (!Number.isFinite(value) || value < previousTime) throw new Error('Invalid monotonic benchmark clock');
      previousTime = value;
      return value;
    };
    global.console = {log: (...args) => logs.push(args.join(' ')), warn: (...args) => logs.push(args.join(' ')),
      error: (...args) => { throw new Error(args.join(' ')); }};
    suite.benchmarks = [benchmark];
    suite.RunSingleBenchmark = function (selected, data) {
      const startRuns = data ? data.runs : 0, startElapsed = data ? data.elapsed : 0;
      const start = now();
      const next = originalSingle.call(this, selected, data);
      if (!data) batches.push({phase: 'warmup', elapsedMs: now() - start, runs: null});
      else {
        batches.push({phase: 'measurement', elapsedMs: data.elapsed - startElapsed, runs: data.runs - startRuns});
        if (next === null) measured = {runs: data.runs, elapsedMs: data.elapsed};
      }
      return next;
    };
    try {
      // In the original suite Encrypt precedes Decrypt and supplies its input.
      // A standalone Decrypt case must prepare that input outside measurement.
      if (dependencies.length) lookup.get('Crypto/Encrypt').benchmark.run();
      for (let repeat = 0; repeat < repeats; repeat++) {
        measured = null;
        BenchmarkSuite.scores = [];
        let result = null;
        const runner = {NotifyError: (name, error) => { failure = new Error(`${name}: ${error?.stack || error}`); },
          NotifyStep: () => {}, NotifyResult: () => { result = suite.results[0]; }};
        let continuation = suite.RunStep(runner);
        while (continuation && !failure) continuation = continuation();
        if (failure) throw failure;
        if (!result || !measured || measured.runs < 32 || measured.elapsedMs < 1000) throw new Error('Incomplete upstream V8 result');
        if (!Number.isFinite(result.time) || result.time <= 0) throw new Error('Invalid upstream V8 time');
        samplesMs.push(result.time / 1000);
        runs.push(measured.runs);
        elapsedMs.push(measured.elapsedMs);
      }
      // The original Crypto suite validates encryption in its following
      // Decrypt benchmark. Retain that check for a standalone Encrypt row.
      if (caseId === 'Crypto/Encrypt') lookup.get('Crypto/Decrypt').benchmark.run();
      const stats = summary(samplesMs);
      const completeSuite = allBenchmarks.length === 1;
      return {schema: 'dynarmic.js/official-v8-case-v1', version: '7', mode: 'upstream', suite: suite.name,
        caseId, name: benchmark.name, status: 'pass', referenceUs: suite.reference,
        warmupMs: 1000, measureMs: 1000, minimumRuns: 32, warmRuns: null,
        samplesMs, summary: stats, runs, elapsedMs, batches, microsecondsPerRun: samplesMs.map(value => value * 1000),
        score: completeSuite ? 100 * suite.reference / (stats.medianMs * 1000) : null,
        completeSuite, suiteCases: allBenchmarks.map(item => `${suite.name}/${item.name}`),
        logs, dependencies, validation: {upstreamAssertions: true, setupAndTearDown: true,
          independentOracle: false, scope: 'All checks present in the upstream case; RegExp and NavierStokes have no numerical result assertion'},
        methodology: 'Upstream V8 v7 harness: one-second warmup, one-second measurement batches until at least 32 runs; host monotonic clock'};
    } finally {
      suite.benchmarks = allBenchmarks;
      suite.RunSingleBenchmark = originalSingle;
      global.__officialV8Clock = previousClock;
      global.console = oldConsole;
    }
  }
  global.OfficialV8Bench = {catalog: source.cases.map(entry => entry.id), run};
})(globalThis);
