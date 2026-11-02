import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {validateEnvironment} from './environment.mjs';
import {median, compileTimers, profileCounters} from './protocol.mjs';

export const coremarkCases = ['a32', 'a64'].flatMap(architecture =>
  ['thunk', 'page-table', 'flat-window'].map((memoryMode, mode) => ({
    name: `${architecture}-${memoryMode}`, architecture, memoryMode, mode,
  })));
export const coremarkMethodology = {
  workload: 'CoreMark 2K performance seeds, one context, fixed compile-time iterations',
  process: 'one new module and JIT per sample; cases rotate between fresh-process rounds',
  timing: 'Run wall time excludes module load, RAM setup and JIT construction; includes cold compilation and publication',
  kernel: 'guest microsecond timer around CoreMark iterate; includes JIT misses and engine tiering; no steady-state claim',
  duration: 'durationQualified requires every guest sample to meet the upstream 10-second minimum; not certification',
  reference: 'native executable validates CRCs, including final CRC; native timing is not a performance comparison',
};
const shortError = 'ERROR! Must execute for at least 10 secs for a valid result!';
const correctLine = 'Correct operation validated. See README.md for run and reporting rules.';
const sha = value => assert(typeof value === 'string' && /^[a-f0-9]{64}$/.test(value), 'invalid SHA256');
const text = value => assert(typeof value === 'string' && value.length > 0, 'missing text');

