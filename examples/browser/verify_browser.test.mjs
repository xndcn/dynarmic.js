// Exercise the CLI with real child processes and HTTP reports, without Firefox.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

const runner = fileURLToPath(new URL('verify_browser.mjs', import.meta.url));
const fakeSource = `#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import vm from 'node:vm';
const args = process.argv.slice(2);
assert.deepEqual(args.slice(0, 3), ['--headless', '--no-remote', '--profile']);
const profile = args[3];
const url = new URL(args[4]);
const mode = fs.readFileSync('mode.txt', 'utf8');
if (mode === 'early-exit') process.exit(7);
const stylesheet = await fetch(new URL('styles.css', url));
assert.equal(stylesheet.status, 200);
assert.equal(stylesheet.headers.get('content-type'), 'text/css');
if (mode === 'ignore-term') process.on('SIGTERM', () => {});
else process.on('SIGTERM', () => {
  setTimeout(() => {
    // A real browser can recreate profile files while shutting down.
    fs.mkdirSync(profile, {recursive: true});
    fs.writeFileSync(path.join(profile, 'late-profile-write'), 'flushed');
    fs.writeFileSync('shutdown.txt', 'flushed');
    process.exit(0);
  }, 100);
});
setInterval(() => {}, 1000);
if (mode === 'quickjs') {
  assert.equal(url.searchParams.get('quickjs'), '1');
  assert.equal(url.searchParams.has('auto'), false);
}
if (mode.startsWith('official')) {
  assert.equal(url.searchParams.get('official'), 'all');
  assert.equal(url.searchParams.has('auto'), false);
}
if (mode === 'cbench') {
  assert.equal(url.searchParams.get('cbench'), 'quick');
  assert.equal(url.searchParams.has('auto'), false);
  const elf = await fetch(new URL('c-bench-x86.elf', url));
  assert.equal(elf.headers.get('content-type'), 'application/octet-stream');
}
const report = url.searchParams.get(mode === 'cbench' ? 'cbench-report' : mode.startsWith('official') ? 'official-report' : mode === 'quickjs' ? 'quickjs-report' : 'report');
const failed = mode === 'failed-report' || mode === 'official-failed';
const callbackURL = report + '?status=' + encodeURIComponent(failed ? 'FAIL|one measurement failed' : 'PASS|guest passed');
if (fs.existsSync('capture-report')) {
  const html = await (await fetch(url)).text();
  assert.ok(html.includes('<script src="/__firefox-report.js"></script>'));
  const collector = await (await fetch(new URL('/__firefox-report.js', url))).text();
  const detailed = {schema:'dynarmic.js/official-v8-comparison-v1',status:failed?'fail':'pass',cases:{
    'RegExp/RegExp':{dynarmic:failed?{status:'fail',error:'deadline exceeded after 900000 ms'}:{status:'pass',result:{sampleMs:12.5}},
      wasm:{status:'pass',result:{sampleMs:2.5}},native:{status:'pass',result:{sampleMs:0.25}}}}};
  const context = vm.createContext({fetch, URL, location:url, navigator:{userAgent:'Firefox fixture'},
    officialBenchReport:detailed,quickjsReport:{schema:'quickjs-fixture',runtimes:{native:{ok:true}}},
    cBenchReport:{schema:'cbench-fixture',observations:[{status:'pass',checksum:42}]}});
  vm.runInContext(collector, context);
  if (mode === 'official-malformed') await fetch(callbackURL,{method:'POST',body:'{'});
  else await context.fetch(new URL(callbackURL),{cache:'no-store'});
} else await fetch(callbackURL);
`;

