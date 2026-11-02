#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {validateCoremarkReport} from '../coremark_protocol.mjs';
export function compareCoremark(before, after, requireDuration = false) {
  validateCoremarkReport(before, requireDuration); validateCoremarkReport(after, requireDuration);
  for (const key of ['node', 'v8', 'runtime', 'host', 'build', 'methodology', 'workloadSha256']) {
    assert.deepEqual(after[key], before[key], `incompatible ${key}`);
  }
  assert.equal(after.git.upstream, before.git.upstream, 'upstream revision changed');
  assert.deepEqual(after.native.metrics.crc, before.native.metrics.crc);
  const cases = before.cases.map(a => {
    const b = after.cases.find(c => c.name === a.name);
    assert(a.runs >= 5 && b.runs >= 5, 'need five fresh processes');
    const limits = {kernelMedianMs: 1 / 0.95, runMedianMs: 1.15, compileMedianMs: 1.15, emittedBytesMedian: 1.10};
    if (before.build.profile) limits.peakCachedBytesMedian = 1.10;
    const metrics = Object.entries(limits).map(([metric, limit]) => {
      const baseline = a.summary[metric], candidate = b.summary[metric];
      const ratio = baseline === 0 ? (candidate === 0 ? 1 : null) : candidate / baseline;
      return {metric, baseline, candidate, ratio, limit, pass: ratio !== null && ratio <= limit + 1e-12};
    });
    return {name: a.name, metrics, pass: metrics.every(m => m.pass)};
  });
  return {schema: 'dynarmic.js/coremark-comparison-v1', durationQualified: before.durationQualified && after.durationQualified,
    pass: cases.every(c => c.pass), cases};
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const [before, after, option, ...extra] = process.argv.slice(2);
    assert(before && after && !extra.length && (!option || option === '--require-duration'),
      'usage: compare.mjs before.json after.json [--require-duration]');
    const result = compareCoremark(...[before, after].map(f => JSON.parse(fs.readFileSync(f, 'utf8'))), option === '--require-duration');
    console.log(JSON.stringify(result, null, 2)); process.exitCode = result.pass ? 0 : 1;
  } catch (error) { console.error(`Cannot compare CoreMark workloads: ${error.message}`); process.exitCode = 2; }
}
