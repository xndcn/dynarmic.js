#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {validateReport} from './protocol.mjs';

export function compareReports(baseline, candidate) {
  validateReport(baseline); validateReport(candidate);
  for (const key of ['node', 'v8', 'runtime', 'host', 'build', 'methodology']) {
    assert.deepEqual(candidate[key], baseline[key], `incompatible ${key}`);
  }
  assert.equal(candidate.git.upstream, baseline.git.upstream, 'upstream revision changed');
  const results = [];
  for (const before of baseline.cases) {
    const after = candidate.cases.find(c => c.name === before.name);
    assert(before.runs >= 5 && after.runs >= 5, 'comparison needs at least five fresh processes');
    assert.equal(after.workloadSha256, before.workloadSha256, `${before.name}: workload changed`);
    assert.equal(after.samples[0].instructions, before.samples[0].instructions, `${before.name}: instruction count changed`);
    const metrics = [
      ['warmMinstrPerSec', 0.95, 'min'],
      ['emittedBytesMedian', 1.10, 'max'],
      ['coldCompileMedianMs', 1.15, 'max'],
    ].map(([metric, limit, direction]) => {
      const old = before.summary[metric], value = after.summary[metric];
      const ratio = old === 0 ? (value === 0 ? 1 : null) : value / old;
      const pass = ratio !== null && (direction === 'min' ? ratio >= limit - 1e-12 : ratio <= limit + 1e-12);
      return {metric, baseline: old, candidate: value, ratio, limit, direction, pass};
    });
    results.push({name: before.name, pass: metrics.every(m => m.pass), metrics});
  }
  return {schema: 'dynarmic.js/comparison-v1', pass: results.every(r => r.pass), cases: results};
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const args = process.argv.slice(2);
    assert.equal(args.length, 2, 'usage: compare.mjs <baseline.json> <candidate.json>');
    const result = compareReports(...args.map(file => JSON.parse(fs.readFileSync(file, 'utf8'))));
    console.log(JSON.stringify(result, null, 2));
    process.exitCode = result.pass ? 0 : 1;
  } catch (error) {
    console.error(`Cannot compare: ${error.message}`);
    process.exitCode = 2;
  }
}