async function runCase(mode, callback, {report = false, extra = []} = {}) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-firefox-runner-'));
  try {
    const dist = path.join(root, 'dist');
    fs.mkdirSync(dist);
    for (const name of ['index.html', 'app.js', 'site.js', 'styles.css', 'worker.js', 'arm_jit.js', 'arm_jit.wasm', 'demo.bin', 'demo_a64.bin', 'expected.txt']) {
      fs.writeFileSync(path.join(dist, name), 'fixture');
    }
    if (mode === 'quickjs') {
      for (const name of ['quickjs-app.js', 'quickjs-worker.js', 'quickjs-workloads.js',
        'quickjs-arm.bin', 'quickjs-wasm.js', 'quickjs-wasm.wasm', 'quickjs-manifest.json', 'wasm-build-manifest.json']) {
        fs.writeFileSync(path.join(dist, name), 'fixture');
      }
    }
    if (mode.startsWith('official')) {
      for (const name of ['official-benchmarks-app.js', 'official-benchmarks-worker.js', 'official-benchmarks.json',
        'official-v8.js', 'official-v8-adapter.js', 'quickjs-arm.bin', 'quickjs-wasm.js', 'quickjs-wasm.wasm',
        'quickjs-manifest.json', 'arm_bench_jit.js', 'arm_bench_jit.wasm', 'arm-bench-build-manifest.json']) {
        fs.writeFileSync(path.join(dist, name), 'fixture');
      }
    }
    if (mode === 'cbench') {
      for (const name of ['c-bench-app.js', 'c-bench-worker.js', 'c-bench-protocol.js', 'c-bench-manifest.json',
        'c-bench-reference.json', 'c-bench-source.c', 'c-bench-arm.bin', 'c-bench-x86.elf', 'c-bench.wasm',
        'c-bench-v86.js', 'c-bench-v86.wasm', 'c-bench-v86-LICENSE.txt', 'wasm-build-manifest.json']) {
        fs.writeFileSync(path.join(dist, name), 'fixture');
      }
    }
    if (mode === 'missing-app') fs.rmSync(path.join(dist, 'app.js'));
    if (mode === 'missing-styles') fs.rmSync(path.join(dist, 'styles.css'));
    fs.writeFileSync(path.join(root, 'mode.txt'), mode);
    if (report) fs.writeFileSync(path.join(root, 'capture-report'), '1');
    const browser = path.join(root, mode === 'missing-browser' ? 'missing-firefox' : 'fake-firefox.mjs');
    if (mode !== 'missing-browser') fs.writeFileSync(browser, fakeSource, {mode: 0o755});
    const child = spawn(process.execPath, [runner, browser, dist,
      ...(mode === 'cbench' ? ['--cbench'] : mode.startsWith('official') ? ['--official'] : mode === 'quickjs' ? ['--quickjs'] : []),
      ...(report ? ['--report', path.join(root, 'evidence/result.json')] : []), ...extra], {cwd: root, stdio: ['ignore', 'pipe', 'pipe']});
    let stdout = '', stderr = '';
    child.stdout.on('data', chunk => { stdout += chunk; });
    child.stderr.on('data', chunk => { stderr += chunk; });
    let timer;
    const code = await new Promise((resolve, reject) => {
      timer = setTimeout(() => { child.kill('SIGKILL'); reject(new Error('runner failed to finish within 8 seconds')); }, 8000);
      child.once('error', reject);
      child.once('close', resolve);
    }).finally(() => clearTimeout(timer));
    assert.deepEqual(fs.readdirSync(root).filter(name => name.startsWith('.firefox-profile-')), [], 'profile must be removed after browser close');
    await callback({root, code, stdout, stderr});
  } finally {
    fs.rmSync(root, {recursive: true, force: true});
  }
}

test('Firefox runner waits for profile writes on graceful shutdown before reporting success', async () => {
  await runCase('success', ({root, code, stdout, stderr}) => {
    assert.equal(code, 0, stderr);
    assert.match(stdout, /PASS Firefox Worker smoke/);
    assert.equal(fs.readFileSync(path.join(root, 'shutdown.txt'), 'utf8'), 'flushed');
  });
});

test('Firefox QuickJS mode launches only the comparison and waits for its report', async () => {
  await runCase('quickjs', ({code, stdout, stderr}) => {
    assert.equal(code, 0, stderr);
    assert.match(stdout, /PASS Firefox QuickJS comparison smoke/);
  });
});

test('Firefox official mode launches every V8 case and waits for its report', async () => {
  await runCase('official', ({code, stdout, stderr}) => {
    assert.equal(code, 0, stderr);
    assert.match(stdout, /PASS Firefox official V8 benchmark smoke/);
  });
});

test('Firefox C mode runs the three-path quick comparison and serves ELF assets', async () => {
  await runCase('cbench', ({code, stdout, stderr}) => {
    assert.equal(code, 0, stderr);
    assert.match(stdout, /PASS Firefox C benchmark comparison smoke/);
  });
});

test('Firefox runner kills a browser that ignores SIGTERM and cleans its profile', async () => {
  await runCase('ignore-term', ({code, stdout, stderr}) => {
    assert.equal(code, 0, stderr);
    assert.match(stdout, /PASS Firefox Worker smoke/);
  });
});

