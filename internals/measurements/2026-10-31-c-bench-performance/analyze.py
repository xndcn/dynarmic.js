#!/usr/bin/env python3
"""Summarize independent browser sessions, retaining within-round pairing."""
from pathlib import Path
import json, statistics, math, csv, sys
root = Path(sys.argv[1]); out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=False)
spec = json.loads((root / 'experiment.json').read_text())
records = [json.loads((root / f'session-{i}.json').read_text()) for i in range(spec['sessions'])]
assert len(records) == 5 and all(r['status'] == 'pass' for r in records)
assert len({r['userAgent'] for r in records}) == 1
cases = [r['id'] for r in spec['reference']['cases']]
runtimes = ['dynarmic', 'v86', 'wasm']

def stats(values):
    return {'samples': values, 'median': statistics.median(values), 'min': min(values), 'max': max(values)}

def geom(values):
    return math.exp(sum(math.log(x) for x in values) / len(values))

result = {'schema': 'dynarmic.js/c-bench-session-summary-v1', 'environment': spec['environment'],
          'browser': records[0]['userAgent'], 'sessions': len(records), 'observations': 0,
          'measuredCalls': 0, 'checkedCalls': 0, 'verified': True, 'cases': {}, 'overall': {},
          'methodology': 'Within each session summarize three round medians; paired round ratios precede session medians. Final median/range describes five independent browser-session summaries.'}
rows = []
sessions = []
for record in records:
    r = record['report']; assert r['verified'] and r['performanceValid'] and not r['environment']['visibilityChanged']
    observations = r['observations']; assert len(observations) == 45
    lookup = {(e['task']['caseId'], e['task']['runtime'], e['task']['round']): e for e in observations}
    assert len(lookup) == 45
    local = {}; round_ratios = {'v86OverDynarmic': [[] for _ in range(3)], 'dynarmicOverWasm': [[] for _ in range(3)]}
    for case in cases:
        item = {'runtime': {}, 'ratio': {}}
        for runtime in runtimes:
            samples = []
            for round in range(3):
                entry = lookup[(case, runtime, round)]; assert entry['status'] == 'pass'
                value = entry['result']; config = spec['reference']['profiles']['full']
                assert value['iterations'] == config['iterations'][case] and value['seed'] == config['seed']
                assert value['checksums'] == [config['expected'][case]] * 6
                assert len(value['samplesMs']) == 3 and len(value['warmupMs']) == 2
                assert all(math.isfinite(t) and t >= 0 for t in [value['loadMs'], value['firstMs'], *value['warmupMs'], *value['samplesMs']])
                median = statistics.median(value['samplesMs']); assert median >= .1
                samples.append({'warmMs': median, 'firstMs': value['firstMs'], 'loadMs': value['loadMs']})
                rows.append([record['session'], round, case, runtime, value['iterations'], median, value['firstMs'], value['loadMs'], *value['samplesMs'], *value['warmupMs']])
                result['observations'] += 1; result['measuredCalls'] += 3; result['checkedCalls'] += 6
            item['runtime'][runtime] = {metric: statistics.median([s[metric] for s in samples]) for metric in ['warmMs', 'firstMs', 'loadMs']}
        for name, numerator, denominator in [('v86OverDynarmic','v86','dynarmic'), ('dynarmicOverWasm','dynarmic','wasm')]:
            values = []
            for round in range(3):
                v = statistics.median(lookup[(case,numerator,round)]['result']['samplesMs']) / statistics.median(lookup[(case,denominator,round)]['result']['samplesMs'])
                values.append(v); round_ratios[name][round].append(v)
            item['ratio'][name] = statistics.median(values)
        local[case] = item
    sessions.append({'session': record['session'], 'cases': local,
        'overall': {name: statistics.median([geom(v) for v in rounds]) for name, rounds in round_ratios.items()},
        'roundOverallRatios': {name: [geom(v) for v in rounds] for name, rounds in round_ratios.items()}})
for case in cases:
    result['cases'][case] = {'iterations': spec['reference']['profiles']['full']['iterations'][case],
        'runtimes': {runtime: {metric: stats([s['cases'][case]['runtime'][runtime][metric] for s in sessions]) for metric in ['warmMs','firstMs','loadMs']} for runtime in runtimes},
        'ratios': {name: stats([s['cases'][case]['ratio'][name] for s in sessions]) for name in ['v86OverDynarmic','dynarmicOverWasm']}}
for name in ['v86OverDynarmic','dynarmicOverWasm']:
    result['overall'][name] = stats([s['overall'][name] for s in sessions])
result['sessionSummaries'] = sessions
(out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
with (out / 'samples.csv').open('w', newline='') as f:
    writer=csv.writer(f, lineterminator="\n"); writer.writerow(['session','round','case','runtime','iterations','warmMedianMs','firstMs','loadMs','measured1','measured2','measured3','warmup1','warmup2']);writer.writerows(rows)
print(json.dumps({'observations':result['observations'],'checkedCalls':result['checkedCalls'],'overall':result['overall']},indent=2))
