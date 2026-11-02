#!/usr/bin/env node
// Kept in the tarball so npm pack and downstream audits verify identical files.
import {createHash} from 'node:crypto';
import {readFileSync, readdirSync, lstatSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import path from 'node:path';

const directory = path.dirname(fileURLToPath(import.meta.url));
const options = process.argv.slice(2);
if (options.some(option => option !== '--require-clean')) throw new Error('Usage: node verify.mjs [--require-clean]');
const read = name => readFileSync(path.join(directory, name));
const manifest = JSON.parse(read('source-manifest.json'));
const metadata = JSON.parse(read('package.json'));
const required = [
  'package.json', 'index.js', 'index.d.ts', 'runtime.mjs', 'runtime.wasm',
  'runtime-build-manifest.json', 'source.tar.gz', 'runtime-licenses.txt',
  'LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md', 'REBUILD.md', 'verify.mjs',
];
if (manifest.schema !== 'dynarmic.js/npm-source-v1' ||
    manifest.package.name !== metadata.name || manifest.package.version !== metadata.version)
  throw new Error('Package metadata does not match source manifest');
if (JSON.stringify(Object.keys(manifest.files).sort()) !== JSON.stringify([...required].sort()))
  throw new Error('Manifest file inventory differs from the package contract');
for (const name of required) {
  if (!lstatSync(path.join(directory, name)).isFile()) throw new Error(`Non-regular package file: ${name}`);
  const contents = read(name);
  const actual = createHash('sha256').update(contents).digest('hex');
  if (manifest.files[name].bytes !== contents.byteLength || manifest.files[name].sha256 !== actual)
    throw new Error(`Package checksum mismatch: ${name}`);
}
const actual = readdirSync(directory).filter(name => name !== 'node_modules');
if (JSON.stringify(actual.sort()) !== JSON.stringify([...required, 'source-manifest.json'].sort()))
  throw new Error('Package contains files outside its allowlist');
const runtime = JSON.parse(read('runtime-build-manifest.json'));
if (runtime.schema !== 'dynarmic.js/npm-runtime-build-v1' ||
    runtime.inputs.sha256 !== manifest.runtime.inputSha256 ||
    JSON.stringify(runtime.source) !== JSON.stringify(manifest.runtime.source) ||
    JSON.stringify(runtime.build) !== JSON.stringify(manifest.runtime.build))
  throw new Error('Runtime build provenance differs from source manifest');
for (const name of ['runtime.mjs', 'runtime.wasm']) {
  if (JSON.stringify(runtime.artifacts[name]) !== JSON.stringify(manifest.files[name]))
    throw new Error(`Runtime artifact provenance mismatch: ${name}`);
}
if (options.includes('--require-clean') &&
    (!manifest.correspondenceVerified || manifest.source.dirty !== false || runtime.source.dirty !== false ||
     manifest.source.revision !== runtime.source.revision ||
     JSON.stringify(manifest.source.submodules) !== JSON.stringify(runtime.source.submodules)))
  throw new Error('Publication requires a clean recursive checkout and a matching clean runtime build');
// Lifecycle diagnostics must not corrupt npm pack --json's machine-readable stdout.
console.error(`Verified ${metadata.name}@${metadata.version}: ${required.length + 1} files${options.length ? ', clean release candidate' : ''}`);
