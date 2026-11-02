import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {output} from './coremark_fixtures.mjs';

const sample = fileURLToPath(new URL('./coremark/sample.mjs', import.meta.url));
const profile = fileURLToPath(new URL('./coremark/profile.mjs', import.meta.url));

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'coremark-profile-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const jit = path.join(root, 'arm_jit.js');
  const binary = path.join(root, 'coremark.bin');
  fs.writeFileSync(binary, 'synthetic guest');
  const text = JSON.stringify(output(1000));
  fs.writeFileSync(jit, `module.exports = async () => {
    const fs = require('node:fs');
    const events = [];
    const record = name => events.push(name);
    const text = new TextEncoder().encode(${text});
    const heap = new Uint8Array(200000); heap.set(text);
    const compile = {blocks: 1, bytes: 100, translateMs: 0.1, optMs: 0,
      emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0};
    return {HEAPU8: heap, _arm_ram: () => 10000, _arm_ram_size: () => 100000,
      _arm_console_clear() {}, _dynarmic_wasm_debug_names(value) {
        record('debug:' + value);
      }, _arm_init(mode, architecture) {
        record('init:' + mode + ':' + architecture);
      }, _arm_run(entry) {
        record('run:' + entry);
        fs.writeFileSync(process.env.COREMARK_EVENTS, JSON.stringify(events));
        const end = performance.now() + 20; while (performance.now() < end) {}
        if (process.env.COREMARK_CREATE_PROFILE) {
          fs.writeFileSync(process.env.COREMARK_CREATE_PROFILE, 'race sentinel');
        }
        return Number(process.env.COREMARK_HALT || 0x01000000);
      }, _arm_console: () => 0, _arm_console_len: () => text.length,
      dynarmicWasmStats: {activeInstance: 1, getInstance: () => ({compile})}};
  };`);
  return {root, jit, binary};
}

function invoke(script, {jit, binary}, outputFile, extraEnv = {}) {
  const events = path.join(path.dirname(jit), `${path.basename(script)}-${extraEnv.COREMARK_HALT || 'ok'}-events.json`);
  const args = [script, jit, binary, 'a64', '2'];
  if (outputFile) args.push(outputFile);
  const result = spawnSync(process.execPath, args, {encoding: 'utf8', timeout: 20000,
    env: {...process.env, COREMARK_EVENTS: events, ...extraEnv}});
  return {result, events};
}

function parseSample(result) {
  assert.equal(result.status, 0, result.stderr);
  const lines = result.stdout.trim().split('\n');
  assert.equal(lines.length, 1, result.stdout);
  assert(lines[0].startsWith('COREMARK_SAMPLE '));
  return JSON.parse(lines[0].slice('COREMARK_SAMPLE '.length));
}

function checkProfile(file) {
  const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
  assert(Array.isArray(raw.nodes) && raw.nodes.length > 0);
  assert(Array.isArray(raw.samples) && Array.isArray(raw.timeDeltas));
  assert.equal(raw.samples.length, raw.timeDeltas.length);
  assert(Number.isFinite(raw.startTime) && Number.isFinite(raw.endTime));
  assert(raw.endTime >= raw.startTime);
}

test('profile preserves the ordinary sample and enables names before init/run', t => {
  const paths = fixture(t);
  const ordinary = invoke(sample, paths);
  const ordinarySample = parseSample(ordinary.result);
  assert.deepEqual(JSON.parse(fs.readFileSync(ordinary.events, 'utf8')), ['init:2:1', 'run:65536']);

  const outputFile = path.join(paths.root, 'run.cpuprofile');
  const profiled = invoke(profile, paths, outputFile);
  const profiledSample = parseSample(profiled.result);
  assert.deepEqual(JSON.parse(fs.readFileSync(profiled.events, 'utf8')),
    ['debug:1', 'init:2:1', 'run:65536']);
  const {runMs: ordinaryRunMs, ...ordinaryComparable} = ordinarySample;
  const {runMs: profiledRunMs, ...profiledComparable} = profiledSample;
  assert(Number.isFinite(ordinaryRunMs) && Number.isFinite(profiledRunMs));
  assert.deepEqual(profiledComparable, ordinaryComparable);
  checkProfile(outputFile);
});

test('guest failure retains its profile and emits no successful sample', t => {
  const paths = fixture(t);
  const outputFile = path.join(paths.root, 'fault.cpuprofile');
  const {result} = invoke(profile, paths, outputFile, {COREMARK_HALT: '7'});
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /guest fault: halt reason 7/);
  assert(!result.stdout.includes('COREMARK_SAMPLE'));
  checkProfile(outputFile);
});

test('guest fault and profile write failure retain both errors', t => {
  const paths = fixture(t);
  const outputFile = path.join(paths.root, 'fault-race.cpuprofile');
  const {result} = invoke(profile, paths, outputFile,
    {COREMARK_HALT: '7', COREMARK_CREATE_PROFILE: outputFile});
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /AggregateError/);
  assert.match(result.stderr, /guest fault: halt reason 7/);
  assert.match(result.stderr, /EEXIST/);
  assert(!result.stdout.includes('COREMARK_SAMPLE'));
  assert.equal(fs.readFileSync(outputFile, 'utf8'), 'race sentinel');
});

test('existing profile output is unchanged and the guest does not run', t => {
  const paths = fixture(t);
  const outputFile = path.join(paths.root, 'existing.cpuprofile');
  fs.writeFileSync(outputFile, 'sentinel');
  const {result, events} = invoke(profile, paths, outputFile);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /profile output already exists/);
  assert.equal(fs.readFileSync(outputFile, 'utf8'), 'sentinel');
  assert(!fs.existsSync(events));
});
