#!/usr/bin/env python3
"""Read retained t0-gate evidence; no builds or performance runs."""
import collections
import hashlib
import json
from pathlib import Path
import re
import statistics

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
KEEP = Path.home() / '.local/share/op1-perf-keep'
GATE = KEEP / 't0-gate'
inputs = {}


def read(path):
    data = path.read_bytes()
    inputs[str(path)] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    return data.decode()


def stats(values):
    return dict(samples=values, median=statistics.median(values), min=min(values), max=max(values))


labs = {}
total = checked = 0
for directory in sorted((GATE / 'lab/results').iterdir()):
    experiment = json.loads(read(directory / 'experiment.json'))
    reference = json.loads(read(directory / 'reference.json'))['profiles']['quick']
    groups = collections.defaultdict(list)
    sessions = []
    for filename in sorted(directory.glob('session-*.json')):
        session = json.loads(read(filename))
        assert session['status'] == 'pass'
        sessions.append(dict(session=session['session'], start=session['startedAt'], end=session['finishedAt']))
        for observation in session['observations']:
            task = observation['task']
            case = task['caseId']
            assert observation['ok']
            assert observation['iterations'] == reference['iterations'][case]
            assert observation['expected'] == reference['expected'][case]
            assert observation['checksums'] == [reference['expected'][case]] * 6
            assert len(observation['samples']) == 6
            key = f"{case}/{task['runtime']}/{task.get('mode', '-')}"
            groups[key].append(statistics.median(observation['samples'][3:]))
            total += 1
            checked += 6
    paired = {}
    for case in ['integer', 'branch', 'stream', 'pointer']:
        optimized = groups.get(f'{case}/bcore/0')
        if optimized is None:
            continue
        for label, denominator in [('overDynarmic', groups.get(f'{case}/arm/-')),
                                   ('overUnpromotedCP', groups.get(f'{case}/bcore/8'))]:
            if denominator is not None:
                paired[f'{case}/{label}'] = stats([a/b for a, b in zip(optimized, denominator)])
    labs[directory.name] = dict(diagWasm=experiment['frozen'].get('diag.wasm'), sessions=sessions,
        timingsMs={k:stats(v) for k,v in groups.items()}, pairedRatios=paired)

node = collections.defaultdict(list)
name = None
for line in read(GATE / 'node-boot.log').splitlines():
    match = re.match(r'== (\d+) (\S+) load', line)
    if match:
        name = match[2]
    match = re.search(r'\[node\] boot .*wall_s=([\d.]+) rss_mb=(\d+)', line)
    if match:
        node[name].append(dict(seconds=float(match[1]), rssMiB=int(match[2])))

chrome = collections.defaultdict(list)
browser = variant = round_index = visit = None
for line in read(GATE / 'chrome.log').splitlines():
    match = re.match(r'== (.+?)\s+round (\d+) site (\d+)', line)
    if match:
        browser, round_index, site = match.groups()
        variant = {'18810':'base','18811':'candidate'}[site]
    match = re.match(r'visit (\d+):', line)
    if match:
        visit = int(match[1])
    match = re.search(r'guest 2.88 s at \+([\d.]+) s', line)
    if match:
        chrome[browser+'/'+variant].append(dict(round=int(round_index), visit=visit, seconds=float(match[1])))

warm = collections.defaultdict(list)
browser = variant = round_index = None
visit = 0
for line in read(GATE / 'chrome-w.log').splitlines():
    match = re.match(r'== (\d+)\. round (\d+) (\S+)', line)
    if match:
        browser, round_index, variant = match.groups()
        visit = 0
    match = re.search(r'guest 2.88 s at \+([\d.]+)', line)
    if match:
        warm[browser+'/'+variant].append(dict(round=int(round_index), visit=visit, seconds=float(match[1])))
        visit += 1

loop_only = collections.defaultdict(list)
for line in read(GATE / 'sweep3.log').splitlines():
    match = re.match(r'== node (\S+): wall_s=([\d.]+)', line)
    if match:
        loop_only[match[1]].append(float(match[2]))

for filename in ['README.md','wip-bcore.patch','wip-op1emu.patch','lab/host.cpp',
                 'lab/measure-lite.mjs','lab/measure-quick.mjs','sweep2.sh','sweep3.sh',
                 'native.sh','web.sh','bisect.log','sweep.log','sweep2.log']:
    read(GATE / filename)
for filename in sorted((GATE / 'commits').glob('*.patch')):
    read(filename)
for filename in ['web-cold6/notes.md','web-cold6/research.md','web-cold6/proto/proto-nolb-pieces.patch',
                 'dynarmic-cmp/README.md']:
    read(KEEP / filename)
quickjs = json.loads(read(ROOT / 'internals/measurements/2026-11-02-quickjs-bcore-wasm/comparison.json'))
qratio = quickjs['eightSuiteRatio']['median']
assert quickjs['verified']

result = dict(method='Read-only recomputation of retained records; no new performance measurement.',
    observations=total, checksums=checked, labs=labs,
    node={k:dict(wallSeconds=stats([r['seconds'] for r in v]), rssMiB=stats([r['rssMiB'] for r in v]))
          for k,v in node.items()},
    chrome={k:dict(records=v, repeatVisitSeconds=stats([r['seconds'] for r in v if r['visit']>0]))
            for k,v in chrome.items()},
    chromeWarm={k:dict(records=v, repeatVisitSeconds=stats([r['seconds'] for r in v if r['visit']>0]))
                for k,v in warm.items()},
    nodeLoopOnly={k:stats(v) for k,v in loop_only.items()},
    quickjs=dict(ratio=qratio, bcoreTimeReductionForParity=1-1/qratio,
        bcoreTimeReductionForRatio1_2=1-1.2/qratio), inputs=inputs)
(HERE / 'audit.json').write_text(json.dumps(result,indent=2)+'\n')
print('Validated retained lab observations:',total,'checksums:',checked)
for k,v in result['node'].items():print('Node',k,v)
for k,v in result['chrome'].items():print('Chrome',k,v['repeatVisitSeconds'])
for k,v in result['chromeWarm'].items():print('Chrome warm',k,v['repeatVisitSeconds'])
print('Loop-only Node:',result['nodeLoopOnly'])
print('QuickJS budget:',result['quickjs'])
