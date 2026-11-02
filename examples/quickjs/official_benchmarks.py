#!/usr/bin/env python3
"""Prepare the pure JavaScript benchmark suite shipped with QuickJS extras.

The extras archive is pinned separately from the QuickJS engine archive.  The
generated bundle removes only the command-line harness; benchmark source and
the benchmark algorithms remain byte-for-byte from the upstream combined file.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
EXTRAS_VERSION = "2025-09-13"
EXTRAS_URL = f"https://bellard.org/quickjs/quickjs-extras-{EXTRAS_VERSION}.tar.xz"
EXTRAS_SHA256 = "7e5aa54376aad91c952b6a6f44f150856073da71973c8914f041932969e9f4e8"


def _download(cache: Path) -> Path:
    archive = cache / f"quickjs-extras-{EXTRAS_VERSION}.tar.xz"
    if not archive.exists():
        with urllib.request.urlopen(EXTRAS_URL, timeout=60) as response:
            data = response.read(16 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != EXTRAS_SHA256:
            raise SystemExit("QuickJS extras source checksum mismatch")
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != EXTRAS_SHA256:
        raise SystemExit(f"QuickJS extras source checksum mismatch: remove {archive}")
    return archive


def _extract(archive: Path, cache: Path) -> Path:
    # Keep extras separate: its archive uses the same top-level directory as
    # the engine source. Removing it must never delete the engine build input.
    cache = cache / "extras"
    cache.mkdir(parents=True, exist_ok=True)
    source = cache / f"quickjs-{EXTRAS_VERSION}"
    if source.exists():
        shutil.rmtree(source)
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            target = (cache / member.name).resolve()
            if not target.is_relative_to(cache) or member.issym() or member.islnk():
                raise SystemExit("Unsafe QuickJS extras archive member")
        bundle.extractall(cache)
    return source / "tests/bench-v8"


def prepare_bench_v8(cache: Path, dist: Path, version: str) -> dict:
    archive = _download(cache)
    bench = _extract(archive, cache)
    upstream = (bench / "combined.js").read_text()
    marker = "\n/* run_harness.js */"
    if upstream.count(marker) != 1:
        raise SystemExit("QuickJS V8 benchmark harness marker changed")
    bundle = upstream.split(marker, 1)[0] + "\n"
    if "BenchmarkSuite.suites" not in bundle or "new BenchmarkSuite" not in bundle:
        raise SystemExit("QuickJS V8 benchmark bundle is incomplete")
    declarations = list(re.finditer(r"new BenchmarkSuite\('([^']+)'", bundle))
    suites = []
    cases = []
    for index, declaration in enumerate(declarations):
        suite_name = declaration[1]
        end = declarations[index + 1].start() if index + 1 < len(declarations) else len(bundle)
        block = bundle[declaration.end():end]
        names = re.findall(r"new Benchmark\(\"([^\"]+)\"", block)
        # Some historical entries use single quotes.
        names += re.findall(r"new Benchmark\('([^']+)'", block)
        names = list(dict.fromkeys(names))
        suites.append({"id": suite_name, "title": suite_name, "benchmarks": names})
        cases.extend({"id": f"{suite_name}/{name}", "suite": suite_name, "name": name,
                      "dependencies": ["Crypto/Encrypt"] if suite_name == "Crypto" and name == "Decrypt" else []}
                     for name in names)
    if len(suites) != 8 or len(cases) != 10:
        raise SystemExit(f"Unexpected V8 benchmark inventory: {len(suites)} suites, {len(cases)} cases")
    metadata = {"version": "7", "quickjsVersion": version, "suites": suites,
                "cases": cases, "source": "tests/bench-v8/combined.js"}
    adaptations = [("var start = new Date();", "var start = globalThis.__officialV8Clock();"),
                   ("elapsed = new Date() - start;", "elapsed = globalThis.__officialV8Clock() - start;")]
    adapted = bundle
    for before, after in adaptations:
        if adapted.count(before) != 1:
            raise SystemExit("Upstream V8 clock hook no longer matches")
        adapted = adapted.replace(before, after)
    adapted += "\nglobalThis.OfficialV8Source = " + json.dumps(metadata) + ";\n"
    (dist / "official-v8-upstream.js").write_text(upstream)
    (dist / "official-v8.js").write_text(adapted)
    shutil.copyfile(HERE / "official-v8-adapter.js", dist / "official-v8-adapter.js")
    shutil.copyfile(archive, dist / "quickjs-extras-source.tar.xz")
    # DeltaBlue is GPL-2.0-or-later. The pristine archive retains its notice but
    # does not contain the license text, so ship an independent complete copy.
    shutil.copyfile(HERE / "licenses/GPL-2.0.txt", dist / "quickjs-extras-LICENSE.txt")
    inventory = {
        "schema": "dynarmic.js/official-benchmarks-v1",
        "quickjsVersion": version,
        "sources": [{"id": "quickjs-extras-bench-v8", "version": "7",
                     "archive": "quickjs-extras-source.tar.xz", "url": EXTRAS_URL,
                     "sha256": EXTRAS_SHA256,
                     "upstreamCombinedSha256": hashlib.sha256(upstream.encode()).hexdigest(),
                     "source": "tests/bench-v8/combined.js"}],
        "suites": suites,
        "cases": cases,
        "methodology": {
            "mode": "upstream", "warmupMs": 1000, "measurementBatchMs": 1000, "minimumRuns": 32,
            "harness": "Unchanged upstream RunStep and RunSingleBenchmark; only Date timing calls use the host monotonic clock",
            "standaloneDependencies": "Decrypt prepares one Encrypt input outside timing because its original suite ran Encrypt first",
            "score": "Complete suite only: 100 times reference microseconds divided by geometric mean of all case times; partial multi-case suites have no score",
            "comparison": "Each runtime receives the same official benchmark source and case. Scores are local observations and are not cross-device claims.",
        },
    }
    (dist / "official-benchmarks.json").write_text(json.dumps(inventory, indent=2) + "\n")
    return {"inventory": inventory, "archive": archive}
