import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const script = fileURLToPath(new URL('./check_instruction_counts.py', import.meta.url));
const python = process.env.PYTHON || 'python3';
const available = spawnSync(python, ['-c', 'import unicorn'], {encoding: 'utf8'});

test('actual embedded benchmark guests have independently counted instruction totals', {
  skip: available.status !== 0 ? 'optional Unicorn dependency; strict regression requires this check' : false,
}, () => {
  const child = spawnSync(python, [script], {encoding: 'utf8', timeout: 180000});
  assert.equal(child.status, 0, child.stderr || child.error?.message);
  assert.equal(child.stdout.split('\n').filter(line => line.startsWith('PASS instruction count ')).length, 6);
});

test('instruction audit rejects a false denominator and a wrong guest result', {
  skip: available.status !== 0 ? 'optional Unicorn dependency' : false,
}, () => {
  const program = `import importlib.util, sys
spec = importlib.util.spec_from_file_location('counts', sys.argv[1])
module = importlib.util.module_from_spec(spec); sys.modules[spec.name] = module; spec.loader.exec_module(module)
from pathlib import Path
from dataclasses import replace
workload = module.WORKLOADS[1]  # Thumb is short, but exercises the same verifier.
source = (Path(sys.argv[1]).resolve().parents[2] / 'tests/hosts/jit_thumb_bench.cpp').read_text()
code, count = module.load_workload(source, workload)
for supplied, guest, expected in [(count - 1, workload, 'independently executed'), (count, replace(workload, expected_result=0), 'guest result')]:
    try: module.verify_workload(code, supplied, guest)
    except ValueError as error: assert expected in str(error), error
    else: raise AssertionError('invalid benchmark was accepted')
`;
  const child = spawnSync(python, ['-c', program, script], {encoding: 'utf8', timeout: 10000});
  assert.equal(child.status, 0, child.stderr || child.error?.message);
});
