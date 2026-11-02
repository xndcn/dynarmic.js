#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {validateNativeReport} from './native_protocol.mjs';

const [file, option, extra] = process.argv.slice(2);
assert(file && !extra && (!option || option === '--require-comparison'),
  'usage: native_validate.mjs <report.json> [--require-comparison]');
const report = validateNativeReport(JSON.parse(fs.readFileSync(file, 'utf8')), option === '--require-comparison');
console.log(`native comparison PASS cases=${report.cases.length} pairs=${report.runs} qualification=${report.qualification}`);
