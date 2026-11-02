#!/usr/bin/env node
// Execute the code visitors copy from Pages against the real installed tarball.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {build} from 'vite';
import {withChromium} from '../browser/chromium.mjs';
import {install} from './fixture.mjs';

const [tarball, browser, ...extra] = process.argv.slice(2);
const options = {};
for (let index = 0; index < extra.length; index += 2) {
  const name = extra[index];
  if (!['--report', '--html'].includes(name) || !extra[index + 1] || options[name])
    throw new Error('Usage: node tests/npm/verify_pages_examples.mjs package.tgz chrome-executable [--html page.html] [--report result.json]');
  options[name] = extra[index + 1];
}
if (!tarball || !browser)
  throw new Error('Usage: node tests/npm/verify_pages_examples.mjs package.tgz chrome-executable [--html page.html] [--report result.json]');

const htmlPath = path.resolve(options['--html'] || fileURLToPath(new URL('../../examples/browser/index.html', import.meta.url)));
const html = fs.readFileSync(htmlPath, 'utf8');
const sha256 = content => createHash('sha256').update(content).digest('hex');
const htmlSha256 = sha256(html);
const snippets = {};
function code(id) {
  const matches = [...html.matchAll(new RegExp(`<pre\\b[^>]*\\bid=["']${id}["'][^>]*>([\\s\\S]*?)<\\/pre>`, 'g'))];
  assert.equal(matches.length, 1, `Pages must expose one ${id} code block`);
  const markup = matches[0][1].trim();
  assert.match(markup, /^<code(?:\s[^>]*)?>[\s\S]*<\/code>$/);
  let source = markup.replace(/^<code(?:\s[^>]*)?>/, '').replace(/<\/code>$/, '');
  // These are plain code blocks. Decode once, as HTML textContent does; do not
  // accidentally treat a decoded JavaScript comparison as an HTML tag.
  assert(!/<\/?[a-z][^>]*>/i.test(source), `${id} must contain copyable plain source`);
  const named = {amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: '\u00a0'};
  source = source.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, (match, entity) => {
    if (entity.startsWith('#')) {
      const number = entity[1].toLowerCase() === 'x' ? parseInt(entity.slice(2), 16) : Number(entity.slice(1));
      return String.fromCodePoint(number);
    }
    assert(Object.hasOwn(named, entity), `Unknown HTML entity ${match} in ${id}`);
    return named[entity];
  });
  snippets[id] = {sha256: sha256(source), bytes: Buffer.byteLength(source)};
  return source;
}

const installation = code('npm-install-command');
assert.match(installation, /npm install\s+[^\n]+\.tgz(?:\s|$)/,
  'Release downloads and local builds must use a tarball install command');
const nodeExample = code('npm-node-example');
const mainExample = code('npm-vite-main');
const workerExample = code('npm-vite-worker');
const configExample = code('npm-vite-config');
const htmlExample = code('npm-vite-html');
const fixture = install(tarball);
const checks = [];
try {
  fs.writeFileSync(path.join(fixture.root, 'hello.mjs'), nodeExample);
  const output = execFileSync(process.execPath, ['hello.mjs'],
    {cwd: fixture.root, encoding: 'utf8', timeout: 30000});
  assert.equal(output.trim(), 'svc 7 42', 'Pages Node example must execute its documented guest result');
  checks.push('Unchanged Pages Node example executes ARM32 and reports svc 7 42');

  fs.writeFileSync(path.join(fixture.root, 'main.js'), mainExample);
  fs.writeFileSync(path.join(fixture.root, 'worker.js'), workerExample);
  const config = path.join(fixture.root, 'vite.config.js');
  fs.writeFileSync(config, configExample);
  fs.writeFileSync(path.join(fixture.root, 'index.html'), htmlExample);
  // Observe the example's ordinary output without replacing its Worker or any
  // package calls. Startup/runtime failures are retained rather than timing out.
  const observe = `
window.exampleLogs = [];
window.exampleErrors = [];
const log = console.log;
const error = console.error;
console.log = (...args) => {window.exampleLogs.push(args);log(...args);};
console.error = (...args) => {window.exampleErrors.push(args.map(String).join(' '));error(...args);};
window.addEventListener('error', event => window.exampleErrors.push(event.message));
window.addEventListener('unhandledrejection', event => window.exampleErrors.push(String(event.reason)));
`;
  await build({root: fixture.root, configFile: config, base: '/nested/quickjs/',
    build: {target: 'es2022', assetsInlineLimit: 0}, logLevel: 'warn'});
  checks.push('Unchanged Pages Vite configuration builds the documented main and Worker modules');

  const requests = [];
  let result, environment;
  await withChromium({browser, root: path.join(fixture.root, 'dist'), serve: ({name}) => {
    requests.push(name);
    return false;
  }}, async ({call, navigate, evaluate, until}) => {
    await call('Page.addScriptToEvaluateOnNewDocument', {source: observe});
    await navigate();
    const observation = await until(() => evaluate(`
      window.exampleErrors?.length ? {errors: window.exampleErrors} :
      window.exampleLogs?.length ? {logs: window.exampleLogs} : null
    `), 'Pages Vite example output', 30000);
    assert(!observation.errors, `Pages example failed: ${JSON.stringify(observation.errors)}`);
    assert.equal(observation.logs.length, 1, 'The Worker example should produce one result');
    assert.equal(observation.logs[0].length, 1, 'The Worker example should log its result object');
    result = observation.logs[0][0];
    assert.deepEqual(result, {reason: 'svc', value: '42'});
    assert(requests.some(name => /^assets\/worker-.*\.js$/.test(name)), 'The production Worker must load under the nested base');
    assert(requests.some(name => /^assets\/.*\.wasm$/.test(name)), 'The exported Wasm asset must be emitted and fetched');
    environment = await evaluate('navigator.userAgent');
  });
  checks.push('Pages AArch64 Worker executes in Chromium under a nested base and returns svc / 42');
  checks.push('Vite emits and loads the package Wasm asset through the documented URL import');
  assert.equal(sha256(fs.readFileSync(htmlPath)), htmlSha256, 'Pages HTML changed while its examples were being checked');
  const report = {status: 'pass', node: process.version, environment,
    tarballSha256: fixture.tarballSha256, html: htmlPath, htmlSha256, snippets, checks, result, requests};
  if (options['--report']) fs.writeFileSync(options['--report'], JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report, null, 2));
} finally { fixture.cleanup(); }
