#!/usr/bin/env node

// Check the final static artifact, not release cleanliness or CPU correctness.
// CMake and package_source.py remain the owners of engines and provenance.
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {readFile, lstat} from 'node:fs/promises';
import {constants} from 'node:os';
import {resolve, join, isAbsolute} from 'node:path';
import {pathToFileURL} from 'node:url';
import {parse} from 'parse5';

const ROOT = resolve(import.meta.dirname, '..');
const REQUIRED = [
  'index.html', '404.html', 'robots.txt', 'sitemap.xml', 'site.webmanifest',
  'icon.svg', 'icon-192.png', 'icon-512.png', 'social-card.png',
  'source-manifest.json', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.md',
];
const RUNTIME_SECTIONS = ['runtimeBuild', 'officialHostBuild', 'quickjsBuild', 'cBenchBuild'];
const HELP = `Usage:
  npm run pages:check -- [--dist <generated-pages-root>] [--strict]
  npm run build -- [--build-dir <configured-cmake-build>]

DYNARMIC_JS_BUILD_DIR defaults to build-em. DYNARMIC_JS_PAGES_DIST overrides
only the check/preview artifact directory. Configure CMake before running build.
--strict requires all four engine manifests and verifies their artifact hashes.
This checks local artifacts; it does not certify a clean, publishable release.`;

class UsageError extends Error { exitCode = 2; }

export function parseArgs(argv, env = process.env) {
  if (argv.includes('--help') || argv.includes('-h')) return {help: true};
  const [command, ...args] = argv;
  if (!['check', 'build'].includes(command)) throw new UsageError(`unknown command ${command ?? '(missing)'}`);
  let dist, buildDir, strict = false;
  const seen = new Set();
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (seen.has(arg)) throw new UsageError(`duplicate option ${arg}`);
    seen.add(arg);
    if (arg === '--strict' && command === 'check') strict = true;
    else if ((arg === '--dist' && command === 'check') || (arg === '--build-dir' && command === 'build')) {
      const value = args[++i];
      if (!value || value.startsWith('-')) throw new UsageError(`${arg} requires a path`);
      if (arg === '--dist') dist = value;
      else buildDir = value;
    } else throw new UsageError(`unknown option ${arg} for ${command}`);
  }
  buildDir = resolve(ROOT, buildDir ?? env.DYNARMIC_JS_BUILD_DIR ?? 'build-em');
  return {command, strict, buildDir,
    dist: resolve(ROOT, dist ?? (command === 'check' ? env.DYNARMIC_JS_PAGES_DIST : undefined) ?? join(buildDir, 'examples/browser/dist'))};
}

function safeName(name) {
  if (typeof name !== 'string' || !name || isAbsolute(name) || /[\\\0]/.test(name) ||
      name.split('/').some(part => !part || part === '.' || part === '..')) {
    throw new Error(`unsafe asset path: ${name}`);
  }
  return name;
}

async function mustBeFile(dist, name) {
  safeName(name);
  const parts = name.split('/');
  let path = dist;
  for (let i = 0; i < parts.length; i++) {
    path = join(path, parts[i]);
    let stat;
    try { stat = await lstat(path); }
    catch { throw new Error(`missing Pages asset: ${name}`); }
    if (stat.isSymbolicLink()) throw new Error(`symlink in Pages asset path: ${name}`);
    if (i < parts.length - 1) {
      if (!stat.isDirectory()) throw new Error(`Pages asset parent is not a directory: ${name}`);
    } else {
      if (!stat.isFile()) throw new Error(`Pages asset is not a regular file: ${name}`);
      if (!stat.size) throw new Error(`Pages asset is empty: ${name}`);
    }
  }
  return path;
}

function elements(node, result = []) {
  if (node.tagName) result.push(node);
  // Template contents are inert. Escaped code examples are text nodes; real
  // resource elements still load in the browser, even inside pre/code tags.
  if (node.tagName === 'template') return result;
  for (const child of node.childNodes ?? []) elements(child, result);
  return result;
}
const attr = (node, name) => node?.attrs?.find(attribute => attribute.name === name)?.value;
const tokens = value => (value ?? '').toLowerCase().split(/\s+/);
const hash = value => createHash('sha256').update(value).digest('hex');

