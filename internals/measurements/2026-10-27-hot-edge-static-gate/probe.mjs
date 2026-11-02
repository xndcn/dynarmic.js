#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {
  ENTRY_PC, FULL_GRAPH_SAFETY_LIMIT, HOT_EXIT, HOT_TARGET, LIMITS,
  analyzeDiscoveredGraph, discoverGraph, parseOptimizedIr, validateReportIdentity,
} from './core.mjs';

const SOURCE = 'ec4701f20d2e53a133367e4048f3f0fc2b981c82';
const UPSTREAM = 'a41c380246d3d9f9874f0f792d234dc0cc17c180';
const WORKLOAD_SHA256 = 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa';
const A64_BINARY_SHA256 = '8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1';
const USAGE = 'usage: probe.mjs <native-block_dump> <a64-flat.bin> <base_hex> <pinned-flat-report.json[.gz]> <new-output.json>';

function readJson(file) {
  const bytes = fs.readFileSync(file);
  return JSON.parse(file.endsWith('.gz') ? gunzipSync(bytes).toString('utf8') : bytes.toString('utf8'));
}

export function validateBinarySha256(file, expected = A64_BINARY_SHA256) {
  const actual = createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  assert.equal(actual, expected, `a64 binary SHA-256 mismatch: ${actual}`);
  return actual;
}

export function runCli(argv) {
  assert.equal(argv.length, 5, USAGE);
  const [toolArgument, binaryArgument, base, reportArgument, outputArgument] = argv;
  const output = path.resolve(outputArgument);
  assert(!fs.existsSync(output), `refusing to overwrite ${output}`);
  assert(/^(?:0x)?[0-9a-f]+$/i.test(base), 'base must be hexadecimal');
  const normalizedBase = base.replace(/^0x/i, '').toLowerCase();
  assert.equal(normalizedBase, '10000', 'load base must be 10000');
  const loadBase = Number.parseInt(normalizedBase, 16);
  const tool = path.resolve(toolArgument);
  const binary = path.resolve(binaryArgument);
  const reportPath = path.resolve(reportArgument);
  const binarySha256 = validateBinarySha256(binary);
  const blockDumpSha256 = createHash('sha256').update(fs.readFileSync(tool)).digest('hex');
  const report = readJson(reportPath);
  validateReportIdentity(report, {
    source: SOURCE, upstream: UPSTREAM, workloadSha256: WORKLOAD_SHA256, binarySha256,
  });
  const cache = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-hot-edge-static-'));
  try {
    const translations = new Map();
    const translate = pc => {
      if (translations.has(pc)) return translations.get(pc);
      const wasm = path.join(cache, `${pc.toString(16)}.wasm`);
      const process = spawnSync(tool,
        ['--a64', `${binary}@${normalizedBase}`, pc.toString(16), wasm],
        {encoding: 'utf8', maxBuffer: 64 * 1024 * 1024});
      if (process.error) throw process.error;
      assert.equal(process.status, 0, `block_dump failed at 0x${pc.toString(16)}: ${process.stderr}`);
      const parsed = parseOptimizedIr(process.stderr, pc);
      translations.set(pc, parsed);
      return parsed;
    };
    const baseline = discoverGraph(ENTRY_PC, translate, {
      blockLimit: LIMITS.maxBlocks, instructionLimit: LIMITS.maxInstructions,
      maxGuestDistance: LIMITS.maxGuestDistance,
    });
    const full = discoverGraph(ENTRY_PC, translate, {
      maxGuestDistance: LIMITS.maxGuestDistance, safetyLimit: FULL_GRAPH_SAFETY_LIMIT,
    });
    const analysis = analyzeDiscoveredGraph({
      entry: ENTRY_PC, hotExit: HOT_EXIT, hotTarget: HOT_TARGET,
      full, baseline, report, limits: LIMITS,
    });
    const result = {
      source: SOURCE, upstream: UPSTREAM, git: {dirty: report.git.dirty},
      architecture: report.architecture, memoryMode: report.memoryMode,
      workloadSha256: report.workloadSha256, a64BinarySha256: binarySha256,
      loadBase, blockDumpSha256,
      ...(report.workload?.artifacts?.a64?.sha256 === undefined ? {} :
        {reportA64BinarySha256: report.workload.artifacts.a64.sha256}),
      ...analysis,
    };
    fs.writeFileSync(output, `${JSON.stringify(result, null, 2)}\n`, {flag: 'wx'});
    return result;
  } finally {
    fs.rmSync(cache, {recursive: true, force: true});
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const result = runCli(process.argv.slice(2));
  console.log(`static probe PASS full=${result.full_reachable_within_distance.blocks}/${result.full_reachable_within_distance.instructions} baseline=${result.baseline.blocks}/${result.baseline.instructions} required=${result.required.blocks}/${result.required.instructions} final=${result.filled_selection.blocks}/${result.filled_selection.instructions}`);
}
