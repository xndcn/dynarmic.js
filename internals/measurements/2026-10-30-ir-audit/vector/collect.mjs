// Collect every process in an alternating baseline/candidate cohort. Execution
// is sequential and frozen modules are never rebuilt during measurement.
// Usage: node collect.mjs <baseline-dir> <candidate-dir> <output-dir>
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const [, , baseline, candidate, output] = process.argv;
mkdirSync(output, { recursive: true });
const runner = join(dirname(fileURLToPath(import.meta.url)), 'bench.mjs');
const all = [];
for (let pair = 0; pair < 5; ++pair) {
    const samples = {};
    const order = pair % 2 ? ['candidate', 'baseline'] : ['baseline', 'candidate'];
    for (const variant of order) {
        const child = spawnSync(process.execPath, [runner, resolve(variant === 'baseline' ? baseline : candidate)], {
            encoding: 'utf8',
            env: { PATH: process.env.PATH },
        });
        const stem = join(output, `${pair}-${variant}`);
        writeFileSync(`${stem}.stdout`, child.stdout, { flag: 'wx' });
        writeFileSync(`${stem}.stderr`, child.stderr, { flag: 'wx' });
        writeFileSync(`${stem}.process.json`, JSON.stringify({ status: child.status, signal: child.signal, error: child.error?.message }) + '\n', { flag: 'wx' });
        assert.equal(child.status, 0, child.stderr || String(child.error));
        samples[variant] = JSON.parse(child.stdout);
        assert.equal(samples[variant].samples.length, 14);
    }
    const results = sample => sample.samples.map(({ filename, low, high, fpsr }) => ({ filename, low, high, fpsr }));
    assert.deepEqual(results(samples.baseline), results(samples.candidate));
    all.push({ pair, order, baseline: samples.baseline.totalMs, candidate: samples.candidate.totalMs });
}
const median = values => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
const baselineMedian = median(all.map(x => x.baseline));
const candidateMedian = median(all.map(x => x.candidate));
const report = { pairs: all, baselineMedianMs: baselineMedian, candidateMedianMs: candidateMedian, timeChangePercent: (candidateMedian / baselineMedian - 1) * 100 };
writeFileSync(join(output, 'summary.json'), JSON.stringify(report, null, 2) + '\n', { flag: 'wx' });
console.log(JSON.stringify(report));
