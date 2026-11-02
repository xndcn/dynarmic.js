import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {ANALYSIS_SCOPE} from './analyze.mjs';
import {verifyEvidence} from './verify.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const evidence = path.join(here, 'evidence');
const realAnalysis = () => JSON.parse(fs.readFileSync(path.join(evidence, 'analysis.json'), 'utf8'));

test('real frozen evidence passes inventory, fresh analysis, semantics and byte pins', () => {
  assert.deepEqual(verifyEvidence(evidence), {
    schema: 'dynarmic.js/focused-region-transfer-analysis-v1', files: 4, scope: ANALYSIS_SCOPE,
  });
});

test('real semantic expectations reject a changed generation partition', async () => {
  const {verifyRealAnalysis} = await import('./verify.mjs');
  const analysis = realAnalysis();
  analysis.generations.added.pop();
  assert.throws(() => verifyRealAnalysis(analysis), /real generation partition/);
});

test('real semantic expectations reject changed artifact identity', async () => {
  const {verifyRealAnalysis} = await import('./verify.mjs');
  const analysis = realAnalysis();
  analysis.provenance.artifacts.candidate.wasmSha256 = '0'.repeat(64);
  assert.throws(() => verifyRealAnalysis(analysis), /real artifact identity/);
});

test('real semantic expectations reject a changed newly-external count', async () => {
  const {verifyRealAnalysis} = await import('./verify.mjs');
  const analysis = realAnalysis();
  analysis.newlyExternalizedDirectEdges[0].candidateCount = '0x0000000000000000';
  assert.throws(() => verifyRealAnalysis(analysis), /real newly externalized edges/);
});
