#!/usr/bin/env python3
"""Recompute medians from every sample in a complete matching cohort."""
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1])
records = {side: [json.loads((root / f'{side}-{i}.json').read_text()) for i in range(1, 6)]
           for side in ['baseline', 'candidate']}
processes = json.loads((root / 'processes.json').read_text())
assert len(processes) == 10 and all(row['exitCode'] == 0 for row in processes)
reference = records['baseline'][0]
for side in records:
    for sample in records[side]:
        assert sample['node'] == reference['node'] and sample['v8'] == reference['v8']
        assert len(sample['samples']) == 8
        for row, expected in zip(sample['samples'], reference['samples']):
            for field in ['name', 'iterations', 'operationsPerIteration', 'result']:
                assert row[field] == expected[field], (side, field)
            assert row['elapsedMs'] > 0
output = []
for i in range(8):
    rows = {side: [sample['samples'][i] for sample in records[side]] for side in records}
    times = {side: statistics.median(row['elapsedMs'] for row in rows[side]) for side in rows}
    sizes = {side: rows[side][0]['bytes'] for side in rows}
    assert all(all(row['bytes'] == sizes[side] for row in rows[side]) for side in rows)
    output.append({'name': rows['baseline'][0]['name'], 'medianMs': times, 'bytes': sizes,
                   'timeChangePercent': 100 * (times['candidate'] / times['baseline'] - 1),
                   'byteChangePercent': 100 * (sizes['candidate'] / sizes['baseline'] - 1),
                   'minMs': {side: min(row['elapsedMs'] for row in rows[side]) for side in rows},
                   'maxMs': {side: max(row['elapsedMs'] for row in rows[side]) for side in rows}})
print(json.dumps(output, indent=2))
