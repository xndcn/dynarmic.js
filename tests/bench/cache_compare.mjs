#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {cacheCases, validateCacheReport} from './cache_protocol.mjs';

const ratio = (baseline, candidate) => baseline === 0
  ? (candidate === 0 ? 1 : null) : candidate / baseline;

export function compareCacheReports(baseline, candidate) {
  validateCacheReport(baseline);
  validateCacheReport(candidate);
  for (const key of ['node', 'v8', 'runtime', 'host', 'build', 'methodology']) {
    assert.deepEqual(candidate[key], baseline[key], `incompatible ${key}`);
  }
  assert.equal(candidate.git?.upstream, baseline.git?.upstream, 'upstream revision changed');
  for (const key of ['platform', 'release', 'arch', 'cpu', 'cpus']) {
    assert(candidate.host?.[key] !== undefined, `missing host.${key}`);
  }
  assert(/^[a-f0-9]{40}$/.test(baseline.git?.revision), 'invalid baseline revision');
  assert(/^[a-f0-9]{40}$/.test(candidate.git?.revision), 'invalid candidate revision');
  const results = cacheCases.map(spec => {
    const before = baseline.cases.find(item => item.name === spec.name);
    const after = candidate.cases.find(item => item.name === spec.name);
    assert(before && after, `${spec.name}: missing case`);
    assert(before.runs >= 5 && after.runs >= 5, 'comparison needs at least five fresh processes');
    assert.equal(after.workloadSha256, before.workloadSha256, `${spec.name}: workload changed`);
    const metrics = [['totalMedianMs', 1.15, 'max']];
    // Keep per-cycle latency visible: a single pathological invalidation
    // cycle must not disappear inside the total median.
    for (let cycle = 0; cycle < before.summary.cycleMedianMs.length; ++cycle) {
      metrics.push([`cycleMedianMs[${cycle}]`, 1.15, 'max']);
    }
    for (let cycle = 0; cycle < before.summary.compileBytesMedian.length; ++cycle) {
      metrics.push([`compileBytesMedian[${cycle}]`, 1.10, 'max']);
    }
    const values = metrics.map(([metric, limit, direction]) => {
      const read = item => metric.includes('[')
        ? item.summary[metric.slice(0, metric.indexOf('['))][Number(metric.match(/\[(\d+)\]/)[1])]
        : item.summary[metric];
      const old = read(before), value = read(after), r = ratio(old, value);
      const pass = r !== null && (direction === 'max' ? r <= limit + 1e-12 : r >= limit - 1e-12);
      return {metric, baseline: old, candidate: value, ratio: r, limit, direction, pass};
    });
    if (baseline.build.profile) {
      const old = before.summary.peakCachedBytesMedian;
      const value = after.summary.peakCachedBytesMedian;
      const r = ratio(old, value);
      values.push({metric: 'peakCachedBytesMedian', baseline: old, candidate: value,
        ratio: r, limit: 1.10, direction: 'max', pass: r !== null && r <= 1.10 + 1e-12});
    }
    return {name: spec.name, pass: values.every(metric => metric.pass), metrics: values};
  });
  return {schema: 'dynarmic.js/cache-comparison-v1', pass: results.every(item => item.pass), cases: results};
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const args = process.argv.slice(2);
    assert.equal(args.length, 2, 'usage: cache_compare.mjs <baseline.json> <candidate.json>');
    const result = compareCacheReports(...args.map(file => JSON.parse(fs.readFileSync(file, 'utf8'))));
    console.log(JSON.stringify(result, null, 2));
    process.exitCode = result.pass ? 0 : 1;
  } catch (error) {
    console.error(`Cannot compare cache reports: ${error.message}`);
    process.exitCode = 2;
  }
}
