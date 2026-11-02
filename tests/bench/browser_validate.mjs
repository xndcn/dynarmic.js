#!/usr/bin/env node
import fs from 'node:fs';
import {validateBrowserReport} from './browser_protocol.mjs';

const [file] = process.argv.slice(2);
if (!file) throw new Error('usage: browser_validate.mjs <report.json>');
validateBrowserReport(JSON.parse(fs.readFileSync(file, 'utf8')));
console.log('browser benchmark schema PASS');
