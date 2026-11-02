import assert from 'node:assert/strict';
import {cases, median, parseHostOutput} from './protocol.mjs';
import {validateEnvironment} from './environment.mjs';

export const nativeMethodology = {
  platforms: 'Linux x86_64 and arm64 native hosts',
  scope: 'same fixed guest kernels and host sources, Wasm backend versus unmodified upstream native backend',
  cold: 'first Run of a new JIT in a fresh process; includes translation and guest execution, excludes process/module/JIT construction',
  warm: 'eight subsequent Runs of the same JIT; all retained, including Wasm engine tiering',
  timing: 'Run wall time only; reset, result checks and printing excluded; native steady_clock and Wasm performance clock',
  order: 'rotate cases each round; alternate native/wasm process order by round plus case index',
  aggregation: 'median of process warm medians; median and MAD of paired native/wasm throughput ratios',
  ratio: 'nativeOverWasmThroughput = wasm Run milliseconds / native Run milliseconds; greater than one favors native',
  memory: 'same callbacks and page tables; flat-window uses Wasm checked flat window and native fastmem with the same in-range addresses',
  limitations: 'microkernels, not applications; warm Runs include engine tiering; no native compilation counters, RSS or browser measurement',
};

export const pairOrder = (round, index) => (round + index) % 2 === 0 ? ['native', 'wasm'] : ['wasm', 'native'];

export function parseNativeOutput(stdout) {
  assert.equal(typeof stdout, 'string');
  assert(!/WRONG|FAIL/.test(stdout), 'native guest correctness failure');
  assert(stdout.split('\n').includes('PASS'), 'missing native guest PASS marker');
  const samples = stdout.split('\n').filter(line => line.startsWith('BENCHMARK '))
    .map(line => JSON.parse(line.slice(10)));
  assert.equal(samples.length, 9, 'missing native samples');
  samples.forEach((s, i) => {
    assert.equal(s.runIndex, i, 'invalid native run order');
    assert.equal(s.phase, i === 0 ? 'cold' : 'warm', 'invalid native phase');
    assert(Number.isFinite(s.ms) && s.ms > 0, 'invalid native timing');
    assert(Number.isSafeInteger(s.instructions) && s.instructions > 0, 'invalid native instruction count');
    assert.equal(s.instructions, samples[0].instructions, 'native instruction count changed');
    assert(Number.isFinite(s.minstrPerSec) && s.minstrPerSec > 0, 'invalid native throughput');
    assert(Math.abs(s.minstrPerSec - s.instructions / s.ms / 1000) < 1e-6, 'inconsistent native throughput');
    assert.equal(s.compile, null, 'native compilation counters are unavailable');
    assert.equal(s.profile, null, 'native profiling is unavailable');
  });
  return samples;
}

const mad = values => { const m = median(values); return median(values.map(v => Math.abs(v - m))); };
const warmMedian = observation => median(observation.samples.slice(1).map(s => s.ms));
export function summarizeNativePairs(pairs) {
  const summary = {};
  for (const side of ['native', 'wasm']) {
    const cold = pairs.map(pair => pair[side].samples[0].ms);
    const warm = pairs.map(pair => warmMedian(pair[side]));
    summary[side] = {
      coldMedianMs: median(cold), coldProcessMadMs: mad(cold),
      warmMedianMs: median(warm), warmProcessMadMs: mad(warm),
      warmMinstrPerSec: pairs[0][side].samples[0].instructions / median(warm) / 1000,
      warmIndexMedianMs: Array.from({length: 8}, (_, i) => median(pairs.map(pair => pair[side].samples[i + 1].ms))),
    };
  }
  const warmRatios = pairs.map(pair => warmMedian(pair.wasm) / warmMedian(pair.native));
  const coldRatios = pairs.map(pair => pair.wasm.samples[0].ms / pair.native.samples[0].ms);
  summary.nativeOverWasmThroughput = {warmMedian: median(warmRatios), warmMad: mad(warmRatios), warmPairs: warmRatios,
    coldMedian: median(coldRatios), coldMad: mad(coldRatios), coldPairs: coldRatios};
  return summary;
}