function localReference(value, pageURL, siteURL, {required = false} = {}) {
  if (!value || value.startsWith('#')) {
    if (required) throw new Error('manifest icon must have a local src');
    return null;
  }
  const url = new URL(value, pageURL);
  if (url.origin !== siteURL.origin || !['http:', 'https:'].includes(url.protocol)) {
    if (required) throw new Error(`manifest icon must be local: ${value}`);
    return null;
  }
  // Reject traversal before URL normalization can erase it, including escapes.
  const rawPath = value.split(/[?#]/, 1)[0];
  let decoded;
  try { decoded = decodeURIComponent(rawPath); }
  catch { throw new Error(`invalid asset URL: ${value}`); }
  if (decoded.includes('\\') || decoded.split('/').includes('..')) throw new Error(`unsafe asset URL: ${value}`);
  if (!url.pathname.startsWith(siteURL.pathname)) throw new Error(`asset URL escapes Pages base: ${value}`);
  let name = decodeURIComponent(url.pathname.slice(siteURL.pathname.length));
  if (!name || name.endsWith('/')) name += 'index.html';
  safeName(name);
  return {name, url};
}

async function verifyRecordedFile(dist, name, expected) {
  if (!expected || typeof expected !== 'object' || !/^[0-9a-f]{64}$/.test(expected.sha256) ||
      !Number.isSafeInteger(expected.bytes) || expected.bytes <= 0) {
    throw new Error(`invalid hash/size metadata for ${name}`);
  }
  const file = await mustBeFile(dist, name);
  const data = await readFile(file);
  if (data.length !== expected.bytes || hash(data) !== expected.sha256) {
    throw new Error(`artifact hash/size mismatch: ${name}`);
  }
}

export async function check({dist, strict = false}) {
  dist = resolve(dist);
  const root = await lstat(dist).catch(() => null);
  if (!root?.isDirectory() || root.isSymbolicLink()) throw new Error(`Pages root is not a regular directory: ${dist}`);
  for (const name of REQUIRED) await mustBeFile(dist, name);
  const html = await readFile(join(dist, 'index.html'), 'utf8');
  const document = parse(html);
  const nodes = elements(document);
  if (!document.childNodes.some(node => node.nodeName === '#documentType' && node.name === 'html')) {
    throw new Error('index.html must have an HTML doctype');
  }
  if (!/^[a-z]+(?:-[a-z0-9]+)*$/i.test(attr(nodes.find(node => node.tagName === 'html'), 'lang') ?? '')) {
    throw new Error('index.html is missing a language attribute');
  }
  const canonical = nodes.find(node => node.tagName === 'link' && tokens(attr(node, 'rel')).includes('canonical'));
  const siteURL = new URL(attr(canonical, 'href') ?? 'https://pages.invalid/');
  if (!['http:', 'https:'].includes(siteURL.protocol) || !siteURL.pathname.endsWith('/')) {
    throw new Error('canonical Pages URL must be an HTTP(S) directory URL');
  }
  for (const page of ['index.html', '404.html']) {
    const pageNodes = page === 'index.html' ? nodes : elements(parse(await readFile(join(dist, page), 'utf8')));
    const pageURL = new URL(page, siteURL);
    for (const node of pageNodes) {
      for (const attribute of ['src', 'href', 'poster']) {
        const value = attr(node, attribute);
        if (value === undefined) continue;
        const reference = localReference(value, pageURL, siteURL);
        if (!reference) continue;
        const file = await mustBeFile(dist, reference.name);
        const cacheKeyRequired = page === 'index.html' &&
          ((node.tagName === 'script' && attribute === 'src' && reference.name.endsWith('.js')) ||
           (node.tagName === 'link' && attribute === 'href' && reference.name.endsWith('.css')));
        if (cacheKeyRequired) {
          const versions = reference.url.searchParams.getAll('v');
          if (versions.length !== 1 || !/^[0-9a-f]{64}$/.test(versions[0])) {
            throw new Error(`missing or invalid content hash in index.html: ${value}`);
          }
          if (hash(await readFile(file)) !== versions[0]) throw new Error(`stale content hash in index.html: ${value}`);
        }
      }
    }
  }
  const manifest = JSON.parse(await readFile(join(dist, 'site.webmanifest'), 'utf8'));
  if (manifest.start_url !== './' || manifest.scope !== './') {
    throw new Error('site.webmanifest must use relative start_url and scope for repository Pages');
  }
  if (!Array.isArray(manifest.icons) || manifest.icons.length < 2) {
    throw new Error('site.webmanifest must expose at least two icons');
  }
  for (const icon of manifest.icons) {
    const reference = localReference(icon?.src, new URL('site.webmanifest', siteURL), siteURL, {required: true});
    await mustBeFile(dist, reference.name);
  }
  const source = JSON.parse(await readFile(join(dist, 'source-manifest.json'), 'utf8'));
  if (source.schema !== 'dynarmic.js/pages-source-v1') throw new Error(`unsupported Pages source manifest schema: ${source.schema ?? 'missing'}`);
  for (const [section, name] of [['archive', 'source.tar.gz'], ['runtimeNotices', 'runtime-licenses.txt']]) {
    if (source[section]?.path !== name) throw new Error(`source manifest is missing ${section}.path: ${name}`);
    await verifyRecordedFile(dist, name, source[section]);
  }
  if (strict) {
    for (const section of RUNTIME_SECTIONS) {
      const artifacts = source[section]?.artifacts;
      if (!artifacts || Array.isArray(artifacts) || typeof artifacts !== 'object' || !Object.keys(artifacts).length) {
        throw new Error(`strict Pages artifact is missing ${section}.artifacts`);
      }
      for (const [name, expected] of Object.entries(artifacts)) await verifyRecordedFile(dist, name, expected);
    }
  }
  return {dist, strict};
}

function run(command, args) {
  return new Promise((resolveRun, reject) => {
    const child = spawn(command, args, {cwd: ROOT, stdio: 'inherit'});
    child.on('error', reject);
    child.on('exit', (code, signal) => {
      if (code === 0) resolveRun();
      else {
        const error = new Error(`${command} ${signal ? `terminated by ${signal}` : `exited with ${code}`}`);
        error.exitCode = code ?? 128 + (constants.signals[signal] ?? 1);
        reject(error);
      }
    });
  });
}

export async function build({buildDir, dist}) {
  await run('cmake', ['--build', buildDir, '--target', 'browser_page', '--parallel', '2']);
  await run('python3', [join(ROOT, 'examples/browser/package_source.py'), '--dist', dist]);
  return check({dist, strict: true});
}

async function main() {
  try {
    const options = parseArgs(process.argv.slice(2));
    if (options.help) { console.log(HELP); return; }
    const result = await (options.command === 'build' ? build(options) : check(options));
    console.log(`Pages artifact valid: ${result.dist}${result.strict ? ' (strict)' : ''}`);
  } catch (error) {
    console.error(`pages: ${error.message}`);
    if (error instanceof UsageError) console.error(HELP);
    process.exitCode = error.exitCode ?? 1;
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) await main();
