#!/usr/bin/env node
import fs from 'node:fs';
import {validatePopulationReport} from './cache_population_protocol.mjs';

const args = process.argv.slice(2);
const [file, option] = args;
if (!file || args.length > 2 || (option && option !== '--require-profile')) {
  throw new Error('usage: cache_population_validate.mjs <report.json> [--require-profile]');
}
validatePopulationReport(JSON.parse(fs.readFileSync(file, 'utf8')), option === '--require-profile');
console.log('cache population benchmark schema PASS cases=12');
