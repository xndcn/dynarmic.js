#!/usr/bin/env python3
"""Build matched static x86-64/AArch64 Linux CoreMark binaries from pinned inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str], cwd: Path) -> str:
    environment = os.environ.copy()
    environment.pop("PYTHONPATH", None)
    environment.pop("PYTHONHOME", None)
    result = subprocess.run(command, cwd=cwd, env=environment, check=False,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n"
                           f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result.stdout.strip()


def checked_source(path: Path, expected: str) -> dict[str, Any]:
    actual = sha256(path)
    if actual != expected:
        raise ValueError(f"source hash mismatch: {path}: {actual} != {expected}")
    return {"path": str(path), "sha256": actual}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="clean pinned CoreMark checkout")
    parser.add_argument("workload", type=Path, help="directory containing manifest.json and adapted ee_printf.c")
    parser.add_argument("output", type=Path, help="new output directory")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    workload = args.workload.resolve(strict=True)
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"refusing to overwrite output directory: {output}")
    output.mkdir(parents=True)

    manifest_path = workload / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if manifest["schema"] != "dynarmic.js/coremark-build-v1" or manifest["iterations"] != 200000:
        raise ValueError("unexpected workload manifest")
    revision = run(["git", "-C", str(source), "rev-parse", "HEAD"], output)
    if revision != manifest["revision"] or run(["git", "-C", str(source), "status", "--short"], output):
        raise ValueError("CoreMark checkout is not at the pinned clean revision")

    upstream_names = ["core_list_join.c", "core_main.c", "core_matrix.c", "core_state.c",
                      "core_util.c", "barebones/cvt.c"]
    sources = [source / name for name in upstream_names]
    sources += [workload / "ee_printf.c",
                Path(__file__).resolve().parents[4] / "tests/bench/coremark/core_portme.c"]
    source_records = [checked_source(source / name, manifest["sources"][f"upstream/{name}"])
                      for name in upstream_names]
    source_records += [checked_source(source / "coremark.h", manifest["sources"]["upstream/coremark.h"]),
                       checked_source(workload / "ee_printf.c", manifest["uartSourceSha256"]),
                       checked_source(sources[-1], manifest["sources"]["port/core_portme.c"]),
                       checked_source(sources[-1].with_suffix(".h"), manifest["sources"]["port/core_portme.h"])]
    port_include = sources[-1].parent
    compiler = [sys.executable, "-m", "ziglang", "cc"]
    compiler_version = run(compiler + ["--version"], output)
    artifacts: dict[str, Any] = {}
    commands: dict[str, list[str]] = {}
    for implementation, target, filename in [
        ("x86Native", "x86_64-linux-musl", "coremark_x86_64"),
        ("qemuUser", "aarch64-linux-musl", "coremark_aarch64"),
    ]:
        flags = f"-target {target} -O2 -static"
        destination = output / filename
        command = compiler + ["-target", target, "-O2", "-static",
                              f'-DCOMPILER_FLAGS="{flags}"',
                              f'-DITERATIONS={manifest["iterations"]}',
                              f'-DTOTAL_DATA_SIZE={manifest["dataSize"]}', "-DPERFORMANCE_RUN=1",
                              "-DMAIN_HAS_NOARGC=1", f"-I{port_include}", f"-I{source}", "-DHOST"]
        command += [str(path) for path in sources] + ["-o", str(destination)]
        run(command, output)
        commands[implementation] = command
        artifacts[implementation] = {"file": filename, "path": str(destination),
                                     "target": target, "flags": flags,
                                     "sha256": sha256(destination), "bytes": destination.stat().st_size}

    result = {
        "schema": "dynarmic.js/native-coremark-linux-build-v1",
        "revision": revision,
        "iterations": manifest["iterations"],
        "dataSize": manifest["dataSize"],
        "contexts": manifest["contexts"],
        "timer": "tests/bench/coremark/core_portme.c HOST: CLOCK_MONOTONIC, wrapping u32 microseconds",
        "compiler": compiler_version,
        "baseManifest": {"path": str(manifest_path), "sha256": sha256(manifest_path)},
        "sources": source_records,
        "commands": commands,
        "artifacts": artifacts,
    }
    (output / "linux-manifest.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
