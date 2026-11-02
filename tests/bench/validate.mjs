#!/usr/bin/env node
import fs from 'node:fs';
import {validateReport} from './protocol.mjs';
const [file, option] = process.argv.slice(2);
if (!file || (option && option !== '--require-profile')) throw new Error('usage: validate.mjs <report.json> [--require-profile]');
const report = JSON.parse(fs.readFileSync(file, 'utf8'));
validateReport(report, option === '--require-profile');
console.log(`benchmark schema PASS cases=${report.cases.length}`);
