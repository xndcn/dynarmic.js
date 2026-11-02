#!/usr/bin/env node
// Experiment-only offline comparison for the push-RSB ablation evidence.
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
import {isDeepStrictEqual} from 'node:util';
import {analyzeRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {parseCoremark} from '../../../tests/bench/coremark_protocol.mjs';

const REPORT_SCHEMA = 'dynarmic.js/coremark-region-diagnostics-v1';
const COMPARISON_SCHEMA = 'dynarmic.js/push-rsb-ablation-comparison-v1';
const WORKLOAD_SHA = 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa';
const A64_SHA = '8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1';
const REVISION = '9731e9a779755edb3e61ca17ed8ea3798119f3ec';
const UPSTREAM = 'a41c380246d3d9f9874f0f792d234dc0cc17c180';
const BASELINE_TOTALS = Object.freeze({
  regionCount: 111, selectedBlockCount: 4177, uniqueDescriptorCount: 862,
  duplicatedDescriptorCount: 587, duplicateSelectionCount: 3315,
  moduleBytes: 708917, entryBodyBytes: 694277,
});
const TARGETS = Object.freeze([
  '0x0000000000010cb8', '0x0000000000010e50', '0x0000000000010edc', '0x0000000000010f10',
  '0x0000000000010f4c', '0x0000000000010f8c', '0x0000000000010f9c', '0x0000000000010fa4',
  '0x0000000000010fb0', '0x0000000000010fb8', '0x0000000000010fd8', '0x0000000000010fe8',
  '0x0000000000010ff0', '0x0000000000011008', '0x0000000000011024', '0x0000000000011038',
  '0x000000000001104c', '0x0000000000011060', '0x0000000000011070', '0x00000000000110ac',
  '0x00000000000110f8', '0x0000000000011144', '0x0000000000011188', '0x00000000000111d0',
  '0x00000000000111ec', '0x000000000001120c', '0x0000000000011220', '0x000000000001122c',
]);
const HOT_ENTRIES = Object.freeze([
  '0x0000000000010b00', '0x0000000000010b18', '0x0000000000011700',
  '0x0000000000011fd0', '0x000000000001202c', '0x000000000001209c',
]);
const CRC_KEYS = ['seedcrc', 'crclist', 'crcmatrix', 'crcstate', 'crcfinal'];
const TOTAL_KEYS = Object.keys(BASELINE_TOTALS);

function requireCondition(condition, message) {
  if (!condition) throw new Error(`comparison: ${message}`);
}

function object(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function validateCrc(crc, role) {
  requireCondition(object(crc), `${role} CRC must be an object`);
  for (const key of CRC_KEYS) {
    requireCondition(typeof crc[key] === 'string' && /^[0-9a-f]{4}$/.test(crc[key]),
      `${role} CRC ${key} must be a lowercase 16-bit value`);
  }
}

function validateReport(report, role) {
  requireCondition(object(report) && report.schema === REPORT_SCHEMA, `${role} report schema mismatch`);
  requireCondition(report.architecture === 'a64', `${role} architecture must be a64`);
  requireCondition(report.memoryMode === '2', `${role} memoryMode must be 2`);
  requireCondition(report.workloadSha256 === WORKLOAD_SHA, `${role} workload SHA mismatch`);
  requireCondition(report.workload?.iterations === 200000, `${role} workload iterations must be 200000`);
  requireCondition(report.workload?.artifacts?.a64?.sha256 === A64_SHA, `${role} A64 binary SHA mismatch`);
  requireCondition(report.git?.revision === REVISION, `${role} base revision mismatch`);
  requireCondition(report.git?.upstream === UPSTREAM, `${role} upstream revision mismatch`);
  requireCondition(report.git.dirty === (role === 'candidate'),
    role === 'baseline' ? 'baseline must have a clean source tree' : 'candidate must have a dirty source tree');
  requireCondition(report.build?.profile === false, `${role} build profile must be OFF`);
  requireCondition(object(report.sample) && report.sample.profile === null,
    `${role} sample must not contain profile counters`);
  requireCondition(report.sample.haltReason === 0x01000000, `${role} guest halt was not normal`);
  validateCrc(report.native?.metrics?.crc, `${role} native`);
  validateCrc(report.sample.metrics?.crc, `${role} guest`);
  requireCondition(isDeepStrictEqual(report.native.metrics.crc, report.sample.metrics.crc),
    `${role} native/guest CRC mismatch`);
  let nativeMetrics, guestMetrics;
  try {
    nativeMetrics = parseCoremark(report.native.stdout, report.workload.iterations);
    guestMetrics = parseCoremark(report.sample.stdout, report.workload.iterations);
  } catch (error) {
    throw new Error(`comparison: ${role} retained CoreMark stdout invalid: ${error.message}`);
  }
  requireCondition(isDeepStrictEqual(report.native.metrics, nativeMetrics),
    `${role} native metrics/CRC differ from retained stdout`);
  requireCondition(isDeepStrictEqual(report.sample.metrics, guestMetrics),
    `${role} guest metrics/CRC differ from retained stdout`);
  for (const [kind, hash] of Object.entries({JS: report.artifact?.jsSha256, Wasm: report.artifact?.wasmSha256})) {
    requireCondition(typeof hash === 'string' && /^[0-9a-f]{64}$/.test(hash), `${role} ${kind} artifact hash invalid`);
  }
  requireCondition(object(report.sample.compile), `${role} compile totals missing`);

  let duplication;
  try {
    duplication = analyzeRegionDiagnostics(report.regionDiagnostics);
  } catch (error) {
    throw new Error(`comparison: ${role} region diagnostics invalid: ${error.message}`);
  }
  requireCondition(isDeepStrictEqual(report.duplication, duplication), `${role} embedded duplication is stale`);
  requireCondition(duplication.totals.regionCount === report.sample.compile.blocks,
    `${role} compile block totals differ from region diagnostics`);
  requireCondition(duplication.totals.moduleBytes === report.sample.compile.bytes,
    `${role} compile module byte totals differ from region diagnostics`);
  const descriptors = new Map(duplication.descriptors.map(row => [row.descriptor, row]));
  return {report, duplication, descriptors};
}

function descriptorView(row) {
  if (!row) return {selectionCount: 0, selectedRegionIds: [], entryRegionIds: [], duplicateSelectionCount: 0};
  return {
    selectionCount: row.selectedRegionIds.length,
    selectedRegionIds: [...row.selectedRegionIds],
    entryRegionIds: [...row.entryRegionIds],
    duplicateSelectionCount: row.duplicateSelectionCount,
  };
}

function targetComparison(before, after) {
  const summarize = validated => TARGETS.reduce((sum, target) => {
    const row = validated.descriptors.get(target);
    if (!row) return sum;
    return {
      selectionCount: sum.selectionCount + row.selectedRegionIds.length,
      duplicateSelectionCount: sum.duplicateSelectionCount + row.duplicateSelectionCount,
    };
  }, {selectionCount: 0, duplicateSelectionCount: 0});
  const baseline = summarize(before), candidate = summarize(after);
  return {
    before: baseline,
    after: candidate,
    delta: {
      selectionCount: candidate.selectionCount - baseline.selectionCount,
      duplicateSelectionCount: candidate.duplicateSelectionCount - baseline.duplicateSelectionCount,
    },
    descriptors: TARGETS.map(descriptor => ({
      descriptor,
      before: descriptorView(before.descriptors.get(descriptor)),
      after: descriptorView(after.descriptors.get(descriptor)),
    })),
  };
}

function copyRegion(region) {
  return {...region, selectedDescriptors: [...region.selectedDescriptors]};
}

function compactRegion(region) {
  return {
    regionId: region.regionId, frontend: region.frontend,
    entryDescriptor: region.entryDescriptor, entryPc: region.entryPc, entryIndex: region.entryIndex,
    moduleBytes: region.moduleBytes, entryBodyBytes: region.entryBodyBytes,
    selectedDescriptors: [...region.selectedDescriptors],
  };
}

function entriesByDescriptor(report) {
  const entries = new Map();
  for (const region of report.regionDiagnostics.regions) {
    const history = entries.get(region.entryDescriptor) || [];
    entries.set(region.entryDescriptor, [...history, region]);
  }
  return entries;
}

function historyComparison(before, after) {
  const baseline = entriesByDescriptor(before.report), candidate = entriesByDescriptor(after.report);
  const descriptors = [...new Set([...baseline.keys(), ...candidate.keys()])].sort();
  const ignoreRegionId = ({regionId, ...region}) => region;
  return descriptors.map(entryDescriptor => {
    const beforeHistory = (baseline.get(entryDescriptor) || []).map(compactRegion);
    const afterHistory = (candidate.get(entryDescriptor) || []).map(compactRegion);
    const changed = !isDeepStrictEqual(beforeHistory.map(ignoreRegionId), afterHistory.map(ignoreRegionId));
    return changed ? {entryDescriptor, before: beforeHistory, after: afterHistory} : null;
  }).filter(Boolean);
}

function retainedHotHistories(before, after) {
  const baseline = entriesByDescriptor(before.report), candidate = entriesByDescriptor(after.report);
  return HOT_ENTRIES.map(entryDescriptor => ({
    entryDescriptor,
    before: (baseline.get(entryDescriptor) || []).map(copyRegion),
    after: (candidate.get(entryDescriptor) || []).map(copyRegion),
  }));
}

function totalsComparison(before, after) {
  const baseline = {...before.duplication.totals}, candidate = {...after.duplication.totals};
  return {
    before: baseline, after: candidate,
    delta: Object.fromEntries(TOTAL_KEYS.map(key => [key, candidate[key] - baseline[key]])),
  };
}

export function compareReports(baselineReport, candidateReport) {
  const before = validateReport(baselineReport, 'baseline');
  const after = validateReport(candidateReport, 'candidate');
  for (const target of TARGETS) {
    requireCondition(before.descriptors.has(target), `baseline missing pinned target ${target}`);
  }
  requireCondition(isDeepStrictEqual(before.duplication.totals, BASELINE_TOTALS),
    'baseline archived totals differ from the pinned experiment baseline');
  const targets = targetComparison(before, after);
  requireCondition(targets.before.selectionCount === 451 && targets.before.duplicateSelectionCount === 423,
    'baseline target aggregate differs from selections=451 duplicates=423');
  requireCondition(after.report.artifact.wasmSha256 !== before.report.artifact.wasmSha256,
    'candidate Wasm artifact is unchanged from baseline');
  const artifacts = {
    before: {...before.report.artifact}, after: {...after.report.artifact},
    jsIdentical: after.report.artifact.jsSha256 === before.report.artifact.jsSha256,
    wasmIdentical: after.report.artifact.wasmSha256 === before.report.artifact.wasmSha256,
  };
  const totals = totalsComparison(before, after);
  const pass = totals.after.moduleBytes < BASELINE_TOTALS.moduleBytes &&
    totals.after.entryBodyBytes < BASELINE_TOTALS.entryBodyBytes;
  return {
    schema: COMPARISON_SCHEMA,
    verdict: pass ? 'PASS' : 'REJECT',
    artifacts,
    totals,
    targets,
    changedEntryHistories: historyComparison(before, after),
    retainedHotHistories: retainedHotHistories(before, after),
  };
}

function main(args) {
  requireCondition(args.length === 3 && args.every(Boolean),
    'usage: analyze.mjs <baseline.json> <candidate.json> <new-comparison.json>');
  const [baselinePath, candidatePath, outputPath] = args;
  requireCondition(!fs.existsSync(outputPath), `refusing to overwrite ${outputPath}`);
  const baseline = JSON.parse(fs.readFileSync(baselinePath, 'utf8'));
  const candidate = JSON.parse(fs.readFileSync(candidatePath, 'utf8'));
  const comparison = compareReports(baseline, candidate);
  fs.writeFileSync(outputPath, `${JSON.stringify(comparison, null, 2)}\n`, {flag: 'wx'});
  console.log(`${comparison.verdict} moduleBytes=${comparison.totals.after.moduleBytes} ` +
    `entryBodyBytes=${comparison.totals.after.entryBodyBytes}`);
  return comparison.verdict === 'PASS' ? 0 : 1;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    process.exitCode = main(process.argv.slice(2));
  } catch (error) {
    console.error(`Cannot compare push-RSB ablation reports: ${error.message}`);
    process.exitCode = 2;
  }
}
