#!/usr/bin/env python3
"""Record the exact runtime artifacts, toolchain and compilable source inputs."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "examples/browser"))
from package_source import PackageError, digest, source_files


def runtime_inputs(root: Path) -> dict:
    """Hash repository compiler inputs, independently of documentation/API edits.

    Directory enumeration also works in the supplied source archive without Git.
    Upstream submodules contain no generated outputs in supported builds.
    """
    paths = [root / "CMakeLists.txt"]
    for directory in ("src", "cmake", "patches", "externals/dynarmic"):
        for file in (root / directory).rglob("*"):
            if file.is_file() and ".git" not in file.relative_to(root).parts:
                paths.append(file)
    for name in ("host.cpp", "CMakeLists.txt", "write_build_manifest.py"):
        paths.append(root / "packages/dynarmic" / name)
    entries = {file.relative_to(root).as_posix(): digest(file)
               for file in sorted(set(paths))}
    identity = hashlib.sha256(json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {"sha256": identity, "files": entries}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime-js", "runtime-wasm", "output", "build-dir", "compiler", "build-type", "profile", "region-diagnostics"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    build = Path(args.build_dir).resolve()
    compiler = Path(args.compiler).resolve()
    sdk = compiler.parent
    version = (sdk / "emscripten-version.txt").read_text().strip().strip('"')
    source = {"revision": None, "dirty": None, "submodules": []}
    if (ROOT / ".git").exists():
        _, source = source_files(ROOT, build)
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if ":" in line and "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    boost_header = Path(cache["Boost_INCLUDE_DIR"]) / "boost/version.hpp"
    boost_version = re.search(r"^#define BOOST_VERSION (\d+)\s*$", boost_header.read_text(), re.M)
    if boost_version is None:
        raise PackageError("cannot identify the Boost headers used by this build")
    boost_number = int(boost_version.group(1))
    info = {
        "schema": "dynarmic.js/npm-runtime-build-v1",
        "source": source,
        "inputs": runtime_inputs(ROOT),
        "build": {
            "type": args.build_type,
            "emscriptenVersion": version,
            "cmakeVersion": subprocess.check_output([cache.get("CMAKE_COMMAND", "cmake"), "--version"], text=True).splitlines()[0],
            "boostVersion": f"{boost_number // 100000}.{boost_number // 100 % 1000}.{boost_number % 100}",
            "boostVersionHeader": digest(boost_header),
            "compiler": subprocess.check_output([str(compiler), "--version"], text=True).splitlines()[0],
            "profile": args.profile.upper() in ("1", "ON", "TRUE"),
            "regionDiagnostics": args.region_diagnostics.upper() in ("1", "ON", "TRUE"),
            "a64RegionFocus": cache.get("DYNARMIC_JS_A64_REGION_FOCUS", ""),
            "flags": {key: value for key, value in cache.items()
                      if key.startswith(("CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS")) and not key.endswith("-ADVANCED")},
            "emccCflags": os.environ.get("EMCC_CFLAGS", ""),
        },
        "artifacts": {
            "runtime.mjs": digest(Path(args.runtime_js)),
            "runtime.wasm": digest(Path(args.runtime_wasm)),
        },
    }
    Path(args.output).write_text(json.dumps(info, indent=2) + "\n")


if __name__ == "__main__":
    main()
