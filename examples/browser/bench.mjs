// Node benchmark for the demo guests: runs dist/demo.bin (ARM32) and
// dist/demo_a64.bin (AArch64) through the same arm_jit.js the page uses and
// prints per-kernel times, next to the native build of the same C.
//
//   node examples/browser/bench.mjs <build>/examples/browser [runs]
import {createRequire} from 'node:module';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {systemInfo} from '../../tests/bench/environment.mjs';
import {browserKernels as KERNELS, summarizeBrowser, validateBrowserReport} from '../../tests/bench/browser_protocol.mjs';

const cli = process.argv.slice(2);
const dir = path.resolve(cli.shift() || 'build-em/examples/browser');
const runs = Number(cli.shift() || 3);
if (!Number.isInteger(runs) || runs < 1 || runs > 100) throw new Error('runs must be 1..100');
let jsonOutput = null;
if (cli.length === 2 && cli[0] === '--json' && cli[1]) jsonOutput = path.resolve(cli[1]);
else if (cli.length) throw new Error('usage: bench.mjs [browser-build-dir] [runs] [--json report.json]');
if (process.env.DYN_DUMP_DIR) throw new Error('disable DYN_DUMP_DIR during benchmark measurement');
if (jsonOutput && fs.existsSync(jsonOutput)) throw new Error(`report already exists: ${jsonOutput}`);
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const artifactFiles = ['arm_jit.js', 'arm_jit.wasm', 'demo.bin', 'demo_a64.bin', 'expected.txt'];
const hashes = Object.fromEntries(artifactFiles.map(file => [file, hash(path.join(dir, 'dist', file))]));
const require = createRequire(import.meta.url);
const createArmJit = require(path.join(dir, 'dist/arm_jit.js'));
const LOAD_BASE = 0x10000;

function runGuest(arm, bin, arch, mode) {
  const ram = arm._arm_ram();
  arm.HEAPU8.fill(0, ram, ram + arm._arm_ram_size());
  arm.HEAPU8.set(bin, ram + LOAD_BASE);
  arm._arm_console_clear();
  arm._arm_init(mode, arch);
  const t0 = performance.now();
  const haltReason = arm._arm_run(LOAD_BASE);
  if (haltReason !== 0x01000000) throw new Error(`guest fault: halt reason ${haltReason}`);
  const total = performance.now() - t0;
  const marks = Array.from(new Float64Array(arm.HEAPU8.buffer, arm._arm_marks(), arm._arm_mark_count()));
  if (marks.length !== KERNELS.length + 1 || marks.some(mark => !Number.isFinite(mark)) ||
      marks.some((mark, i) => i > 0 && mark <= marks[i - 1])) throw new Error('invalid guest timing marks');
  const len = arm._arm_console_len();
  const text = new TextDecoder().decode(arm.HEAPU8.subarray(arm._arm_console(), arm._arm_console() + len));
  return {total, kernels: marks.slice(1).map((t, i) => t - marks[i]), text};
}

const best = (rows) => {
  const kernels = rows[0].kernels || rows[0];
  return kernels.map((_, i) => Math.min(...rows.map((row) => (row.kernels || row)[i])));
};
const fmt = (ms) => ms.map((v, i) => `${KERNELS[i]} ${v.toFixed(0).padStart(5)}`).join('  ');

const arm = await createArmJit();
const expected = fs.readFileSync(path.join(dir, 'dist/expected.txt'), 'utf8');
const cases = [];
for (const [name, file, arch] of [['ARM32  ', 'demo.bin', 0], ['AArch64', 'demo_a64.bin', 1]]) {
  const binPath = path.join(dir, 'dist', file);
  const bin = fs.readFileSync(binPath);
  for (const mode of [2, 0]) {
    const rows = [];
    for (let i = 0; i < runs; i++) {
      const r = runGuest(arm, bin, arch, mode);
      if (r.text !== expected) throw new Error(`${name.trim()} mode ${mode} run ${i}: guest result mismatch`);
      rows.push({total: r.total, kernels: r.kernels, correct: true});
    }
    console.log(`${name} mode ${mode}: ${fmt(best(rows))} ms  PASS`);
    cases.push({name: name.trim().toLowerCase().replace(' ', '-'), architecture: arch === 0 ? 'a32' : 'a64',
      memoryMode: mode === 0 ? 'thunk' : 'flat-window', runs,
      samples: rows,
      summary: summarizeBrowser(rows),
    });
  }
}

const host = path.join(dir, 'demo_host');
if (fs.existsSync(host)) {
  const rows = [];
  for (let i = 0; i < runs; i++) {
    const result = spawnSync(host, {encoding: 'utf8', timeout: 60000});
    if (result.error || result.status !== 0 || result.stdout !== expected) {
      throw new Error(`native reference failed: ${result.error || result.stderr || result.status}`);
    }
    const err = result.stderr;
    const marks = err.split('\n').filter((l) => l.startsWith('mark ')).map((l) => Number(l.slice(5)));
    if (marks.length !== KERNELS.length + 1 || marks.some(mark => !Number.isFinite(mark)) ||
        marks.some((mark, i) => i > 0 && mark <= marks[i - 1])) throw new Error('invalid native timing marks');
    rows.push(marks.slice(1).map((t, j) => t - marks[j]));
  }
  console.log(`native : ${fmt(best(rows))} ms`);
}

for (const file of artifactFiles) {
  if (hash(path.join(dir, 'dist', file)) !== hashes[file]) throw new Error(`artifact changed during measurement: ${file}`);
}
{
  const report = {
    schema: 'dynarmic.js/browser-benchmark-v1', timestamp: new Date().toISOString(),
    ...systemInfo(),
    source: {directory: dir, armJitSha256: hashes['arm_jit.js'], wasmSha256: hashes['arm_jit.wasm'],
      demoA32Sha256: hashes['demo.bin'], demoA64Sha256: hashes['demo_a64.bin']},
    kernels: {lcg: 'integer ALU loop', sieve: 'guest RAM memory loop', pi: 'FP64 floating-point loop'},
    cases,
  };
  validateBrowserReport(report);
  if (jsonOutput) fs.writeFileSync(jsonOutput, JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
}