export function parseCoremark(stdout, iterations) {
  assert(typeof stdout === 'string', 'missing CoreMark output');
  const lines = stdout.trim().split(/\r?\n/);
  const one = (regex, label) => {
    const matches = lines.map(line => regex.exec(line)).filter(Boolean);
    assert.equal(matches.length, 1, `missing/duplicate ${label}`);
    return matches[0][1];
  };
  assert.equal(lines.filter(l => l === '2K performance run parameters for coremark.').length, 1);
  assert.equal(Number(one(/^CoreMark Size\s*:\s*(\d+)$/, 'size')), 666);
  const ticks = Number(one(/^Total ticks\s*:\s*(\d+)$/, 'ticks'));
  const seconds = Number(one(/^Total time \(secs\)\s*:\s*([\d.]+)$/, 'seconds'));
  const iterationsPerSecond = Number(one(/^Iterations\/Sec\s*:\s*([\d.]+)$/, 'throughput'));
  assert.equal(Number(one(/^Iterations\s*:\s*(\d+)$/, 'iterations')), iterations);
  assert(Number.isInteger(ticks) && ticks > 0 && ticks <= 0xFFFFFFFF, 'invalid timer ticks');
  assert(Number.isFinite(seconds) && seconds > 0 && Math.abs(seconds - ticks / 1e6) <= 1.1e-6, 'inconsistent seconds');
  const rate = iterations / (ticks / 1e6);
  assert(Number.isFinite(iterationsPerSecond) && iterationsPerSecond > 0 &&
    Math.abs(iterationsPerSecond - rate) <= Math.max(1e-5, rate * 1e-8), 'inconsistent throughput');
  const crc = {};
  for (const key of ['seedcrc', 'crclist', 'crcmatrix', 'crcstate', 'crcfinal']) {
    crc[key] = one(new RegExp(`^${key === 'seedcrc' ? '' : '\\[0\\]'}${key}\\s*:\\s*0x([a-fA-F0-9]{4})$`), key).toLowerCase();
  }
  for (const [key, value] of Object.entries({seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a'})) {
    assert.equal(crc[key], value, `wrong ${key}`);
  }
  const durationValid = ticks >= 10_000_000;
  const errors = lines.filter(l => /ERROR!|Errors detected|Cannot validate/.test(l));
  assert.deepEqual(errors, durationValid ? [] : [shortError, 'Errors detected'], 'unexpected CoreMark error');
  assert.equal(lines.filter(l => l === correctLine).length, durationValid ? 1 : 0, 'inconsistent validation marker');
  assert.equal(one(/^Memory location\s*:\s*(.+)$/, 'memory location'), 'STACK');
  const compilerVersion = one(/^Compiler version\s*:\s*(.+)$/, 'compiler version');
  const compilerFlags = one(/^Compiler flags\s*:\s*(.+)$/, 'compiler flags');
  if (durationValid) {
    assert.equal(one(/^CoreMark 1\.0 : (.+)$/, 'score summary'),
      `${iterationsPerSecond.toFixed(6)} / ${compilerVersion} ${compilerFlags} / STACK`, 'inconsistent score summary');
  }
  // This pinned single-context port prints exactly these fields and two status
  // lines. Reject extra diagnostics (including unfamiliar error spellings).
  assert.equal(lines.length, 16, 'unexpected CoreMark output lines');
  return {ticks, seconds, iterationsPerSecond, crc, durationValid, compilerVersion, compilerFlags};
}

export function validateCoremarkBuild(workload) {
  assert.equal(workload.schema, 'dynarmic.js/coremark-build-v1');
  assert(typeof workload.revision === 'string' && /^[a-f0-9]{40}$/.test(workload.revision));
  assert(Number.isInteger(workload.iterations) && workload.iterations >= 1 && workload.iterations <= 2147483647);
  assert.equal(workload.dataSize, 2000); assert.equal(workload.contexts, 1);
  const sources = ['core_list_join.c', 'core_main.c', 'core_matrix.c', 'core_state.c', 'core_util.c',
    'barebones/cvt.c', 'barebones/ee_printf.c', 'coremark.h'].map(n => `upstream/${n}`);
  sources.push('port/core_portme.c', 'port/core_portme.h', 'port/build.py', 'a32/start.S', 'a32/link.ld',
    'a64/start.S', 'a64/link.ld', 'a64/include/string.h', 'a64/include/math.h');
  assert.deepEqual(Object.keys(workload.sources).sort(), sources.sort(), 'missing workload source');
  Object.values(workload.sources).forEach(sha); sha(workload.uartSourceSha256);
  for (const arch of ['a32', 'a64', 'native']) {
    text(workload.compilers?.[arch]); text(workload.flags?.[arch]);
    assert(Array.isArray(workload.commands?.[arch]) && workload.commands[arch].length > 0);
    workload.commands[arch].forEach(text);
    assert.equal(workload.artifacts?.[arch]?.file, arch === 'native' ? 'coremark_host' : `coremark_${arch}.bin`);
    sha(workload.artifacts[arch].sha256);
  }
}

export function coremarkIdentity(workload) {
  validateCoremarkBuild(workload);
  // Paths in compiler commands are provenance, not workload identity. The
  // builder itself is hashed with the fixed flags, definitions and inputs.
  const {commands, ...identity} = workload;
  const canonical = value => Array.isArray(value) ? value.map(canonical)
    : value && typeof value === 'object' ? Object.fromEntries(Object.keys(value).sort().map(k => [k, canonical(value[k])])) : value;
  return createHash('sha256').update(JSON.stringify(canonical(identity))).digest('hex');
}

export function summarizeCoremark(samples, profile) {
  const runMedianMs = median(samples.map(s => s.runMs));
  const kernelMedianMs = median(samples.map(s => s.metrics.ticks / 1000));
  return {runMedianMs, kernelMedianMs,
    runMadMs: median(samples.map(s => Math.abs(s.runMs - runMedianMs))),
    kernelMadMs: median(samples.map(s => Math.abs(s.metrics.ticks / 1000 - kernelMedianMs))),
    compileMedianMs: median(samples.map(s => compileTimers.reduce((sum, k) => sum + s.compile[k], 0))),
    emittedBytesMedian: median(samples.map(s => s.compile.bytes)),
    peakCachedBytesMedian: profile ? median(samples.map(s => s.profile.peakCachedBytes)) : null,
    durationQualified: samples.every(s => s.metrics.durationValid)};
}

export function validateCoremarkReport(report, requireDuration = false) {
  assert.equal(report.schema, 'dynarmic.js/coremark-workload-v1');
  validateEnvironment(report); text(report.runtime.cpuAffinity);
  validateCoremarkBuild(report.workload);
  assert.equal(report.workloadSha256, coremarkIdentity(report.workload), 'workload identity mismatch');
  assert.deepEqual(report.methodology, coremarkMethodology);
  sha(report.artifact?.jsSha256); sha(report.artifact?.wasmSha256);
  const native = parseCoremark(report.native.stdout, report.workload.iterations);
  assert.deepEqual(report.native.metrics, native);
  assert.equal(native.compilerFlags, report.workload.flags.native);
  assert.equal(report.cases.length, coremarkCases.length);
  for (const spec of coremarkCases) {
    const item = report.cases.find(c => c.name === spec.name);
    assert(item, `missing ${spec.name}`);
    assert.equal(item.architecture, spec.architecture); assert.equal(item.memoryMode, spec.memoryMode);
    assert(Number.isInteger(item.runs) && item.runs >= 1 && item.runs <= 100);
    assert.equal(item.samples.length, item.runs);
    assert.deepEqual(item.samples.map(s => s.process), Array.from({length: item.runs}, (_, i) => i));
    for (const sample of item.samples) {
      assert.equal(sample.haltReason, 0x01000000, 'guest did not exit normally');
      const parsed = parseCoremark(sample.stdout, report.workload.iterations);
      assert.deepEqual(sample.metrics, parsed, 'incorrect parsed output');
      assert.deepEqual(parsed.crc, native.crc, 'native/guest CRC mismatch');
      assert.equal(parsed.compilerFlags, report.workload.flags[spec.architecture]);
      if (requireDuration) assert(parsed.durationValid, 'guest run below 10 seconds');
      assert(Number.isFinite(sample.runMs) && sample.runMs > 0 && sample.runMs < 4294967.296,
        'invalid Run time or timer-wrap ambiguity');
      assert(sample.runMs + 0.01 >= parsed.ticks / 1000, 'kernel time exceeds Run');
      for (const key of ['blocks', 'bytes', ...compileTimers]) {
        assert(Number.isFinite(sample.compile?.[key]) && sample.compile[key] >= 0, `invalid compile.${key}`);
      }
      for (const key of ['blocks', 'bytes']) assert(Number.isSafeInteger(sample.compile[key]) && sample.compile[key] > 0);
      assert(compileTimers.reduce((s, k) => s + sample.compile[k], 0) <= sample.runMs + 0.01, 'compile phases exceed Run');
      if (report.build.profile) {
        assert.equal(sample.profile?.schema, 'dynarmic.js/profile-v1'); assert.equal(sample.profile.enabled, true);
        for (const key of profileCounters) assert(Number.isSafeInteger(sample.profile[key]) && sample.profile[key] >= 0, `invalid profile.${key}`);
        assert.equal(sample.profile.runCount, 1); assert.equal(sample.profile.stepCount, 0);
        assert.equal(sample.profile.compiledBlocks, sample.compile.blocks); assert.equal(sample.profile.emittedBytes, sample.compile.bytes);
        assert.equal(sample.profile.invalidationCount, 0); assert.equal(sample.profile.invalidatedBlocks, 0);
        assert.equal(sample.profile.peakCachedBlocks, sample.compile.blocks); assert.equal(sample.profile.peakCachedBytes, sample.compile.bytes);
      } else assert.equal(sample.profile, null);
    }
    assert(item.samples.every(s => s.metrics.compilerVersion === item.samples[0].metrics.compilerVersion), 'guest compiler changed');
    assert.deepEqual(item.summary, summarizeCoremark(item.samples, report.build.profile));
  }
  assert.equal(report.durationQualified, report.cases.every(c => c.summary.durationQualified));
  return report;
}
