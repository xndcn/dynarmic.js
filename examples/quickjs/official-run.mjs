// Execute each official case in a disposable Node Worker, preserving complete
// upstream timing and assertions. Per-case timeouts produce explicit failures.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir} from 'node:fs/promises';
import {createRequire} from 'node:module';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import vm from 'node:vm';
import {Worker, isMainThread, parentPort, workerData} from 'node:worker_threads';
import {createHash} from 'node:crypto';

const defaults = fileURLToPath(new URL('../../build-em/examples/browser/dist', import.meta.url));
async function runOne({dist, runtime, caseId}) {
  const loadStart = performance.now();
  const [source, adapter] = await Promise.all(['official-v8.js', 'official-v8-adapter.js'].map(name => readFile(path.join(dist, name), 'utf8')));
  const expression = `JSON.stringify(OfficialV8Bench.run(${JSON.stringify(caseId)}, {}, benchNow))`;
  const bundled = `${source}\n${adapter}\n${expression}\n`;
  const bytes = new TextEncoder().encode(bundled + '\0');
  const require = createRequire(import.meta.url);
  let execute, memory = () => null, stats = () => null;
  if (runtime === 'native') {
    const context = vm.createContext({console, benchNow: () => performance.now()});
    execute = () => vm.runInContext(bundled, context, {filename: 'official-v8.js'});
  } else if (runtime === 'wasm') {
    const lines = [];
    const module = await require(path.join(dist, 'quickjs-wasm.js'))({print: value => lines.push(String(value))});
    assert.equal(module._qjs_set_memory_limit(192 * 1024 * 1024), 0);
    const pointer = module._malloc(bytes.length);
    assert.ok(pointer > 0, 'Source allocation');
    module.HEAPU8.set(bytes, pointer);
    execute = () => {
      const status = module._qjs_eval(pointer, bytes.length - 1);
      module._free(pointer);
      assert.equal(status, 0, lines.join('\n'));
      return lines.join('\n');
    };
    memory = () => module.HEAPU8.byteLength;
  } else {
    const [module, bin] = await Promise.all([
      require(path.join(dist, 'arm_bench_jit.js'))(), readFile(path.join(dist, 'quickjs-arm.bin'))]);
    const ram = module._arm_ram(), size = module._arm_ram_size();
    const sourceAddress = size - 8 * 1024 * 1024;
    assert.ok(size >= 256 * 1024 * 1024, 'Official benchmark host needs 256 MiB guest RAM');
    assert.ok(bin.byteLength < sourceAddress - 0x10000 && bytes.length < 4 * 1024 * 1024);
    module.HEAPU8.fill(0, ram, ram + size);
    module.HEAPU8.set(bin, ram + 0x10000);
    module.HEAPU8.set(bytes, ram + sourceAddress);
    const request = new DataView(module.HEAPU8.buffer, ram + 0xf000, 24);
    [2, sourceAddress, bytes.length - 1, 192 * 1024 * 1024, size, size - 65536]
      .forEach((value, index) => request.setUint32(index * 4, value, true));
    assert.equal(module._arm_init(2, 0), 0);
    module._arm_console_clear();
    execute = () => {
      const halt = module._arm_run(0x10000);
      const text = new TextDecoder().decode(module.HEAPU8.subarray(module._arm_console(), module._arm_console() + module._arm_console_len()));
      assert.equal(halt, 0x01000000, `Guest halt: ${text}`);
      assert.equal(module._arm_reg(0), 0, `Guest status: ${text}`);
      return text;
    };
    memory = () => module.HEAPU8.byteLength;
    stats = () => {
      const raw = module.dynarmicWasmStats || {};
      return raw.instances?.[String(raw.activeInstance)]?.compile || {};
    };
  }
  const loadMs = performance.now() - loadStart, start = performance.now();
  const official = JSON.parse(execute());
  const runMs = performance.now() - start;
  assert.equal(official.caseId, caseId);
  assert.equal(official.status, 'pass');
  assert.equal(official.mode, 'upstream');
  assert.equal(official.minimumRuns, 32);
  assert.equal(official.warmupMs, 1000);
  assert.ok(official.runs.every(value => value >= 32));
  assert.ok(official.samplesMs.every(value => Number.isFinite(value) && value > 0));
  return {runtime, caseId, status: 'pass', official, loadMs, runMs, memoryBytes: memory(), stats: stats()};
}

