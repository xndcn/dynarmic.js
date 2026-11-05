"""Recompute the retained RSB candidate's results without excluding samples."""
import json
import pathlib
import random
import statistics

root = pathlib.Path(__file__).resolve().parent
read = lambda name: json.loads((root / name).read_text())
median = statistics.median


def interval(values):
    rng = random.Random(0xa18df34)
    boot = sorted(median(rng.choices(values, k=len(values))) for _ in range(10000))
    return [boot[250], boot[9749]]


before, after = [read(f'paired-fixed/fixed-{v}.json') for v in ['before', 'after']]
comparison = read('paired-fixed/fixed-comparison.json')
rsb = read('rsb-calls.json')
assert rsb['status'] == 'complete'
rows = []
for a, b, gate in zip(before['cases'], after['cases'], comparison['cases']):
    assert a['name'] == b['name'] == gate['name']
    process_time = lambda case, i: median(s['ms'] for s in case['samples'] if s['process'] == i and s['phase'] == 'warm')
    ratios = [process_time(b, i) / process_time(a, i) for i in range(a['runs'])]
    metric = lambda name: next(m['ratio'] for m in gate['metrics'] if m['metric'] == name)
    rows.append({'name': a['name'], 'beforeMs': a['summary']['warmMedianMs'], 'afterMs': b['summary']['warmMedianMs'],
                 'pairedTimeRatio': median(ratios), 'pairedTimeCI95': interval(ratios),
                 'throughputRatio': metric('warmMinstrPerSec'), 'byteRatio': metric('emittedBytesMedian'),
                 'compileRatio': metric('coldCompileMedianMs'), 'gate': gate['pass']})
board = {'before': rsb['before'], 'after': rsb['after'], 'pairedTimeRatio': rsb['pairedTimeRatio'],
         'pairedTimeCI95': interval(rsb['pairedTimeRatios']), 'byteRatio': rsb['after']['bytes'] / rsb['before']['bytes']}
summary = {'fixedGate': comparison['pass'], 'fixed': rows, 'boardCalls': board,
           'failedCases': [r['name'] for r in rows if not r['gate']],
           'decision': 'RSB-only board candidate; functional checks pass and board calls improve, but the default performance gate is not passed.',
           'interval': '10000 percentile bootstrap resamples of per-round ratios, process pair as sampling unit; local screening, not steady-state certification'}
(root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
lines = ['RSB 单项候选；普通套件门槛仍未通过。负时间变化表示更快。', '',
         '| 默认负载 | before / after 热态 ms | 配对时间变化 | 95% 区间 | 协议吞吐比 | bytes 比 | compile 比 | gate |',
         '| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |']
for r in rows:
    lo, hi = r['pairedTimeCI95']
    lines.append(f"| {r['name']} | {r['beforeMs']:.4f} / {r['afterMs']:.4f} | {(r['pairedTimeRatio']-1)*100:+.1f}% | {(lo-1)*100:+.1f}%～{(hi-1)*100:+.1f}% | {r['throughputRatio']:.4f} | {r['byteRatio']:.4f} | {r['compileRatio']:.4f} | {'pass' if r['gate'] else 'FAIL'} |")
lo, hi = board['pairedTimeCI95']
lines += ['', f"board call：25 对进程，配对耗时变化 {(board['pairedTimeRatio']-1)*100:+.1f}%，95% 区间 {(lo-1)*100:+.1f}%～{(hi-1)*100:+.1f}%。",
          f"各进程热态中位数的总体中位数为 {rsb['before']['warmMs']:.3f}→{rsb['after']['warmMs']:.3f} ms；这两个总体中位数之比与配对比值是不同统计量。",
          f"emitted Wasm {rsb['before']['bytes']:.0f}→{rsb['after']['bytes']:.0f} bytes（{(board['byteRatio']-1)*100:+.1f}%），冷态均为一个模块。", '',
          '默认 gate 按原协议使用总体热态中位数计算吞吐比；配对统计不能代替 gate。',
          '两个未通过项的方差和 tiering 影响仍未解释，本轮不声明默认配置无性能回退。']
(root / 'results.md').write_text('\n'.join(lines) + '\n')
print('\n'.join(lines))
