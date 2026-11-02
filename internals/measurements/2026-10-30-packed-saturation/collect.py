#!/usr/bin/env python3
"""Collect five alternating fresh-process pairs from frozen emitted modules."""
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

baseline, candidate, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=False)
processes = []
for sample in range(1, 6):
    order = ['baseline', 'candidate'] if sample % 2 else ['candidate', 'baseline']
    for side in order:
        directory = baseline if side == 'baseline' else candidate
        command = ['node', 'tests/ir/integer_edge_test.mjs', str(directory), '--bench']
        start = time.time()
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stem = output / f'{side}-{sample}'
        stem.with_suffix('.json').write_bytes(result.stdout)
        stem.with_suffix('.stderr').write_bytes(result.stderr)
        processes.append({'round': sample, 'side': side, 'command': command,
                          'startedUnix': start, 'wallSeconds': time.time() - start,
                          'exitCode': result.returncode})
        (output / 'processes.json').write_text(json.dumps(processes, indent=2) + '\n')
        if result.returncode:
            raise SystemExit(result.returncode)
        data = json.loads(result.stdout)
        assert len(data['samples']) == 8
        assert all(row['iterations'] == 5000000 for row in data['samples'])
        print(side, sample, flush=True)
metadata = {
    'platform': platform.platform(),
    'cpu': next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                if line.startswith('model name')),
    'baseRevision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'files': {}, 'modules': {},
}
for name in ['tests/ir/integer_edge_test.cpp', 'tests/ir/integer_edge_test.mjs',
             'src/dynarmic/backend/wasm/emit_wasm_packed.cpp',
             'src/dynarmic/backend/wasm/emit_wasm_saturation.cpp', __file__]:
    metadata['files'][name] = hashlib.sha256(Path(name).read_bytes()).hexdigest()
for side, directory in [('baseline', baseline), ('candidate', candidate)]:
    metadata['modules'][side] = {
        file.name: hashlib.sha256(file.read_bytes()).hexdigest()
        for file in sorted(directory.glob('bench_*.wasm'))
    }
(output / 'environment.json').write_text(json.dumps(metadata, indent=2) + '\n')