if (!isMainThread) {
  try { parentPort.postMessage(await runOne(workerData)); }
  catch (error) { parentPort.postMessage({runtime: workerData.runtime, caseId: workerData.caseId, status: 'fail', error: error.stack || String(error)}); }
} else {
  const args = process.argv.slice(2);
  if (args.includes('--help')) {
    console.log('Usage: node examples/quickjs/official-run.mjs [dist] [--runtime all|native|wasm|dynarmic] [--case all|Suite/Case] [--timeout 300000] [--report report.json]');
    process.exit(0);
  }
  const dist = path.resolve(args[0] && !args[0].startsWith('--') ? args.shift() : defaults);
  const options = {runtime: 'all', case: 'all', timeout: '300000'};
  while (args.length) {
    const key = args.shift();
    if (!['--runtime', '--case', '--timeout', '--report'].includes(key) || !args.length) throw new Error('Unknown or missing option ' + key);
    options[key.slice(2)] = args.shift();
  }
  if (!['all', 'native', 'wasm', 'dynarmic'].includes(options.runtime)) throw new Error('Unknown runtime');
  const timeout = Number(options.timeout);
  if (!Number.isSafeInteger(timeout) || timeout < 1000 || timeout > 3600000) throw new Error('Timeout must be 1000..3600000 ms');
  const inventory = JSON.parse(await readFile(path.join(dist, 'official-benchmarks.json'), 'utf8'));
  assert.equal(inventory.schema, 'dynarmic.js/official-benchmarks-v1', 'Official inventory schema');
  assert.equal(inventory.suites?.length, 8, 'All eight official suites are required');
  assert.equal(inventory.cases?.length, 10, 'All ten official cases are required');
  const inventoryIds = inventory.cases.map(item => item.id);
  assert.equal(new Set(inventoryIds).size, 10, 'Duplicate official cases');
  assert.deepEqual(inventory.suites.flatMap(suite => suite.benchmarks.map(name => `${suite.id}/${name}`)), inventoryIds, 'Suite/case mapping');
  const cases = inventoryIds.filter(id => options.case === 'all' || options.case === id);
  if (!cases.length) throw new Error('Unknown case ' + options.case);
  const runtimes = options.runtime === 'all' ? ['native', 'wasm', 'dynarmic'] : [options.runtime];
  const artifactNames = ['official-benchmarks.json', 'official-v8.js', 'official-v8-adapter.js'];
  if (runtimes.includes('wasm')) artifactNames.push('quickjs-wasm.js', 'quickjs-wasm.wasm');
  if (runtimes.includes('dynarmic')) artifactNames.push('arm_bench_jit.js', 'arm_bench_jit.wasm', 'quickjs-arm.bin');
  const artifacts = await Promise.all(artifactNames.map(async name => {
    const bytes = await readFile(path.join(dist, name));
    return {path: name, bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex')};
  }));
  const report = {schema: 'dynarmic.js/official-v8-node-v1', engine: process.version, platform: process.platform,
    arch: process.arch, timeoutMs: timeout, methodology: inventory.methodology,
    sources: inventory.sources, artifacts, reports: []};
  async function persist() {
    if (!options.report) return;
    await mkdir(path.dirname(path.resolve(options.report)), {recursive: true});
    await writeFile(options.report, JSON.stringify(report, null, 2) + '\n');
  }
  for (const caseId of cases) for (const runtime of runtimes) {
    const result = await new Promise(resolve => {
      const worker = new Worker(new URL(import.meta.url), {workerData: {dist, runtime, caseId}});
      let finished = false;
      const finish = async result => {
        if (finished) return; finished = true; clearTimeout(timer);
        await worker.terminate(); resolve(result);
      };
      const timer = setTimeout(() => finish({runtime, caseId, status: 'fail', error: `Timeout after ${timeout} ms`}), timeout);
      worker.once('message', finish);
      worker.once('error', error => finish({runtime, caseId, status: 'fail', error: error.stack || String(error)}));
      worker.once('exit', code => { if (!finished) finish({runtime, caseId, status: 'fail', error: `Worker exited before a report (${code})`}); });
    });
    report.reports.push(result);
    console.error(`${result.status.toUpperCase()} ${runtime} ${caseId}${result.error ? ': ' + result.error.split('\n')[0] : ''}`);
    await persist();
  }
  report.verified = report.reports.every(item => item.status === 'pass');
  await persist();
  console.log(JSON.stringify(report, null, 2));
  if (!report.verified) process.exitCode = 1;
}
