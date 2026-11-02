// Exercise the real shell gate with deterministic subprocesses. These fixtures
// test orchestration, retention and failure propagation, not guest correctness.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtempSync, mkdirSync, writeFileSync, readFileSync, readdirSync, rmSync, existsSync, symlinkSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

const regression = fileURLToPath(new URL('../regress.sh', import.meta.url));
function write(path, content) { mkdirSync(join(path, '..'), {recursive: true}); writeFileSync(path, content, {mode: 0o755}); }
function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'dynarmic-regression test-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  const em = join(root, 'em'), native = join(root, 'native'), bin = join(root, 'bin');
  const artifacts = join(root, 'retained results');
  mkdirSync(bin); mkdirSync(join(em, 'tests'), {recursive: true});
  write(join(em, 'examples/browser/dist/arm_jit.js'), '');
  write(join(em, 'examples/browser/dist/c-bench-reference.json'), '{"fixture":true}\n');
  write(join(native, 'tests/guest64/test.bin'), '');
  const nativeHost = `#!/bin/bash
case "$(basename "$0")" in
  host_ref) for i in {0..5}; do echo "stage$i 123"; done ;;
  fp_ref) echo 123 ;;
  *) printf mix > ref_mix.log; printf mix > ref2_mix.log; printf mix > ref3_mix.log; printf mix > qc_ref.log ;;
esac
[ "$(basename "$0")" != "\${FAIL_REFERENCE:-}" ] || exit 19
`;
  for (const name of ['region_plan_test', 'arm_decoder_test', 'smoke_ir', 'sat64_ir', 'wasm_binary_test', 'tools/block_dump',
    ...['host_ref', 'fp_ref', 'neon4_ref', 'neon5_ref', 'neon6_ref', 'fp16_ref', 'media_ref_log', 'neon_ref_log',
      'neon2_ref_log', 'neon3_ref_log', 'qc_ref'].map(name => `guest/${name}`)]) write(join(native, 'tests', name), nativeHost);
  write(join(bin, 'node'), `#!/bin/bash
host="$(basename -- "$1")"
if [ "$host" = "\${FAIL_HOST:-}" ]; then echo PASS; echo 'deliberate host failure' >&2; exit 17; fi
if [[ "$host" == *_runbin.js ]]; then
  guest="$(basename "$2")"
  echo PASS-marker
  if [[ "$guest" == test* ]]; then
    for i in {0..5}; do echo "result$i 123"; done
  else echo 'result0 123'; fi
  [[ "$guest" != *_log.bin ]] || printf mix > guest_mix.log
else
  printf 'PASS\\nPASS\\nPASS\\nPASS\\n'
fi
while [ "$#" -gt 1 ]; do
  if [ "$1" = --output ] || [ "$1" = --json ]; then printf '{}' > "$2"; fi
  shift
done
`);
  write(join(bin, 'python3'), `#!/bin/bash
if [ "$1" = -c ]; then [ "\${MISSING_FUZZ:-0}" = 0 ]; exit $?; fi
seed=''; corpus=''; output=''
while [ "$#" -gt 1 ]; do
  [ "$1" != --seed ] || seed="$2"
  [ "$1" != --save-corpus ] || corpus="$2"
  [ "$1" != --output ] || output="$2"
  shift
done
[ -z "$output" ] || printf '{"fixture":true}\\n' > "$output"
echo "fuzz seed=$seed"
if [ -n "$seed" ] && [ "$seed" = "\${FAIL_SEED:-}" ]; then
  mkdir -p "$corpus"
  printf '{"seed":%s}\\n' "$seed" > "$corpus/case.json"
  echo "mismatch seed=$seed" >&2
  exit 1
fi
`);
  return {root, em, native, artifacts, bin};
}
function run(data, {strict = true, retain = true, environment = {}, coremark = null} = {}) {
  const env = {...process.env, PATH: `${data.bin}:${process.env.PATH}`, ...environment};
  delete env.DYNARMIC_JS_TEST_ARTIFACTS;
  if (retain) env.DYNARMIC_JS_TEST_ARTIFACTS = data.artifacts;
  const result = spawnSync('bash', [regression, ...(strict ? ['--strict'] : []),
    ...(coremark ? ['--coremark', coremark] : []), data.em, data.native],
    {encoding: 'utf8', env, cwd: data.root, timeout: 30000});
  assert.equal(result.error, undefined, result.error?.message);
  return result;
}
function commands(directory) {
  return readdirSync(join(directory, 'commands')).map(name => {
    const root = join(directory, 'commands', name);
    return {root, argv: readFileSync(join(root, 'command.sh'), 'utf8'),
      status: Number(readFileSync(join(root, 'exit-code.txt'), 'utf8'))};
  });
}

test('strict successful regression retains unique guest, command and fuzz evidence', t => {
  const data = fixture(t), result = run(data);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(readFileSync(join(data.artifacts, 'run.txt'), 'utf8'), /strict=1[\s\S]*exit_code=0/);
  const all = commands(data.artifacts), fuzz = all.filter(row => row.argv.includes('/fuzz.py'));
  assert.equal(fuzz.length, 21);
  assert.equal(new Set(fuzz.map(row => row.argv.match(/--save-corpus (.*?)\s*$/)[1])).size, 21);
  assert(fuzz.every(row => /--seed \d+/.test(row.argv) && row.status === 0));
  for (const row of fuzz) {
    const maximum = row.argv.includes('/a64/fuzz.py') ? '0.05' : row.argv.includes('--thumb') ? '0.10' : '0.30';
    assert(row.argv.includes(`--max-unsupported-rate ${maximum}`), row.argv);
  }
  assert.equal(readdirSync(join(data.artifacts, 'fuzz')).filter(name => name.endsWith('.log')).length, 21);
  assert.equal(readdirSync(join(data.artifacts, 'guests')).filter(name => name.endsWith('.mix.log')).length, 27);
  assert.match(readFileSync(join(data.artifacts, 'guests/a32-test-mode0.log'), 'utf8'), /result5 123/);
  assert.match(readFileSync(join(data.artifacts, 'guests/a64-fp-mode2.log'), 'utf8'), /result0 123/);
  assert(all.some(row => row.argv.includes('run.mjs') && row.argv.includes('--runs 5')));
});

