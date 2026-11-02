/* Shared validation for the three C execution paths. Also loaded by Node tests. */
(function (global) {
  'use strict';
  const CASE_IDS = ['integer', 'branch', 'stream', 'pointer', 'float'];
  const RUNTIMES = ['dynarmic', 'v86', 'wasm'];
  const RESOLUTION_MS = 0.1;
  const uint = value => Number.isSafeInteger(value) && value >= 0 && value <= 0xffffffff;
  const timing = value => typeof value === 'number' && Number.isFinite(value) && value >= 0;
  const require = (condition, message) => { if (!condition) throw new Error(message); };
  function statistics(values) {
    if (!values.length) return null;
    const sorted = [...values].sort((a, b) => a - b), middle = sorted.length >> 1;
    return {samples: [...values], median: sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2,
      min: sorted[0], max: sorted[sorted.length - 1]};
  }
  function validateReference(ref) {
    require(ref?.schema === 'dynarmic.js/c-bench-reference-v1', 'Invalid C benchmark reference');
    require(Array.isArray(ref.cases) && ref.cases.length === CASE_IDS.length &&
      ref.cases.every((c, i) => c.id === CASE_IDS[i] && c.index === i && typeof c.title === 'string'), 'Incomplete C benchmark cases');
    const expected = {firstRuns: 1, warmupRuns: 2, measuredRuns: 3, totalRuns: 6, rounds: 3,
      guestRamBytes: 16777216, mailboxAddress: 0xf000, magic: 0x43424e31};
    for (const [key, value] of Object.entries(expected)) require(ref.protocol?.[key] === value, 'Changed C benchmark protocol: ' + key);
    for (const profile of ['quick', 'full']) {
      const config = ref.profiles?.[profile]; require(uint(config?.seed), 'Invalid benchmark seed');
      for (const id of CASE_IDS) {
        require(uint(config.iterations?.[id]) && config.iterations[id] > 0 && config.iterations[id] <= 100000000,
          'Invalid benchmark work count: ' + id);
        require(uint(config.expected?.[id]), 'Missing reference checksum: ' + id);
      }
    }
    return ref;
  }
  function validateManifest(manifest) {
    require(manifest?.schema === 'dynarmic.js/c-bench-build-v1' && typeof manifest.compiler?.version === 'string', 'Missing C compiler provenance');
    require(Array.isArray(manifest.commonFlags) && manifest.commonFlags.includes('-O2'), 'Missing shared optimization flags');
    require(['arm','x86','wasm'].every(id => manifest.targets?.[id]), 'Missing build targets');
    const required = ['c-bench-arm.bin','c-bench-x86.elf','c-bench.wasm','c-bench-reference.json',
      'c-bench-source.c','c-bench-v86.js','c-bench-v86.wasm','c-bench-v86-LICENSE.txt'];
    require(Array.isArray(manifest.artifacts) && new Set(manifest.artifacts.map(x => x.path)).size === manifest.artifacts.length,
      'Invalid C benchmark artifact list');
    for (const name of required) {
      const record = manifest.artifacts.find(x => x.path === name);
      require(record && Number.isSafeInteger(record.bytes) && record.bytes > 0 && /^[a-f0-9]{64}$/.test(record.sha256), 'Missing artifact identity: ' + name);
    }
    return manifest;
  }
  function validateResult(result, request, ref) {
    validateReference(ref);
    require(result?.ok === true, result?.error || 'C benchmark Worker failed');
    for (const field of ['runtime','caseId','profile','round','token']) require(result[field] === request[field], 'Mismatched Worker ' + field);
    require(RUNTIMES.includes(result.runtime) && CASE_IDS.includes(result.caseId) && ['quick','full'].includes(result.profile), 'Invalid benchmark identity');
    require(Number.isInteger(result.round) && result.round >= 0 && result.round < ref.protocol.rounds, 'Invalid benchmark round');
    const input = ref.profiles[result.profile], expected = input.expected[result.caseId];
    require(result.iterations === input.iterations[result.caseId] && result.seed === input.seed, 'Worker changed the input or work count');
    require(result.expected === expected && Array.isArray(result.checksums) && result.checksums.length === 6 &&
      result.checksums.every(value => value === expected), 'C benchmark checksum mismatch');
    require(result.validation?.pass === true && result.validation.actual === expected && result.validation.expected === expected,
      'Missing benchmark correctness verification');
    require(timing(result.loadMs) && timing(result.firstMs), 'Invalid preparation or first-call timing');
    require(Array.isArray(result.warmupMs) && result.warmupMs.length === 2 && result.warmupMs.every(timing), 'Invalid warmup samples');
    require(Array.isArray(result.samplesMs) && result.samplesMs.length === 3 && result.samplesMs.every(timing), 'Invalid measured samples');
    require(Number.isSafeInteger(result.memoryBytes) && result.memoryBytes >= 16777216, 'Invalid runtime memory capacity');
    require(result.stats && typeof result.stats === 'object' && !Array.isArray(result.stats) && Object.values(result.stats).every(timing), 'Invalid runtime statistics');
    return result;
  }
  function summarize(records, caseIds = CASE_IDS, rounds = 3) {
    const cases = {}, ids = new Set();
    for (const r of records) {
      const id = `${r.caseId}/${r.round}/${r.runtime}`;
      require(!ids.has(id), 'Duplicate benchmark observation'); ids.add(id);
    }
    for (const caseId of caseIds) {
      const item = {runtimes: {}, ratios: {v86OverDynarmic: null, dynarmicOverWasm: null}, complete: false, belowResolution: false};
      const byRuntime = {};
      for (const runtime of RUNTIMES) {
        const observations = records.filter(r => r.caseId === caseId && r.runtime === runtime).sort((a, b) => a.round - b.round);
        byRuntime[runtime] = new Map(observations.map(r => [r.round, statistics(r.samplesMs).median]));
        const warm = statistics([...byRuntime[runtime].values()]);
        item.runtimes[runtime] = {rounds: observations.length, firstMs: statistics(observations.map(r => r.firstMs)),
          loadMs: statistics(observations.map(r => r.loadMs)), warmMs: warm,
          belowResolution: !!warm && warm.min < RESOLUTION_MS};
      }
      item.complete = RUNTIMES.every(runtime => Array.from({length: rounds}, (_, i) => i).every(i => byRuntime[runtime].has(i)));
      item.belowResolution = RUNTIMES.some(runtime => item.runtimes[runtime].belowResolution);
      if (item.complete) {
        for (const [name, numerator, denominator] of [['v86OverDynarmic','v86','dynarmic'], ['dynarmicOverWasm','dynarmic','wasm']]) {
          if (item.runtimes[numerator].belowResolution || item.runtimes[denominator].belowResolution) continue;
          item.ratios[name] = statistics(Array.from({length: rounds}, (_, i) => byRuntime[numerator].get(i) / byRuntime[denominator].get(i)));
        }
      }
      cases[caseId] = item;
    }
    return {cases, complete: caseIds.every(id => cases[id].complete), resolutionFloorMs: RESOLUTION_MS};
  }
  const api = {CASE_IDS, RUNTIMES, RESOLUTION_MS, statistics, validateReference, validateManifest, validateResult, summarize};
  if (typeof module === 'object' && module.exports) module.exports = api;
  else global.CBenchProtocol = api;
})(globalThis);
