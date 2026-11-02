#!/usr/bin/env node
// Collect one complete CoreMark Run as compile-history evidence, not a score.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {hashFile, readBuildInfo, sourceInfo, systemInfo} from '../environment.mjs';
import {analyzeRegionDiagnostics, projectRegionDiagnostics} from '../region_diagnostics.mjs';
import {analyzeRegionTransferDiagnostics, checkRunTransferConservation} from '../region_transfer_diagnostics.mjs';
import {profileCounters} from '../protocol.mjs';
import {coremarkIdentity, parseCoremark, validateCoremarkBuild} from '../coremark_protocol.mjs';
import {collectCoremarkSample} from './sample.mjs';

function validateTransferAttribution(transfers, compileDiagnostics, architecture) {
  const compileRegions = projectRegionDiagnostics(compileDiagnostics).regions;
  assert.equal(transfers.regions.length, compileRegions.length,
    'region transfer diagnostics: compile/transfer region counts differ');
  transfers.regions.forEach((transfer, index) => {
    const compile = compileRegions[index];
    assert.equal(transfer.regionId, compile.regionId, 'region transfer diagnostics: region ID differs');
    assert.equal(transfer.frontend, architecture, 'region transfer diagnostics: workload frontend differs');
    assert.equal(compile.frontend, architecture, 'region transfer diagnostics: compile frontend differs');
    assert.equal(transfer.callerEntryDescriptor, compile.entryDescriptor,
      'region transfer diagnostics: caller descriptor differs');
    assert.equal(transfer.callerEntryPc, compile.entryPc, 'region transfer diagnostics: caller PC differs');
    for (const exit of transfer.exits) {
      assert(compile.selectedDescriptors.includes(exit.exitBlockDescriptor),
        'region transfer diagnostics: exit block is absent from its compile generation');
    }
  });
}

function validateRunTransferConservation(transferAnalysis, profile) {
  if (transferAnalysis === undefined || profile === null || profile === undefined) return {};
  assert.equal(profile.runCount, 1, 'region transfer diagnostics: profile must contain one Run');
  assert.equal(profile.stepCount, 0, 'region transfer diagnostics: profile contains an unexpected Step');
  for (const key of profileCounters) {
    assert(Number.isSafeInteger(profile[key]) && profile[key] >= 0,
      `region transfer diagnostics: invalid profile.${key}`);
  }
  const transferConservation = checkRunTransferConservation(transferAnalysis, profile.dispatchCount);
  assert.equal(transferConservation.conserved, true, 'region transfer diagnostics: Run conservation mismatch');
  return {transferConservation};
}

export function createCoremarkRegionDiagnosticReport({architecture, memoryMode, workloadSha256, artifact, native, sample}) {
  assert(['a32', 'a64'].includes(architecture), 'invalid architecture');
  assert(['0', '1', '2'].includes(memoryMode), 'invalid memory mode');
  assert.equal(sample.haltReason, 0x01000000, 'guest halt was not normal');
  assert(sample.regionDiagnostics, 'missing region diagnostics');
  assert.deepEqual(sample.metrics?.crc, native.metrics?.crc, 'native/guest CRC mismatch');
  const duplication = analyzeRegionDiagnostics(sample.regionDiagnostics);
  assert.equal(duplication.totals.regionCount, sample.compile.blocks, 'region diagnostics: compile block totals differ');
  assert.equal(duplication.totals.moduleBytes, sample.compile.bytes, 'region diagnostics: compile byte totals differ');
  if (sample.profile) {
    assert.equal(duplication.totals.regionCount, sample.profile.compiledBlocks, 'region diagnostics: profile block totals differ');
    assert.equal(duplication.totals.moduleBytes, sample.profile.emittedBytes, 'region diagnostics: profile byte totals differ');
  }
  const {regionDiagnostics, regionTransferDiagnostics, ...ordinarySample} = sample;
  const transferAnalysis = regionTransferDiagnostics === undefined
    ? undefined : analyzeRegionTransferDiagnostics(regionTransferDiagnostics);
  if (transferAnalysis !== undefined) {
    validateTransferAttribution(regionTransferDiagnostics, regionDiagnostics, architecture);
  }
  const transferFields = transferAnalysis === undefined ? {} : {
    regionTransferDiagnostics,
    transferAnalysis,
  };
  const conservationFields = validateRunTransferConservation(transferAnalysis, sample.profile);
  return {
    schema: 'dynarmic.js/coremark-region-diagnostics-v1',
    architecture,
    memoryMode,
    workloadSha256,
    artifact,
    native,
    sample: ordinarySample,
    regionDiagnostics,
    duplication,
    ...transferFields,
    ...conservationFields,
  };
}

async function collect() {
  const [browserArg, workloadArg, architecture, memoryMode, output, ...extra] = process.argv.slice(2);
  assert(browserArg && workloadArg && output && !extra.length,
    'usage: region_diagnostics.mjs <browser-build-dir> <workload-dir> <a32|a64> <0|1|2> <new-output.json>');
  const browserDir = path.resolve(browserArg), workloadDir = path.resolve(workloadArg);
  assert(!fs.existsSync(output), `refusing to overwrite ${output}`);
  const workload = JSON.parse(fs.readFileSync(path.join(workloadDir, 'manifest.json'), 'utf8'));
  validateCoremarkBuild(workload);
  const jit = path.join(browserDir, 'dist/arm_jit.js');
  const artifact = {jsSha256: hashFile(jit), wasmSha256: hashFile(path.join(browserDir, 'dist/arm_jit.wasm'))};
  const nativeResult = spawnSync(path.join(workloadDir, workload.artifacts.native.file), [],
    {encoding: 'utf8', timeout: 300000, maxBuffer: 1024 * 1024});
  assert.equal(nativeResult.status, 0, String(nativeResult.error || nativeResult.stderr || nativeResult.signal));
  const native = {stdout: nativeResult.stdout, metrics: parseCoremark(nativeResult.stdout, workload.iterations)};
  const sample = await collectCoremarkSample(jit, path.join(workloadDir, workload.artifacts[architecture].file),
    architecture, memoryMode);
  sample.metrics = parseCoremark(sample.stdout, workload.iterations);
  const {sourceRoot, build} = readBuildInfo(path.resolve(browserDir, '../..'));
  const report = createCoremarkRegionDiagnosticReport({
    architecture, memoryMode, workloadSha256: coremarkIdentity(workload), artifact, native, sample,
  });
  Object.assign(report, {timestamp: new Date().toISOString(), ...systemInfo(),
    git: sourceInfo(sourceRoot), build, workload});
  fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
  console.log(`CoreMark region diagnostics PASS regions=${report.duplication.totals.regionCount} ` +
    `duplicateSelections=${report.duplication.totals.duplicateSelectionCount}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    await collect();
  } catch (error) {
    console.error(`Cannot collect CoreMark region diagnostics: ${error.message}`);
    process.exitCode = 2;
  }
}
