import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import {gunzipSync} from 'node:zlib';
import {analyzeRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {analyzeReports} from './analyze.mjs';

const readGzip = path => JSON.parse(gunzipSync(fs.readFileSync(new URL(path, import.meta.url))));
const baseline = () => readGzip('../2026-10-24-region-diagnostics/a64-flat.json.gz');
const current = () => readGzip('./a64-flat-v2.json.gz');

test('fresh v2 evidence exactly projects to v1 and classifies the registered cohort', () => {
  const result = analyzeReports(baseline(), current());
  assert.equal(result.schema, 'dynarmic.js/region-alias-evidence-v1');
  assert.equal(result.projectionMatchesV1, true);
  assert.deepEqual(result.coverage.totals, {
    regionCount: 111,
    laterEntryCount: 82,
    coveredLaterEntryCount: 16,
  });
  assert.equal(result.coverage.decision, 'GO');
});

test('stale projections and altered cohort membership are rejected', () => {
  const before = baseline(), after = current();
  before.regionDiagnostics.regions[0].entryPc = '0x8000000000001000';
  assert.throws(() => analyzeReports(before, after), /projection/);

  const otherBefore = baseline(), otherAfter = current();
  const region = otherAfter.regionDiagnostics.regions[1];
  region.entryDescriptor = '0x8000000000001000';
  region.blocks[region.entryIndex].descriptor = region.entryDescriptor;
  otherAfter.duplication = analyzeRegionDiagnostics(otherAfter.regionDiagnostics);
  assert.throws(() => analyzeReports(otherBefore, otherAfter), /projection/);
});

test('invalid masks, layouts and live generations fail closed', () => {
  for (const mutate of [
    value => { value.regionDiagnostics.regions[0].layout = 'invalid'; },
    value => {
      const region = value.regionDiagnostics.regions.find(row => row.layout === 'fallback');
      region.blocks[0].emitted = false;
    },
    value => { value.regionDiagnostics.regions[1].liveRegionIdsBeforeCompile = [2]; },
    value => { value.regionDiagnostics.regions[1].liveRegionIdsBeforeCompile = [1, 1]; },
  ]) {
    const after = current();
    mutate(after);
    assert.throws(() => analyzeReports(baseline(), after), /layout|emit|live/i);
  }
});

test('valid owner mutations produce different analysis rather than being hidden', () => {
  const after = current();
  const owner = after.regionDiagnostics.regions.find(region =>
    region.layout === 'fallback' && region.blocks.some(block => block.returnSite));
  const site = owner.blocks.find(block => block.returnSite);
  site.returnSite = false;
  const result = analyzeReports(baseline(), after);
  assert.notDeepEqual(result.coverage, analyzeReports(baseline(), current()).coverage);
});
