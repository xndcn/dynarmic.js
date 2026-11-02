// Real-engine smoke and statistics collector. For browser measurements use the
// Pages lab; Node's engine and clock are explicitly identified in this report.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createRequire} from 'node:module';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import vm from 'node:vm';

const args = process.argv.slice(2);
if (args.includes('--help')) {
    console.log('Usage: node examples/quickjs/run.mjs [build-em/examples/browser/dist] [light|full]');
    process.exit(0);
}
const dist = path.resolve(args[0] || fileURLToPath(new URL('../../build-em/examples/browser/dist', import.meta.url)));
const profile = args[1] || 'light';
if (args.length > 2 || !['light', 'full'].includes(profile)) throw new Error('Expected a dist directory and light|full profile');
const require = createRequire(import.meta.url);
vm.runInThisContext(await readFile(path.join(dist, 'quickjs-workloads.js'), 'utf8'), {filename: 'quickjs-workloads.js'});
const expected = QuickJSBench.expected[profile];
const reports = [];

function check(runtime, result, extras) {
    assert.equal(result.profile, profile);
    assert.deepEqual(result.outputs, expected, runtime + ' correctness');
    assert.equal(result.warmupRuns, 1);
    assert.equal(result.measuredRuns, 3);
    for (const name of ['lcg', 'sieve', 'pi']) {
        assert.equal(result.samplesMs[name].length, 3);
        for (const value of [result.coldMs[name], ...result.samplesMs[name]]) {
            assert.ok(Number.isFinite(value) && value >= 0, runtime + ' timing');
        }
        const sorted = [...result.samplesMs[name]].sort((a, b) => a - b);
        assert.equal(result.summary[name].medianMs, sorted[1]);
    }
    reports.push({runtime, ...result, ...extras});
}

let start = performance.now();
const native = QuickJSBench.run(profile, () => performance.now());
check('node-js', native, {runMs: performance.now() - start});

const lines = [];
start = performance.now();
const quickjs = await require(path.join(dist, 'quickjs-wasm.js'))({print: line => lines.push(line)});
const wasmLoadMs = performance.now() - start;
start = performance.now();
assert.equal(quickjs._qjs_bench(profile === 'full' ? 1 : 0), 0, 'QuickJS Wasm exit status');
const wasmRunMs = performance.now() - start;
check('quickjs-wasm', JSON.parse(lines.join('\n')), {loadMs: wasmLoadMs, runMs: wasmRunMs});
assert.equal(quickjs._qjs_bench(2), 2, 'Invalid profiles are rejected');

const bin = new Uint8Array(await readFile(path.join(dist, 'quickjs-arm.bin')));
start = performance.now();
const arm = await require(path.join(dist, 'arm_jit.js'))();
const armLoadMs = performance.now() - start;
assert.equal(arm._arm_init(2, 0), 0);
const ram = arm._arm_ram();
assert.ok(bin.byteLength <= arm._arm_ram_size() - 0x10000);
arm.HEAPU8.fill(0, ram, ram + arm._arm_ram_size());
arm.HEAPU8.set(bin, ram + 0x10000);
new DataView(arm.HEAPU8.buffer).setUint32(ram + 0xf000, profile === 'full' ? 1 : 0, true);
arm._arm_console_clear();
start = performance.now();
assert.equal(arm._arm_run(0x10000), 0x01000000, 'QuickJS ARM normal halt');
const armRunMs = performance.now() - start;
const outputStart = arm._arm_console();
const output = new TextDecoder().decode(arm.HEAPU8.subarray(outputStart, outputStart + arm._arm_console_len()));
assert.equal(arm._arm_reg(0), 0, 'QuickJS ARM exit status: ' + output);
const raw = arm.dynarmicWasmStats || {};
const compile = raw.instances?.[String(raw.activeInstance)]?.compile || {};
check('quickjs-dynarmic', JSON.parse(output), {loadMs: armLoadMs, runMs: armRunMs, compile});
console.log(JSON.stringify({schema: 'dynarmic.js/quickjs-node-v1', engine: process.version, platform: process.platform,
    arch: process.arch, profile, verified: true, reports}, null, 2));
