#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {install} from './fixture.mjs';
const [tarball, ...extra] = process.argv.slice(2);
if (!tarball || (extra.length && !(extra.length === 2 && extra[0] === '--report')))
  throw new Error('Usage: node tests/npm/smoke.mjs package.tgz [--report result.json]');
const fixture = install(tarball);
try {
  const result = execFileSync(process.execPath, [path.join(fixture.root, 'consumer.mjs')],
    {cwd: fixture.root, encoding: 'utf8', timeout: 90000, maxBuffer: 1024 * 1024});
  const report = JSON.parse(result);
  if (report.status !== 'pass') throw new Error('consumer did not pass');
  report.tarballSha256 = fixture.tarballSha256;
  const json = JSON.stringify(report, null, 2) + '\n';
  if (extra.length) fs.writeFileSync(extra[1], json);
  console.log(json.trim());
} finally { fixture.cleanup(); }
