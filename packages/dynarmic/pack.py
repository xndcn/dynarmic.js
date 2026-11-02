#!/usr/bin/env python3
"""Stage a dependency-free npm package and its corresponding recursive source.

This command never invokes npm publish or contacts a package registry.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

from write_build_manifest import ROOT, runtime_inputs
from package_source import (EXCLUDED_ARCHIVE_PREFIXES, PackageError, digest,
                            distributable_paths, runtime_notices, source_files, write_archive)

PUBLIC_FILES = (
    "package.json", "index.js", "index.d.ts", "README.md", "verify.mjs",
)
GENERATED_FILES = ("runtime.mjs", "runtime.wasm", "runtime-build-manifest.json")


def distributable_sources(output: Path) -> tuple[list[Path], dict]:
    selected, source = source_files(ROOT, output)
    # Historical performance observations are not compiler inputs. Keep the
    # runtime, public tests, build scripts and recursive dependencies complete.
    return distributable_paths(selected), source


def check_runtime(build: Path, require_clean: bool) -> dict:
    runtime = build / "packages/dynarmic"
    manifest = json.loads((runtime / "runtime-build-manifest.json").read_text())
    if manifest.get("schema") != "dynarmic.js/npm-runtime-build-v1":
        raise PackageError("unsupported runtime build manifest schema")
    if manifest["inputs"] != runtime_inputs(ROOT):
        raise PackageError("runtime compiler inputs changed; rebuild dynarmic_package_runtime before packaging")
    for name in ("runtime.mjs", "runtime.wasm"):
        if digest(runtime / name) != manifest["artifacts"][name]:
            raise PackageError(f"{name} does not match its build manifest; rebuild the runtime")
    configuration = manifest["build"]
    if configuration["type"] != "Release" or configuration["profile"] or configuration["regionDiagnostics"] or configuration["a64RegionFocus"]:
        raise PackageError("npm packages require a Release runtime with profiling, region diagnostics and A64 region focus disabled")
    if require_clean and manifest["source"]["dirty"] is not False:
        raise PackageError("publication requires a runtime linked from a clean checkout; rebuild after committing")
    return manifest


def locate_sdk(build: Path, explicit: Path | None) -> Path:
    if explicit:
        return explicit.resolve()
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith("CMAKE_TOOLCHAIN_FILE:"):
            toolchain = Path(line.split("=", 1)[1]).resolve()
            if toolchain.name == "Emscripten.cmake":
                return toolchain.parents[3]
    raise PackageError("cannot locate the runtime SDK; pass --emscripten-root /path/to/emscripten")


def verify_archive(archive_path: Path, expected: dict) -> None:
    """Bind actual archived bytes to the runtime inputs and shipped wrapper.

    Checking the working tree before and after writing is insufficient: Git
    ignores some files, and a file may change and revert during archiving.
    """
    with tarfile.open(archive_path, "r:gz") as archive:
        members = archive.getmembers()
        if len({member.name for member in members}) != len(members):
            raise PackageError("source archive contains duplicate entries")
        for relative, identity in expected.items():
            try:
                stream = archive.extractfile("dynarmic.js/" + relative)
            except (KeyError, OSError) as error:
                raise PackageError(f"source archive omits compiler/package input: {relative}") from error
            if stream is None:
                raise PackageError(f"source archive input is not a file: {relative}")
            checksum = hashlib.sha256()
            size = 0
            with stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    checksum.update(chunk)
                    size += len(chunk)
            if {"sha256": checksum.hexdigest(), "bytes": size} != identity:
                raise PackageError(f"source archive input differs from runtime/package: {relative}")


def stage(build: Path, output: Path, require_clean: bool, sdk: Path | None) -> dict:
    build, output = build.resolve(), output.resolve()
    if output == ROOT or output in ROOT.parents or output == build:
        raise PackageError("output must be a dedicated package staging directory")
    if output.exists() and not (output / "source-manifest.json").is_file():
        raise PackageError("refusing to replace an existing directory without a source-manifest.json")
    manifest = check_runtime(build, require_clean)
    selected, source = distributable_sources(output)
    if source["revision"] != manifest["source"]["revision"] or source["submodules"] != manifest["source"]["submodules"]:
        raise PackageError("runtime revisions differ from the source checkout; rebuild the runtime")
    if require_clean and source["dirty"]:
        raise PackageError("publication requires a clean recursive checkout; commit changes before packaging")
    for required in ("CMakeLists.txt", "packages/dynarmic/host.cpp", "packages/dynarmic/index.js",
                     "externals/dynarmic/CMakeLists.txt", "externals/dynarmic/LICENSE.txt"):
        if Path(required) not in selected:
            raise PackageError(f"source archive omits {required}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".npm-stage-", dir=output.parent) as temporary:
        temp = Path(temporary)
        for name in PUBLIC_FILES:
            shutil.copyfile(ROOT / "packages/dynarmic" / name, temp / name)
        for name in GENERATED_FILES:
            shutil.copyfile(build / "packages/dynarmic" / name, temp / name)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copyfile(ROOT / name, temp / name)
        notices, notices_info = runtime_notices(ROOT, locate_sdk(build, sdk), manifest)
        notices = notices.replace("dynarmic.js browser runtime", "dynarmic.js npm runtime", 1)
        # Keep linked dependency licenses readable without unpacking the archive.
        for name in ("externals/dynarmic/LICENSE.txt", "externals/dynarmic/externals/fmt/LICENSE.rst",
                     "externals/dynarmic/externals/mcl/LICENSE", "externals/dynarmic/externals/robin-map/LICENSE"):
            notices += f"\n{'=' * 72}\n{name}\n{'=' * 72}\n" + (ROOT / name).read_text()
        (temp / "runtime-licenses.txt").write_text(notices)
        version = manifest["build"]["emscriptenVersion"]
        (temp / "REBUILD.md").write_text(f"""# Rebuild the supplied source

