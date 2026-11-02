#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {populationCases, validatePopulationReport} from './cache_population_protocol.mjs';

export function comparePopulationReports(baseline, candidate) {
  validatePopulationReport(baseline);
  validatePopulationReport(candidate);
  for (const key of ['node', 'v8', 'runtime', 'host', 'build', 'methodology']) {
    assert.deepEqual(candidate[key], baseline[key], `incompatible ${key}`);
  }
  assert.equal(candidate.git.upstream, baseline.git.upstream, 'upstream revision changed');
  const results = populationCases.map(spec => {
    const before = baseline.cases.find(item => item.name === spec.name);
    const after = candidate.cases.find(item => item.name === spec.name);
    assert(before.runs >= 5 && after.runs >= 5, 'comparison needs at least five fresh processes');
    assert.equal(after.workloadSha256, before.workloadSha256, `${spec.name}: workload changed`);
    const limits = {coldMedianMs: 1.15, warmMedianMs: 1.15, compileMedianMs: 1.15, compileBytesMedian: 1.10};
    if (baseline.build.profile) limits.peakCachedBytesMedian = 1.10;
    const metrics = Object.entries(limits).map(([metric, limit]) => {
      const old = before.summary[metric], value = after.summary[metric];
      const ratio = old === 0 ? (value === 0 ? 1 : null) : value / old;
      return {metric, baseline: old, candidate: value, ratio, limit,
        pass: ratio !== null && ratio <= limit + 1e-12};
    });
    return {name: spec.name, pass: metrics.every(metric => metric.pass), metrics};
  });
  return {schema: 'dynarmic.js/cache-population-comparison-v1',
    pass: results.every(item => item.pass), cases: results};
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const args = process.argv.slice(2);
    assert.equal(args.length, 2, 'usage: cache_population_compare.mjs <baseline.json> <candidate.json>');
    const result = comparePopulationReports(...args.map(file => JSON.parse(fs.readFileSync(file, 'utf8'))));
    console.log(JSON.stringify(result, null, 2));
    process.exitCode = result.pass ? 0 : 1;
  } catch (error) {
    console.error(`Cannot compare cache population reports: ${error.message}`);
    process.exitCode = 2;
  }
}