test('Firefox runner handles startup failure without waiting for the report timeout', async () => {
  await runCase('missing-browser', ({code, stderr}) => {
    assert.equal(code, 1);
    assert.match(stderr, /could not start Firefox:.*ENOENT/);
  });
});

test('Firefox runner checks the complete page bundle before starting a browser', async () => {
  for (const [mode, missing] of [['missing-app', 'app.js'], ['missing-styles', 'styles.css']]) {
    await runCase(mode, ({code, stderr}) => {
      assert.equal(code, 1);
      assert.ok(stderr.includes(`missing browser artifacts: ${missing}`), stderr);
    });
  }
});

test('Firefox runner handles early browser exit and cleans its profile', async () => {
  await runCase('early-exit', ({code, stderr}) => {
    assert.equal(code, 1);
    assert.match(stderr, /Firefox exited before reporting \(code=7/);
  });
});

test('Firefox runner still waits for browser closure after a failed page report', async () => {
  await runCase('failed-report', ({root, code, stderr}) => {
    assert.equal(code, 1);
    assert.match(stderr, /FAIL\|one measurement failed/);
    assert.equal(fs.readFileSync(path.join(root, 'shutdown.txt'), 'utf8'), 'flushed');
  });
});

test('Firefox retains every official runtime and its failure reason before returning nonzero', async () => {
  await runCase('official-failed', ({root, code, stderr}) => {
    assert.equal(code, 1, stderr);
    assert.match(stderr, /FAIL\|one measurement failed/);
    const evidence = JSON.parse(fs.readFileSync(path.join(root, 'evidence/result.json')));
    assert.equal(evidence.mode, 'official');
    assert.equal(evidence.outcome, 'fail');
    assert.equal(evidence.userAgent, 'Firefox fixture');
    assert.equal(evidence.report.status, 'fail');
    assert.deepEqual(evidence.report.cases['RegExp/RegExp'], {
      dynarmic: {status:'fail',error:'deadline exceeded after 900000 ms'},
      wasm: {status:'pass',result:{sampleMs:2.5}}, native: {status:'pass',result:{sampleMs:0.25}},
    });
    assert.equal(fs.readFileSync(path.join(root, 'shutdown.txt'), 'utf8'), 'flushed');
    assert.equal(fs.readFileSync(path.join(root, 'dist/index.html'), 'utf8'), 'fixture', 'served instrumentation must not alter the artifact');
  }, {report: true});
});

test('Firefox report captures success and reports malformed evidence as failure', async () => {
  for (const mode of ['official', 'official-malformed']) {
    await runCase(mode, ({root, code, stderr}) => {
      const evidence = JSON.parse(fs.readFileSync(path.join(root, 'evidence/result.json')));
      assert.equal(code, mode === 'official' ? 0 : 1, stderr);
      assert.equal(evidence.outcome, mode === 'official' ? 'pass' : 'fail');
      if (mode === 'official') assert.equal(evidence.report.cases['RegExp/RegExp'].dynarmic.result.sampleMs, 12.5);
      else assert.match(evidence.error, /Firefox report:/);
    }, {report: true});
  }
});

test('Firefox report records early process exit and rejects incomplete CLI arguments', async () => {
  await runCase('early-exit', ({root, code}) => {
    assert.equal(code, 1);
    const evidence = JSON.parse(fs.readFileSync(path.join(root, 'evidence/result.json')));
    assert.equal(evidence.outcome, 'fail');
    assert.equal(evidence.report, null);
    assert.match(evidence.error, /Firefox exited before reporting \(code=7/);
  }, {report: true});
  await runCase('success', ({code, stderr}) => {
    assert.equal(code, 1);
    assert.match(stderr, /usage:/);
  }, {extra: ['--report']});
});

test('Firefox report supports Worker, QuickJS and C modes and retains preflight failures', async () => {
  for (const mode of ['success', 'quickjs', 'cbench', 'missing-app']) {
    await runCase(mode, ({root, code, stderr}) => {
      const evidence = JSON.parse(fs.readFileSync(path.join(root, 'evidence/result.json')));
      assert.equal(code, mode === 'missing-app' ? 1 : 0, stderr);
      if (mode === 'quickjs') assert.deepEqual(evidence.report.runtimes, {native:{ok:true}});
      else if (mode === 'cbench') assert.deepEqual(evidence.report.observations, [{status:'pass',checksum:42}]);
      else assert.equal(evidence.report, null);
      if (mode === 'missing-app') assert.match(evidence.error, /missing browser artifacts: app.js/);
    }, {report: true});
  }
});
