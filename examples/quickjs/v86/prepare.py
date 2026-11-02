#!/usr/bin/env python3
"""Freeze the two runtimes and unchanged official JS inputs for comparison."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
V86_VERSION = "0.5.473"
V86_REVISION = "315b2b0b758703ef0e6cc7b4088d6068f83f3c20"
V86_URL = f"https://registry.npmjs.org/v86/-/v86-{V86_VERSION}.tgz"
V86_SHA512 = "mNHB0DXDmfxn0CrlqCmlgMR1W7aKuD+sHmSctLcTROOwpOvbE4qsAhmaE6Vy+riQfcE4PfA2xvW59/ZXsYnHNw=="


def digest(p):
    data = p.read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--arm-dist", type=Path, default=ROOT / "build-em/examples/browser/dist")
    p.add_argument("--guest", type=Path, default=ROOT / "build-v86-quickjs/guest")
    p.add_argument("--cache", type=Path, default=ROOT / "build-v86-quickjs/runtime")
    p.add_argument("--out", type=Path, default=ROOT / "build-v86-quickjs/browser-dist")
    a = p.parse_args()
    if a.out.exists():
        raise SystemExit("Output exists; use a new directory to preserve measured inputs")
    a.cache.mkdir(parents=True, exist_ok=True)
    archive = a.cache / f"v86-{V86_VERSION}.tgz"
    if not archive.exists():
        with urllib.request.urlopen(V86_URL, timeout=60) as response:
            data = response.read(32 * 1024 * 1024)
        if base64.b64encode(hashlib.sha512(data).digest()).decode() != V86_SHA512:
            raise SystemExit("v86 npm archive integrity mismatch")
        archive.write_bytes(data)
    if base64.b64encode(hashlib.sha512(archive.read_bytes()).digest()).decode() != V86_SHA512:
        raise SystemExit("v86 npm archive integrity mismatch")
    with tarfile.open(archive) as bundle:
        for entry in bundle.getmembers():
            target = (a.cache / entry.name).resolve()
            if not target.is_relative_to(a.cache.resolve()) or entry.issym() or entry.islnk():
                raise SystemExit("Unsafe npm archive entry")
        bundle.extractall(a.cache)
    package = json.loads((a.cache / "package/package.json").read_text())
    if package["version"] != "0.5.473+g315b2b0":
        raise SystemExit("Unexpected v86 package version")
    a.out.mkdir(parents=True)
    arm_names = ["arm_bench_jit.js", "arm_bench_jit.wasm", "arm-bench-build-manifest.json",
                 "quickjs-arm.bin", "quickjs-manifest.json", "official-v8.js", "official-v8-adapter.js",
                 "official-benchmarks.json", "quickjs-workloads.js"]
    for name in arm_names:
        shutil.copy2(a.arm_dist / name, a.out / name)
    for name in ["quickjs-x86.elf", "quickjs-x86-manifest.json"]:
        shutil.copy2(a.guest / name, a.out / name)
    for name in ["quickjs-LICENSE.txt", "newlib-LICENSE.txt", "GPL-3.0.txt", "GCC-Runtime-Exception-3.1.txt",
                 "quickjs-extras-LICENSE.txt"]:
        shutil.copy2(a.guest / name, a.out / name)
    for name in ["libv86.js", "v86.wasm"]:
        shutil.copy2(a.cache / "package/build" / name, a.out / name)
    shutil.copy2(a.cache / "package/LICENSE", a.out / "v86-LICENSE.txt")
    shutil.copy2(ROOT / "examples/browser/official-benchmarks-worker.js", a.out / "official-benchmarks-worker.js")
    shutil.copy2(HERE / "worker.js", a.out / "v86-worker.js")
    shutil.copy2(HERE / "runtime.js", a.out / "v86-runtime.js")
    (a.out / "index.html").write_text('<!doctype html><meta charset="utf-8"><title>QuickJS emulator comparison</title>\n')
    record = {"schema": "dynarmic.js/v86-quickjs-inputs-v1", "v86": {
        "version": package["version"], "revision": V86_REVISION, "url": V86_URL,
        "archiveIntegrity": "sha512-" + V86_SHA512, "archive": digest(archive),
        "build": "official npm release; JIT enabled; explicit v86.wasm with no fallback"},
        "artifacts": {f.name: digest(f) for f in sorted(a.out.iterdir()) if f.is_file()}}
    (a.out / "comparison-inputs.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Prepared {a.out} ({len(record['artifacts'])} frozen artifacts)")


if __name__ == "__main__":
    main()