This package contains the complete runtime and recursive submodule sources in
`source.tar.gz`, rooted at `dynarmic.js/`. It excludes Git metadata and generated
outputs, plus historical experiments in `internals/measurements/` and
`internals/release-checks/`. These are not runtime build inputs. Public tests,
examples, documentation and build scripts are included; archived internal
evidence checks require a Git checkout. The source manifest records both
runtime and archive hashes.

Install Emscripten **{version}**, Python 3, CMake >= 3.16, a C++20 toolchain,
GNU patch and Boost >= 1.57 headers. Put Boost headers outside `/usr/include`
for the Emscripten build. No ARM guest compiler is required for this package.

```sh
tar -xzf source.tar.gz
cd dynarmic.js
. /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-package -DCMAKE_BUILD_TYPE=Release \\
  -DBoost_INCLUDE_DIR=/path/to/boost-headers \\
  -DDYNARMIC_JS_TESTS=OFF -DDYNARMIC_JS_EXAMPLES=OFF \\
  -DDYNARMIC_JS_NPM_PACKAGE=ON
cmake --build build-package --target dynarmic_package_runtime -j2
```

The results are `build-package/packages/dynarmic/runtime.mjs` and `runtime.wasm`.
Use them together with `packages/dynarmic/index.js` and `index.d.ts` from the
archive. Compiler/system dependency versions and flags are recorded in
`runtime-build-manifest.json`; portable rebuilds need not be byte-identical.
Its input inventory hashes repository compiler inputs. External SDK and Boost
trees are identified by versions, not represented as fully hashed source trees.
Rebuilding does not require Git. Preparing a new publishable package requires
a Git checkout so its release revision and cleanliness can be verified.

`node verify.mjs` checks every distributed file against `source-manifest.json`.
`node verify.mjs --require-clean` additionally rejects local dirty snapshots.
Checksums detect accidental mismatch; they are not a signature or proof of trust.
The package's JavaScript API is experimental ({json.loads((temp / 'package.json').read_text())['version']}).
""")
        write_archive(ROOT, selected, temp / "source.tar.gz")
        archived_inputs = dict(manifest["inputs"]["files"])
        archived_inputs.update({"packages/dynarmic/" + name: digest(temp / name) for name in PUBLIC_FILES})
        archived_inputs.update({name: digest(temp / name) for name in ("LICENSE", "THIRD_PARTY_NOTICES.md")})
        verify_archive(temp / "source.tar.gz", archived_inputs)
        after_selected, after_source = distributable_sources(output)
        if selected != after_selected or source != after_source or manifest != check_runtime(build, require_clean):
            raise PackageError("source or runtime changed while packaging; retry when edits finish")
        # Bind the wrapper and other copied sources to the archive snapshot too.
        for name in PUBLIC_FILES:
            if digest(ROOT / "packages/dynarmic" / name) != digest(temp / name):
                raise PackageError(f"{name} changed while packaging")
        files = {file.name: digest(file) for file in sorted(temp.iterdir())}
        report = {
            "schema": "dynarmic.js/npm-source-v1",
            "package": {key: json.loads((temp / "package.json").read_text())[key] for key in ("name", "version")},
            "source": source,
            "runtime": {"source": manifest["source"], "build": manifest["build"], "inputSha256": manifest["inputs"]["sha256"]},
            "correspondenceVerified": not source["dirty"] and manifest["source"]["dirty"] is False,
            "correspondenceNote": "Compiler input and artifact hashes match. Publication additionally requires a clean checkout and runtime build at the same recursive revisions.",
            "archive": {"path": "source.tar.gz", "root": "dynarmic.js/", "files": len(selected),
                        "excluded": list(EXCLUDED_ARCHIVE_PREFIXES),
                        "exclusionReason": "Historical measurements and release observations are not runtime build inputs."},
            "runtimeNotices": notices_info,
            "files": files,
        }
        (temp / "source-manifest.json").write_text(json.dumps(report, indent=2) + "\n")
        subprocess.run(["node", str(temp / "verify.mjs"), *( ["--require-clean"] if require_clean else [])], check=True)
        if output.exists():
            shutil.rmtree(output)
        shutil.copytree(temp, output)
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build-npm/package")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--emscripten-root", type=Path)
    args = parser.parse_args()
    try:
        report = stage(args.build_dir, args.output, args.require_clean, args.emscripten_root)
    except (PackageError, OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"npm staging failed: {error}\n")
    print(f"Staged {report['package']['name']}@{report['package']['version']} in {args.output}")
    print("Clean release candidate" if report["correspondenceVerified"] else "Local snapshot; publication cleanliness not verified")


if __name__ == "__main__":
    main()
