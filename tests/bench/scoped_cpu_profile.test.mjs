import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {captureCpuProfile} from './scoped_cpu_profile.mjs';

function workspace(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'scoped-cpu-profile-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  return root;
}

function busyLoop() {
  const end = performance.now() + 20;
  while (performance.now() < end) {}
}

function checkProfile(file) {
  const profile = JSON.parse(fs.readFileSync(file, 'utf8'));
  assert(Array.isArray(profile.nodes) && profile.nodes.length > 0);
  assert(Array.isArray(profile.samples));
  assert(Array.isArray(profile.timeDeltas));
  assert.equal(profile.samples.length, profile.timeDeltas.length);
  assert(Number.isFinite(profile.startTime));
  assert(Number.isFinite(profile.endTime));
  assert(profile.endTime >= profile.startTime);
  return profile;
}

async function settle(promise) {
  try {
    return {rejected: false, value: await promise};
  } catch (error) {
    return {rejected: true, error};
  }
}

test('captures an async callback and returns its value', async t => {
  const output = path.join(workspace(t), 'async.cpuprofile');
  const sentinel = {completed: true};
  const result = await captureCpuProfile(output, async () => {
    busyLoop();
    await new Promise(resolve => setImmediate(resolve));
    return sentinel;
  }, {interval: 250});
  assert.equal(result, sentinel);
  checkProfile(output);
});

test('sync throw and async rejection retain profiles and preserve errors', async t => {
  const root = workspace(t);
  for (const [name, asynchronous] of [['sync', false], ['async', true]]) {
    const output = path.join(root, `${name}.cpuprofile`);
    const sentinel = new Error(`${name} sentinel`);
    const callback = () => {
      busyLoop();
      throw sentinel;
    };
    await assert.rejects(captureCpuProfile(output,
      asynchronous ? async () => callback() : callback), error => error === sentinel);
    checkProfile(output);
  }
  const afterFailure = path.join(root, 'after-failure.cpuprofile');
  assert.equal(await captureCpuProfile(afterFailure, () => 42), 42);
  checkProfile(afterFailure);
});

test('falsy callback failures remain rejections and compose with cleanup errors', async t => {
  const root = workspace(t);
  for (const asynchronous of [false, true]) {
    for (const [index, sentinel] of [undefined, null, false, 0, ''].entries()) {
      const output = path.join(root, `falsy-${asynchronous}-${index}.cpuprofile`);
      const callback = () => {
        busyLoop();
        throw sentinel;
      };
      const outcome = await settle(captureCpuProfile(output,
        asynchronous ? async () => callback() : callback));
      assert(outcome.rejected, `falsy ${typeof sentinel} failure was accepted`);
      assert.strictEqual(outcome.error, sentinel);
      checkProfile(output);
    }
  }

  const output = path.join(root, 'falsy-and-cleanup.cpuprofile');
  const outcome = await settle(captureCpuProfile(output, () => {
    busyLoop();
    fs.writeFileSync(output, 'falsy sentinel');
    throw null;
  }));
  assert(outcome.rejected);
  assert(outcome.error instanceof AggregateError);
  assert.strictEqual(outcome.error.errors[0], null);
  assert.equal(outcome.error.errors[1].code, 'EEXIST');
});

test('never overwrites an existing destination, including a creation race', async t => {
  const root = workspace(t);
  const existing = path.join(root, 'existing.cpuprofile');
  fs.writeFileSync(existing, 'sentinel');
  let invoked = false;
  await assert.rejects(captureCpuProfile(existing, () => { invoked = true; }),
    error => error.code === 'EEXIST');
  assert.equal(invoked, false);
  assert.equal(fs.readFileSync(existing, 'utf8'), 'sentinel');

  const race = path.join(root, 'race.cpuprofile');
  await assert.rejects(captureCpuProfile(race, () => {
    busyLoop();
    fs.writeFileSync(race, 'race sentinel');
  }), error => error.code === 'EEXIST');
  assert.equal(fs.readFileSync(race, 'utf8'), 'race sentinel');

  const both = path.join(root, 'both-errors.cpuprofile');
  const callbackError = new Error('callback and write failed');
  await assert.rejects(captureCpuProfile(both, () => {
    busyLoop();
    fs.writeFileSync(both, 'both sentinel');
    throw callbackError;
  }), error => error instanceof AggregateError && error.errors.length === 2
    && error.errors[0] === callbackError && error.errors[1].code === 'EEXIST');
  assert.equal(fs.readFileSync(both, 'utf8'), 'both sentinel');
});

test('validates arguments before invoking the callback', async t => {
  const root = workspace(t);
  let invocations = 0;
  const callback = () => { invocations++; };
  const cases = [
    ['', callback, {}], [null, callback, {}], [1, callback, {}],
    [path.join(root, 'missing-callback.cpuprofile'), undefined, {}],
    [path.join(root, 'bad-callback.cpuprofile'), 1, {}],
    ...[0, -1, 1.5, NaN, Infinity, '100', Number.MAX_SAFE_INTEGER + 1].map((interval, index) =>
      [path.join(root, `interval-${index}.cpuprofile`), callback, {interval}]),
  ];
  for (const [output, fn, options] of cases) {
    await assert.rejects(captureCpuProfile(output, fn, options));
    if (typeof output === 'string' && output) assert(!fs.existsSync(output));
  }
  assert.equal(invocations, 0);
});

test('rejects reentrant captures without invoking the second callback', async t => {
  const root = workspace(t);
  let secondInvoked = false;
  const first = captureCpuProfile(path.join(root, 'first.cpuprofile'), async () => {
    busyLoop();
    await assert.rejects(captureCpuProfile(path.join(root, 'second.cpuprofile'),
      () => { secondInvoked = true; }), /already active/);
  });
  await first;
  assert.equal(secondInvoked, false);
  assert(!fs.existsSync(path.join(root, 'second.cpuprofile')));
});
