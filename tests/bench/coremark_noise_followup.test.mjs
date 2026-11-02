import assert from 'node:assert/strict';
import test from 'node:test';
import {fixture, refresh} from './coremark_fixtures.mjs';
import {evaluateNoiseFollowup} from './coremark/noise_followup.mjs';

const cohort = () => ({before: fixture(false, 10000000), after: fixture(false, 10000000)});

test('CoreMark noise follow-up pools every sample without mutating raw cohorts', () => {
  const original = cohort(), followup = cohort();
  original.after.cases[0].samples.forEach((sample, index) => { sample.compile.translateMs = 100 + index; });
  followup.after.cases[0].samples.forEach((sample, index) => { sample.compile.translateMs = 200 + index; });
  refresh(original.after); refresh(followup.after);
  const snapshot = structuredClone(original);
  const result = evaluateNoiseFollowup(original, followup);
  for (const side of ['before', 'after']) {
    const report = result.pooledReports[side];
    for (const item of report.cases) {
      assert.equal(item.runs, 10);
      assert.deepEqual(item.samples.map(sample => sample.process), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    }
  }
  const compile = result.pooledReports.after.cases[0].samples.map(sample => sample.compile.translateMs);
  assert.deepEqual(compile, [100, 101, 102, 103, 104, 200, 201, 202, 203, 204]);
  assert.equal(result.followupComparison.cases.length, 6);
  assert.equal(result.pooledComparison.cases.length, 6);
  assert.deepEqual(original, snapshot);
});

test('CoreMark noise follow-up rejects changed implementation identity', () => {
  const original = cohort(), followup = cohort();
  followup.after.artifact.wasmSha256 = '0'.repeat(64);
  assert.throws(() => evaluateNoiseFollowup(original, followup), /followup after artifact changed/);
});

test('CoreMark noise follow-up requires exactly five fresh processes per cohort', () => {
  const original = cohort(), followup = cohort();
  followup.before.cases[0].runs = 6;
  followup.before.cases[0].samples.push({...followup.before.cases[0].samples[4], process: 5});
  refresh(followup.before);
  assert.throws(() => evaluateNoiseFollowup(original, followup), /five-process cohorts/);
});
