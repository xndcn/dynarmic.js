#!/usr/bin/env node
import fs from 'node:fs';
import {validateCoremarkReport} from '../coremark_protocol.mjs';
const [file, option, ...extra] = process.argv.slice(2);
if (!file || extra.length || (option && option !== '--require-duration')) throw new Error('usage: validate.mjs report.json [--require-duration]');
const r = JSON.parse(fs.readFileSync(file, 'utf8'));
validateCoremarkReport(r, option === '--require-duration');
console.log(`CoreMark workload schema PASS cases=${r.cases.length} durationQualified=${r.durationQualified}`);
