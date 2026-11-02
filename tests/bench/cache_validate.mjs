#!/usr/bin/env node
import fs from 'node:fs';
import {validateCacheReport} from './cache_protocol.mjs';

const [file, option] = process.argv.slice(2);
if (!file || (option && option !== '--require-profile')) {
  throw new Error('usage: cache_validate.mjs <report.json> [--require-profile]');
}
validateCacheReport(JSON.parse(fs.readFileSync(file, 'utf8')), option === '--require-profile');
console.log('cache benchmark schema PASS cases=3');