const sha = value => assert(typeof value === 'string' && /^[a-f0-9]{64}$/.test(value), 'invalid SHA256');
export function validateNativeReport(report, requireComparison = false) {
  assert.equal(report.schema, 'dynarmic.js/native-comparison-v1');
  validateEnvironment(report);
  assert(typeof report.runtime.cpuAffinity === 'string' && report.runtime.cpuAffinity.length > 0, 'missing CPU affinity');
  for (const key of ['revision', 'upstream']) assert.equal(typeof report.git[key], 'string', `invalid git.${key}`);
  assert.equal(report.build.type, 'Release', 'Wasm benchmark needs Release');
  assert.equal(report.build.profile, false, 'Wasm benchmark needs production profile mode');
  assert.equal(report.build.a64RegionFocus, '', 'native comparison requires default region configuration');
  assert.equal(report.build.regionDiagnostics, false, 'native comparison forbids region diagnostics');
  assert.equal(report.build.testExceptions, false, 'native comparison forbids test exception overhead');
  assert.equal(report.host.platform, 'linux');
  assert(['x64', 'arm64'].includes(report.host.arch));
  assert.deepEqual(report.methodology, nativeMethodology, 'unknown native comparison methodology');
  const native = report.nativeBuild;
  assert.equal(native?.backend, 'upstream-native');
  assert.equal(native.type, 'Release', 'native benchmark needs Release');
  assert.equal(native.architecture, report.host.arch === 'x64' ? 'x86_64' : 'arm64', 'native backend must match host');
  assert.equal(native.upstream, report.git.upstream, 'native and Wasm upstream differ');
  assert.equal(native.upstreamDirty, false, 'native upstream must be pristine');
  assert(typeof native.revision === 'string' && /^[a-f0-9]{40}$/.test(native.revision), 'invalid native revision');
  assert.equal(typeof native.dirty, 'boolean');
  for (const key of ['compilerId', 'compilerVersion', 'systemProcessor']) {
    assert(typeof native[key] === 'string' && native[key], `missing native ${key}`);
  }
  for (const key of ['cxxFlags', 'configCxxFlags', 'linkFlags', 'configLinkFlags']) assert.equal(typeof native[key], 'string');
  assert.equal(native.options?.DYNARMIC_TESTS, 'OFF');
  assert.equal(native.options?.DYNARMIC_USE_BUNDLED_EXTERNALS, 'ON');
  assert.equal(native.options?.DYNARMIC_FRONTENDS, 'A32;A64');
  assert.equal(native.options?.DYNARMIC_IGNORE_ASSERTS, 'OFF');
  sha(report.collectorSha256);
  assert(Number.isInteger(report.runs) && report.runs >= 1 && report.runs <= 100, 'invalid pair count');
  assert.equal(report.qualification, report.runs >= 5 ? 'comparison' : 'smoke-only');
  if (requireComparison) assert(report.runs >= 5, 'comparison needs at least five fresh process pairs');
  assert.equal(report.cases?.length, cases.length, 'incomplete native comparison suite');
  assert.equal(new Set(report.cases.map(item => item.name)).size, cases.length, 'duplicate native comparison case');
  for (const [index, spec] of cases.entries()) {
    const item = report.cases.find(value => value.name === spec.name);
    assert(item, `missing case ${spec.name}`);
    assert.equal(item.architecture, spec.architecture); assert.equal(item.memoryMode, spec.memoryMode);
    sha(item.workloadSha256);
    for (const key of ['nativeSha256', 'jsSha256', 'wasmSha256']) sha(item.artifacts?.[key]);
    assert.equal(item.pairs?.length, report.runs, 'incomplete process pairs');
    item.pairs.forEach((pair, round) => {
      assert.equal(pair.round, round, 'invalid pair index');
      assert.deepEqual(pair.order, pairOrder(round, index), 'invalid pair order');
      for (const side of ['native', 'wasm']) {
        const observation = pair[side];
        assert.equal(observation?.exitCode, 0, 'failed process');
        assert.equal(observation.signal, null, 'signalled process');
        assert.equal(typeof observation.stdout, 'string'); assert.equal(typeof observation.stderr, 'string');
        assert(Number.isFinite(observation.wallMs) && observation.wallMs > 0, 'invalid process wall time');
        const parsed = side === 'native' ? parseNativeOutput(observation.stdout) : parseHostOutput(observation.stdout);
        assert.deepEqual(observation.samples, parsed, 'samples differ from raw stdout');
        assert(observation.samples.every(s => s.profile === null), 'profile samples not comparable to native');
        assert(observation.samples.every(s => s.instructions === item.pairs[0].native.samples[0].instructions),
          'native/Wasm workload instruction count mismatch');
      }
    });
    assert.deepEqual(item.summary, summarizeNativePairs(item.pairs), 'incorrect native comparison summary');
  }
  return report;
}
