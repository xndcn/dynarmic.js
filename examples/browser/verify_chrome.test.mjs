// Exercise the smoke driver's failure paths without a running browser.
import assert from 'node:assert/strict';
import {mkdtempSync, writeFileSync, rmSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

const driver = fileURLToPath(new URL('verify_chrome.js', import.meta.url));

function run(scenario, args = ['http://localhost:8000/?auto=1', '9333']) {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'dynarmic-chrome-driver-'));
  try {
    const preload = path.join(directory, 'fake.cjs');
    writeFileSync(preload, `
      const scenario = ${JSON.stringify(scenario)};
      const timeout = global.setTimeout;
      global.setTimeout = (fn, ms, ...args) => timeout(fn, Math.min(ms, 20), ...args);
      let now = 0;
      let pageTitle = 'DONE PASS';
      Date.now = () => (now += 1000);
      global.fetch = async () => ({ok: true, json: async () =>
        scenario === 'bad-targets' ? {error: 'not an array'} :
        scenario === 'empty-targets' ? [] :
        [{type: 'page', webSocketDebuggerUrl: 'ws://localhost/fake'}]});
      global.WebSocket = class {
        constructor() {
          queueMicrotask(() => {
            if (scenario === 'connect-error') this.onerror();
            else if (scenario === 'connect-close') this.onclose();
            else if (scenario !== 'connect-timeout') this.onopen();
          });
        }
        send(raw) {
          const {id, method, params} = JSON.parse(raw);
          if (scenario === 'send-error') throw new Error('cannot send');
          queueMicrotask(() => {
            if (scenario === 'request-close') return this.onclose();
            if (scenario === 'request-timeout') return;
            if (scenario === 'rpc-error') return this.reply({id, error: {message: 'denied'}});
            if (method === 'Page.navigate' && scenario === 'navigation-error') {
              return this.reply({id, result: {errorText: 'net::ERR_FAILED'}});
            }
            if (method === 'Runtime.evaluate') {
              if (scenario === 'evaluation-error') return this.reply({id, result: {exceptionDetails: {text: 'ReferenceError'}}});
              if (params.expression.startsWith('document.title =')) pageTitle = 'Loading';
              const title = scenario === 'stale-page' ? pageTitle : 'DONE PASS';
              return this.reply({id, result: {result: {value: params.expression === 'document.title' ? title : 'ok'}}});
            }
            this.reply({id, result: {}});
          });
        }
        reply(value) { this.onmessage({data: JSON.stringify(value)}); }
        close() {}
      };
    `);
    return spawnSync(process.execPath, ['--require', preload, driver, ...args], {
      encoding: 'utf8', timeout: 5000,
    });
  } finally {
    rmSync(directory, {recursive: true, force: true});
  }
}

test('Chrome smoke rejects invalid arguments before discovery', () => {
  for (const args of [[], ['file:///demo', '9333'], ['http://localhost', '0'], ['http://localhost', '65536']]) {
    const result = run('success', args);
    assert.equal(result.status, 1, result.stderr);
    assert.match(result.stderr, /usage:/);
  }
});

test('Chrome smoke handles malformed or empty target discovery', () => {
  for (const scenario of ['bad-targets', 'empty-targets']) {
    const result = run(scenario);
    assert.equal(result.status, 1, result.stderr);
    assert.match(result.stderr, /did not expose a page target/);
  }
});

test('Chrome smoke bounds connection and request failures', () => {
  const cases = {
    'connect-error': /WebSocket failed/,
    'connect-close': /closed before connecting/,
    'connect-timeout': /timed out connecting/,
    'request-close': /connection closed or failed/,
    'request-timeout': /timeout: Page.enable/,
    'send-error': /cannot send/,
    'rpc-error': /Page.enable: denied/,
    'navigation-error': /navigation failed: net::ERR_FAILED/,
    'evaluation-error': /evaluation failed: ReferenceError/,
  };
  for (const [scenario, message] of Object.entries(cases)) {
    const result = run(scenario);
    assert.equal(result.status, 1, `${scenario}: ${result.stderr}`);
    assert.match(result.stderr, message, scenario);
  }
});

test('Chrome smoke still recognizes a passing page and prints its output', () => {
  const result = run('success');
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /DONE PASS/);
  assert.match(result.stdout, /--- output ---/);
});

test('Chrome smoke does not accept a previous DONE title while navigation is stalled', () => {
  const result = run('stale-page');
  assert.equal(result.status, 2, result.stderr);
  assert.match(result.stdout, /timeout; last status:/);
  assert.doesNotMatch(result.stdout, /title: DONE PASS/);
});
