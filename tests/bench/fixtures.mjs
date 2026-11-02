// Synthetic reports for validating measurement failures, never performance data.
import {cases, methodology, profileCounters, summarize} from './protocol.mjs';
export function samples(profile = false, process = 0) {
  return Array.from({length: 9}, (_, i) => {
    const compile = {blocks: i ? 0 : 1, bytes: i ? 0 : 100,
      translateMs: i ? 0 : 0.1, optMs: 0, emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0};
    return {phase: i ? 'warm' : 'cold', runIndex: i, process, ms: 1, instructions: 1000, minstrPerSec: 1, compile,
      profile: profile ? {...Object.fromEntries(profileCounters.map(key => [key, 0])),
        schema: 'dynarmic.js/profile-v1', enabled: true, runCount: 1,
        compiledBlocks: compile.blocks, emittedBytes: compile.bytes,
        peakCachedBlocks: compile.blocks, peakCachedBytes: compile.bytes} : null};
  });
}
export function report(profile = false) {
  return {
    schema: 'dynarmic.js/benchmark-v2', timestamp: '2026-10-22T00:00:00.000Z', node: 'v24.test', v8: 'test',
    runtime: {nodeOptions: ''},
    host: {platform: 'linux', release: 'test', arch: 'x64', cpu: 'test CPU', cpus: 4},
    git: {revision: 'a'.repeat(40), upstream: 'b'.repeat(40), dirty: false},
    build: {type: 'Release', profile, a64RegionFocus: '', compilerId: 'Clang', compilerVersion: 'test', emscriptenVersion: 'test',
      cxxFlags: '', configCxxFlags: '-O3', linkFlags: '', configLinkFlags: ''},
    methodology: {...methodology},
    cases: cases.map(spec => {
      const observations = Array.from({length: 5}, (_, p) => samples(profile, p)).flat();
      return {name: spec.name, architecture: spec.architecture, memoryMode: spec.memoryMode, runs: 5,
        workloadSha256: 'c'.repeat(64), artifact: {jsSha256: 'd'.repeat(64), wasmSha256: 'e'.repeat(64)},
        samples: observations, summary: summarize(observations)};
    }),
  };
}
export function refresh(report) {
  for (const item of report.cases) item.summary = summarize(item.samples);
  return report;
}
