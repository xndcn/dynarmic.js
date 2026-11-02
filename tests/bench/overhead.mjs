#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {validateReport} from './protocol.mjs';

const median = values => {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};

function sameExceptProfile(production, profile) {
  for (const key of ['node', 'v8', 'runtime', 'host', 'methodology']) {
    assert.deepEqual(profile[key], production[key], `incompatible ${key}`);
  }
  assert.equal(production.build.profile, false, 'first report must be production');
  assert.equal(profile.build.profile, true, 'second report must be profile-enabled');
  assert.equal(profile.git.upstream, production.git.upstream, 'upstream revision changed');
  assert.equal(profile.git.revision, production.git.revision, 'implementation revision changed');
  for (const key of ['type', 'compilerId', 'compilerVersion', 'emscriptenVersion',
    'cxxFlags', 'configCxxFlags', 'linkFlags', 'configLinkFlags', 'a64RegionFocus', 'regionDiagnostics', 'testExceptions']) {
    assert.equal(profile.build[key], production.build[key], `incompatible build.${key}`);
  }
}

const warmMedians = item => [...new Set(item.samples.map(s => s.process))].map(process => median(
  item.samples.filter(s => s.process === process && s.phase === 'warm').map(s => s.ms)));

export function compareOverhead(production, profile) {
  validateReport(production);
  validateReport(profile, true);
  sameExceptProfile(production, profile);
  const cases = production.cases.map(before => {
    const after = profile.cases.find(item => item.name === before.name);
    assert(after, `missing profile case ${before.name}`);
    assert.equal(after.workloadSha256, before.workloadSha256, `${before.name}: workload changed`);
    assert.equal(after.samples[0].instructions, before.samples[0].instructions, `${before.name}: instructions changed`);
    assert(before.runs >= 5 && after.runs >= 5, 'overhead needs at least five processes');
    const warmProduction = median(warmMedians(before));
    const warmProfile = median(warmMedians(after));
    const coldProduction = before.summary.coldCompileMedianMs;
    const coldProfile = after.summary.coldCompileMedianMs;
    const bytesProduction = before.summary.emittedBytesMedian;
    const bytesProfile = after.summary.emittedBytesMedian;
    return {
      name: before.name,
      warmMsRatio: warmProfile / warmProduction,
      warmThroughputRatio: warmProduction / warmProfile,
      coldCompileRatio: coldProduction === 0 ? (coldProfile === 0 ? 1 : null) : coldProfile / coldProduction,
      emittedBytesRatio: bytesProfile / bytesProduction,
      production: {warmMedianMs: warmProduction, coldCompileMedianMs: coldProduction,
        emittedBytesMedian: bytesProduction, peakCachedBlocksMedian: before.summary.peakCachedBlocksMedian,
        peakCachedBytesMedian: before.summary.peakCachedBytesMedian},
      profile: {warmMedianMs: warmProfile, coldCompileMedianMs: coldProfile,
        emittedBytesMedian: bytesProfile, peakCachedBlocksMedian: after.summary.peakCachedBlocksMedian,
        peakCachedBytesMedian: after.summary.peakCachedBytesMedian},
    };
  });
  return {schema: 'dynarmic.js/profile-overhead-v1', cases};
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const [productionPath, profilePath] = process.argv.slice(2);
    assert(productionPath && profilePath, 'usage: overhead.mjs <production.json> <profile.json>');
    console.log(JSON.stringify(compareOverhead(
      JSON.parse(fs.readFileSync(productionPath, 'utf8')),
      JSON.parse(fs.readFileSync(profilePath, 'utf8')),
    ), null, 2));
  } catch (error) {
    console.error(`Cannot measure profile overhead: ${error.message}`);
    process.exitCode = 2;
  }
}