test('an early fuzz mismatch and replay corpus survive later successful campaigns', t => {
  const data = fixture(t), result = run(data, {environment: {FAIL_SEED: '70'}});
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(readFileSync(join(data.artifacts, 'fuzz/a64-straight-mode0-seed70.log'), 'utf8'), /mismatch seed=70/);
  assert.deepEqual(JSON.parse(readFileSync(join(data.artifacts, 'fuzz/a64-straight-mode0-seed70.corpus/case.json'))), {seed: 70});
  assert.match(readFileSync(join(data.artifacts, 'fuzz/a64-straight-mode2-seed72.log'), 'utf8'), /fuzz seed=72/);
  const failed = commands(data.artifacts).filter(row => row.status !== 0);
  assert.equal(failed.length, 1);
  assert.match(failed[0].argv, /--seed 70/);
  assert.match(readFileSync(join(failed[0].root, 'stderr.log'), 'utf8'), /mismatch seed=70/);
  assert.match(readFileSync(join(data.artifacts, 'run.txt'), 'utf8'), /exit_code=1/);
});

test('a PASS marker never masks nonzero process exit, and stderr is retained', t => {
  const data = fixture(t), result = run(data, {environment: {FAIL_HOST: 'jit_demo.js'}});
  assert.equal(result.status, 1, result.stdout + result.stderr);
  const failed = commands(data.artifacts).find(row => row.status === 17);
  assert(failed);
  assert.match(readFileSync(join(failed.root, 'stdout.log'), 'utf8'), /PASS/);
  assert.match(readFileSync(join(failed.root, 'stderr.log'), 'utf8'), /deliberate host failure/);
});

test('strict missing fuzz dependencies fail while ordinary regression explicitly skips', t => {
  const strict = fixture(t), ordinary = fixture(t);
  const a = run(strict, {environment: {MISSING_FUZZ: '1'}});
  const b = run(ordinary, {strict: false, environment: {MISSING_FUZZ: '1'}});
  assert.equal(a.status, 1, a.stdout + a.stderr);
  assert.match(a.stdout, /FAIL fuzz dependencies missing/);
  assert.equal(b.status, 0, b.stdout + b.stderr);
  assert.match(b.stdout, /SKIP A64 \/ A32 fuzz/);
  assert.match(readFileSync(join(ordinary.artifacts, 'skips.txt'), 'utf8'), /A64 \/ A32 fuzz/);
});

test('native reference failures cannot pass using otherwise matching checksum output', t => {
  for (const name of ['host_ref', 'fp_ref']) {
    const data = fixture(t), result = run(data, {environment: {FAIL_REFERENCE: name}});
    assert.equal(result.status, 1, result.stdout + result.stderr);
    assert.match(result.stdout, /native reference/);
    assert(commands(data.artifacts).some(row => row.status === 19 && row.argv.includes(name)));
  }
});

test('a stale browser C benchmark reference fails the regression artifact check', t => {
  const data = fixture(t);
  write(join(data.em, 'examples/browser/dist/c-bench-reference.json'), '{"fixture":false}\n');
  const result = run(data);
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(result.stdout, /FAIL c_bench_reference_artifact/);
});

test('explicit CoreMark evidence remains inside the retained regression directory', t => {
  const data = fixture(t), workload = join(data.root, 'coremark workload');
  mkdirSync(workload);
  const result = run(data, {coremark: workload});
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert(existsSync(join(data.artifacts, 'coremark/report.json')));
  assert(commands(data.artifacts).some(row => row.argv.includes('/coremark/run.mjs')));
});

test('artifact output rejects existing directories and symlinks without changing contents', t => {
  for (const symlink of [false, true]) {
    const data = fixture(t);
    const target = symlink ? join(data.root, 'target') : data.artifacts;
    mkdirSync(target); writeFileSync(join(target, 'sentinel'), 'untouched');
    if (symlink) symlinkSync(target, data.artifacts);
    const result = run(data);
    assert.equal(result.status, 2);
    assert.match(result.stderr, /Artifact directory must be new/);
    assert.deepEqual(readdirSync(target), ['sentinel']);
    assert.equal(readFileSync(join(target, 'sentinel'), 'utf8'), 'untouched');
  }
});

test('temporary successful runs clean up and failed runs retain diagnostics', t => {
  for (const failing of [false, true]) {
    const data = fixture(t), temporary = join(data.root, 'temporary');
    mkdirSync(temporary);
    const result = run(data, {retain: false, environment: {TMPDIR: temporary, ...(failing ? {FAIL_SEED: '70'} : {})}});
    assert.equal(result.status, failing ? 1 : 0, result.stdout + result.stderr);
    const retained = readdirSync(temporary);
    assert.equal(retained.length, failing ? 1 : 0);
    if (failing) assert(existsSync(join(temporary, retained[0], 'fuzz/a64-straight-mode0-seed70.corpus/case.json')));
  }
});
