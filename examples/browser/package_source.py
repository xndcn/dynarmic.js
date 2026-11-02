#!/usr/bin/env python3
"""Attach the actual recursive source checkout and notices to a Pages artifact.

Local snapshots may contain uncommitted work. Only --require-clean establishes
source correspondence for publication, and it also requires a clean runtime
build manifest. No commit or repository setting is changed by this script.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile


class PackageError(RuntimeError):
    """An incomplete or inconsistent source/runtime distribution."""


EXCLUDED_ARCHIVE_PREFIXES = ("internals/measurements/", "internals/release-checks/")
DELTABLUE_GPL2_SHA256 = "8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643"


def distributable_paths(selected: list[Path]) -> list[Path]:
    # Historical reports are not build inputs. Keep public tests, build scripts,
    # examples and recursive dependencies in both npm and Pages distributions.
    return [path for path in selected
            if not path.as_posix().startswith(EXCLUDED_ARCHIVE_PREFIXES)]


def git(root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True)
    if result.returncode:
        raise PackageError(f"git {' '.join(args)} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def digest(file: Path) -> dict:
    checksum = hashlib.sha256()
    size = 0
    with file.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
            size += len(chunk)
    return {"sha256": checksum.hexdigest(), "bytes": size}


def safe_relative(name: bytes) -> Path:
    value = os.fsdecode(name)
    parts = PurePosixPath(value).parts
    if not parts or value.startswith("/") or any(part in ("..", ".git") for part in parts):
        raise PackageError(f"unsafe source path: {value!r}")
    return Path(*parts)


def source_files(root: Path, dist: Path) -> tuple[list[Path], dict]:
    files = set()
    submodules = []

    def visit(repo: Path, prefix: Path, expected: str | None = None) -> dict:
        # An empty submodule directory must not accidentally inherit its parent
        # Git repository; that would silently omit the dependency's sources.
        if not (repo / ".git").exists():
            raise PackageError(f"missing repository metadata at {repo}; initialize recursive submodules")
        top = Path(os.fsdecode(git(repo, "rev-parse", "--show-toplevel")).strip()).resolve()
        if top != repo.resolve():
            raise PackageError(f"uninitialized submodule at {repo}")
        revision = git(repo, "rev-parse", "HEAD").decode().strip()
        status = git(repo, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none")
        info = {"revision": revision, "dirty": bool(status)}
        if expected is not None:
            info.update(path=prefix.as_posix(), indexRevision=expected)
            info["dirty"] = info["dirty"] or revision != expected
            submodules.append(info)
        tracked = git(repo, "ls-files", "--stage", "-z").split(b"\0")
        for record in filter(None, tracked):
            header, name = record.split(b"\t", 1)
            mode, object_id, stage = header.split()
            relative = safe_relative(name)
            if stage != b"0":
                raise PackageError(f"unmerged source file: {prefix / relative}")
            if mode == b"160000":
                visit(repo / relative, prefix / relative, object_id.decode())
            else:
                files.add(prefix / relative)
        for name in filter(None, git(repo, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0")):
            files.add(prefix / safe_relative(name))
        return info

    source = visit(root, Path())
    source["submodules"] = sorted(submodules, key=lambda entry: entry["path"])
    source["dirty"] = source["dirty"] or any(entry["dirty"] for entry in submodules)
    selected = []
    for relative in sorted(files, key=lambda entry: entry.as_posix()):
        file = root / relative
        # The output may be an unignored directory during local experimentation.
        # Never recursively include this package or its build artifact.
        if file == dist or dist in file.parents:
            continue
        if not file.exists() and not file.is_symlink():
            # A deleted tracked file is deliberately absent in a dirty snapshot.
            continue
        if not file.is_file() and not file.is_symlink():
            raise PackageError(f"source is not a regular file or symlink: {relative}")
        # A replaced parent directory could otherwise pull in files from outside
        # the checkout without being recorded as a symlink in the archive.
        for parent in relative.parents:
            if parent != Path() and (root / parent).is_symlink():
                raise PackageError(f"source has a symlink parent: {relative}")
        selected.append(relative)
    return selected, source


def verify_runtime(root: Path, dist: Path, source: dict, require_clean: bool,
                   host: str = "arm_jit", manifest_name: str = "wasm-build-manifest.json") -> dict:
    manifest_file = dist / manifest_name
    try:
        build = json.loads(manifest_file.read_text())
        if build["schema"] != "dynarmic.js/wasm-coremark-host-build-v1":
            raise PackageError("unsupported runtime build manifest schema")
        if build["source"]["revision"] != source["revision"]:
            raise PackageError("runtime build revision does not match the current checkout; rebuild browser_page")
        upstream = next((entry for entry in source["submodules"] if entry["path"] == "externals/dynarmic"), None)
        if upstream is None or build["source"]["upstream"] != upstream["revision"]:
            raise PackageError("runtime upstream revision does not match the recursive checkout")
        if not isinstance(build["source"]["dirty"], bool):
            raise PackageError("runtime source cleanliness is unknown; rebuild from a Git checkout")
        if require_clean and (source["dirty"] or build["source"]["dirty"]):
            raise PackageError("publication requires a clean checkout AND a clean runtime build; rebuild after committing")
        artifacts = {}
        for kind, name in (("js", host + ".js"), ("wasm", host + ".wasm")):
            actual = digest(dist / name)
            expected = build["artifacts"][kind]
            if actual != {key: expected[key] for key in ("sha256", "bytes")}:
                raise PackageError(f"{name} does not match its runtime build manifest")
            artifacts[name] = actual
        artifacts[manifest_name] = digest(manifest_file)
        runtime = {
            "source": build["source"],
            "build": build["build"],
            "configuration": build["configuration"],
            "buildEnvironment": build["buildEnvironment"],
            "artifacts": artifacts,
        }
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise PackageError(f"cannot validate runtime build manifest: {error}") from error
    return runtime


def emscripten_directory(dist: Path, explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit.resolve()
    compiler = shutil.which("emcc")
    if compiler:
        return Path(compiler).resolve().parent
    # browser_page's usual output is <build>/examples/browser/dist. A cache
    # fallback keeps local packaging usable without changing the caller's PATH.
    if len(dist.parents) >= 3:
        cache = dist.parents[2] / "CMakeCache.txt"
        if cache.is_file():
            for line in cache.read_text().splitlines():
                if line.startswith("CMAKE_TOOLCHAIN_FILE:") and "=" in line:
                    toolchain = Path(line.split("=", 1)[1])
                    if toolchain.name == "Emscripten.cmake" and len(toolchain.parents) >= 4:
                        return toolchain.resolve().parents[3]
    raise PackageError("cannot locate the matching Emscripten SDK; pass --emscripten-root /path/to/emscripten")


def verify_quickjs(root: Path, dist: Path) -> dict | None:
    """Bind both QuickJS builds to local harness inputs and the shipped source."""
    manifest_file = dist / "quickjs-manifest.json"
    if not (root / "examples/quickjs/build.py").exists() and not manifest_file.exists():
        return None
    try:
        manifest = json.loads(manifest_file.read_text())
        if manifest["schema"] != "dynarmic.js/quickjs-build-v1":
            raise PackageError("unsupported QuickJS build manifest schema")
        artifacts = {}
        for entry in manifest["artifacts"]:
            name = entry["path"]
            relative = safe_relative(os.fsencode(name))
            if len(relative.parts) != 1 or relative.is_absolute() or name in artifacts:
                raise PackageError(f"invalid QuickJS artifact path: {name}")
            actual = digest(dist / relative)
            if actual != {key: entry[key] for key in ("sha256", "bytes")}:
                raise PackageError(f"{name} does not match the QuickJS build manifest; rebuild browser_page")
            artifacts[name] = actual
        required = {"quickjs-arm.bin", "quickjs-wasm.js", "quickjs-wasm.wasm",
                    "quickjs-workloads.js", "quickjs-LICENSE.txt", "quickjs-source.tar.xz",
                    "quickjs-runtime-licenses.txt"}
        if (root / "examples/quickjs/official_benchmarks.py").exists():
            required.update({"official-v8-upstream.js", "official-v8.js", "official-v8-adapter.js",
                             "official-benchmarks.json", "quickjs-extras-source.tar.xz"})
        if not required.issubset(artifacts):
            raise PackageError("QuickJS build manifest omits required runtime/source artifacts")
        sources = {}
        for entry in manifest["sources"]:
            name = entry["path"]
            relative = safe_relative(os.fsencode(name))
            if not name.startswith("examples/quickjs/") or name in sources:
                raise PackageError(f"invalid QuickJS source path: {name}")
            actual = digest(root / relative)
            if actual != {key: entry[key] for key in ("sha256", "bytes")}:
                raise PackageError(f"QuickJS source {name} changed since the build; rebuild browser_page")
            sources[name] = actual
        if "examples/quickjs/build.py" not in sources:
            raise PackageError("QuickJS build manifest omits its build script")
        upstream = manifest["upstream"]
        if upstream["archive"] != "quickjs-source.tar.xz" or upstream["sha256"] != artifacts["quickjs-source.tar.xz"]["sha256"]:
            raise PackageError("QuickJS upstream source archive does not match its recorded checksum")
        normalized_version(manifest["toolchains"]["emscriptenVersion"])
        official = verify_official_benchmarks(root, dist, artifacts) if "official-benchmarks.json" in artifacts else None
        configuration = manifest.get("configuration", {})
        rebuild_options = quickjs_rebuild_options(configuration)
        return {"manifest": {"path": "quickjs-manifest.json", **digest(manifest_file)},
                "upstream": upstream, "sources": sources, "artifacts": artifacts,
                "toolchains": manifest["toolchains"], "flags": manifest["flags"],
                "configuration": configuration, "rebuildCmakeOptions": rebuild_options,
                "reference": manifest.get("reference", {}),
                "runtimeNotices": manifest["runtimeNotices"],
                **({"officialBenchmarks": official} if official is not None else {})}
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise PackageError(f"cannot validate QuickJS build/source correspondence: {error}") from error


def verify_c_bench(root: Path, dist: Path) -> dict | None:
    """Require the complete C comparison and bind it to the current sources."""
    manifest_file = dist / "c-bench-manifest.json"
    if not (root / "examples/c-bench/build.py").exists() and not manifest_file.exists():
        return None
    try:
        manifest = json.loads(manifest_file.read_text())
        if manifest["schema"] != "dynarmic.js/c-bench-build-v1":
            raise PackageError("unsupported C benchmark build manifest schema")
        artifacts = {}
        for entry in manifest["artifacts"]:
            name = entry["path"]
            relative = safe_relative(os.fsencode(name))
            if len(relative.parts) != 1 or relative.is_absolute() or name in artifacts:
                raise PackageError(f"invalid C benchmark artifact path: {name}")
            actual = digest(dist / relative)
            if actual != {key: entry[key] for key in ("sha256", "bytes")}:
                raise PackageError(f"{name} does not match the C benchmark build manifest; rebuild browser_page")
            artifacts[name] = actual
        required = {"c-bench-arm.bin", "c-bench-x86.elf", "c-bench.wasm", "c-bench-reference.json",
                    "c-bench-source.c", "c-bench-v86.js", "c-bench-v86.wasm", "c-bench-v86-LICENSE.txt"}
        if not required.issubset(artifacts):
            raise PackageError("C benchmark build manifest omits required runtime/source/license artifacts")
        if not (dist / "c-bench-v86-LICENSE.txt").read_text().strip():
            raise PackageError("C benchmark v86 license is empty")
        sources = {}
        for entry in manifest["sources"]:
            name = entry["path"]
            relative = safe_relative(os.fsencode(name))
            if not name.startswith("examples/c-bench/") or name in sources:
                raise PackageError(f"invalid C benchmark source path: {name}")
            actual = digest(root / relative)
            if actual != {key: entry[key] for key in ("sha256", "bytes")}:
                raise PackageError(f"C benchmark source {name} changed since the build; rebuild browser_page")
            sources[name] = actual
        if "examples/c-bench/build.py" not in sources:
            raise PackageError("C benchmark build manifest omits its build script")
        required_sources = {"examples/c-bench/" + name for name in
                            ("reference.py", "kernels.c", "kernels.h", "guest.c", "start-arm.S",
                             "start-x86.S", "link-arm.ld", "link-x86.ld", "licenses/v86-third-party.txt")}
        if not required_sources.issubset(sources):
            raise PackageError("C benchmark build manifest omits required kernel/reference/guest sources")
        return {**manifest, "manifest": {"path": manifest_file.name, **digest(manifest_file)},
                "sources": sources, "artifacts": artifacts}
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise PackageError(f"cannot validate C benchmark build/source correspondence: {error}") from error


def quickjs_rebuild_options(configuration: dict) -> dict[str, str]:
    """Map recorded build choices to safe, explicit CMake cache arguments."""
    if not isinstance(configuration, dict):
        raise PackageError("invalid QuickJS configuration")
    if "targets" not in configuration:
        # Older builds used the fixed default configuration and did not record
        # target-specific choices. Preserve their original rebuild commands.
        return {}
    targets = configuration["targets"]
    if not isinstance(targets, dict) or not isinstance(targets.get("arm"), dict):
        raise PackageError("invalid QuickJS ARM target configuration")
    arm = targets["arm"]
    options = {}
    for field, suffix, allowed in (
        ("optimization", "OPT", ("O2", "O3", "Os")),
        ("dispatch", "DISPATCH", ("switch", "threaded")),
        ("isa", "ISA", ("arm", "thumb")),
    ):
        value = arm.get(field)
        if not isinstance(value, str) or value not in allowed:
            raise PackageError(f"invalid QuickJS ARM {field}; expected one of {', '.join(allowed)}")
        options["DYNARMIC_QUICKJS_ARM_" + suffix] = value
    for field, suffix in (("lto", "LTO"), ("noGcse", "NO_GCSE"), ("noCrossjumping", "NO_CROSSJUMPING")):
        value = arm.get(field)
        if not isinstance(value, bool):
            raise PackageError(f"invalid QuickJS ARM {field}; expected a boolean")
        options["DYNARMIC_QUICKJS_ARM_" + suffix] = "ON" if value else "OFF"
    return options


def verify_official_benchmarks(root: Path, dist: Path, artifacts: dict) -> dict:
    """Verify the shipped V8 inventory against the pristine extras source."""
    license_name = "quickjs-extras-LICENSE.txt"
    if license_name not in artifacts:
        raise PackageError("official benchmark distribution omits the DeltaBlue GPL-2.0 license")
    license_source = root / "examples/quickjs/licenses/GPL-2.0.txt"
    license_digest = digest(license_source)
    if license_digest["sha256"] != DELTABLUE_GPL2_SHA256:
        raise PackageError("DeltaBlue GPL-2.0 license source is incomplete")
    if license_digest != artifacts[license_name]:
        raise PackageError("distributed DeltaBlue GPL-2.0 license does not match its complete source text")
    inventory = json.loads((dist / "official-benchmarks.json").read_text())
    if inventory["schema"] != "dynarmic.js/official-benchmarks-v1":
        raise PackageError("unsupported official benchmark inventory schema")
    sources = inventory["sources"]
    if len(sources) != 1:
        raise PackageError("official benchmark inventory must identify its extras source")
    origin = sources[0]
    archive_name = origin["archive"]
    if archive_name != "quickjs-extras-source.tar.xz" or origin["sha256"] != artifacts[archive_name]["sha256"]:
        raise PackageError("official benchmark extras archive checksum does not match")
    with tarfile.open(dist / archive_name) as archive:
        matching = [item for item in archive.getmembers() if item.name.endswith("/tests/bench-v8/combined.js") and item.isfile()]
        if len(matching) != 1:
            raise PackageError("official extras archive must contain exactly one V8 combined source")
        original = archive.extractfile(matching[0]).read()
    if original != (dist / "official-v8-upstream.js").read_bytes() or hashlib.sha256(original).hexdigest() != origin["upstreamCombinedSha256"]:
        raise PackageError("official V8 upstream source differs from its pristine extras archive")
    # Inventory identifiers must describe every upstream benchmark exactly once.
    declarations = list(re.finditer(rb"new BenchmarkSuite\('([^']+)'", original))
    expected = []
    for index, declaration in enumerate(declarations):
        suite_name = declaration[1]
        end = declarations[index + 1].start() if index + 1 < len(declarations) else len(original)
        body = original[declaration.end():end]
        for name in re.findall(rb'new Benchmark\([\"\x27]([^\"\x27]+)[\"\x27]', body):
            expected.append(suite_name.decode() + "/" + name.decode())
    actual = [entry["id"] for entry in inventory["cases"]]
    grouped = [suite["id"] + "/" + name for suite in inventory["suites"] for name in suite["benchmarks"]]
    if len(declarations) != 8 or len(expected) != 10 or len(actual) != 10 or len(set(actual)) != 10 or set(actual) != set(expected) or grouped != actual:
        raise PackageError("official V8 inventory omits, duplicates or changes an upstream case")
    return {"inventory": inventory, "archive": {"path": archive_name, **artifacts[archive_name]}}


def normalized_version(value: str) -> str:
    match = re.fullmatch(r'"?(\d+\.\d+\.\d+(?:[-+][^"\s]+)?)"?', value.strip()) if isinstance(value, str) else None
    if not match:
        raise PackageError(f"invalid Emscripten version: {value!r}")
    return match.group(1)


def runtime_notices(root: Path, sdk: Path, runtime: dict) -> tuple[str, dict]:
    try:
        version = normalized_version((sdk / "emscripten-version.txt").read_text())
        recorded = normalized_version(runtime["build"]["emscriptenVersion"])
        if version != recorded:
            raise PackageError(f"SDK version {version} differs from runtime build {recorded}; select the matching SDK")
        parts = [
            f"dynarmic.js browser runtime — third-party license texts\nEmscripten {version}\n\n"
            "The JavaScript/Wasm distribution includes Emscripten runtime support and\n"
            "system-library code. The following notices are retained from the matching\n"
            "SDK, including optional unwind/LLVM libc notices where supplied. Inclusion\n"
            "of a notice is not a claim that every function of that library is linked.\n"
            "Project and vendored dependency notices are also in source.tar.gz.\n"
        ]
        files = []

        def append(component: str, base: Path, relative: str, origin: str, excerpt: str | None = None) -> None:
            file = base / relative
            content = file.read_text()
            if not content.strip():
                raise PackageError(f"empty license text: {relative}")
            notice = content if excerpt is None else excerpt
            parts.extend([f"\n{'=' * 72}\n{component}\nSource: {origin}/{relative}\n{'=' * 72}\n", notice.rstrip() + "\n"])
            entry = {"component": component, "origin": origin, "path": relative, **digest(file)}
            if excerpt is not None:
                entry["excerptSha256"] = hashlib.sha256(excerpt.encode()).hexdigest()
            files.append(entry)

        for component, relative in (
            ("Emscripten runtime (MIT / University of Illinois NCSA)", "LICENSE"),
            ("musl libc", "system/lib/libc/musl/COPYRIGHT"),
            ("LLVM libc++", "system/lib/libcxx/LICENSE.TXT"),
            ("LLVM libc++abi", "system/lib/libcxxabi/LICENSE.TXT"),
            ("LLVM compiler-rt", "system/lib/compiler-rt/LICENSE.TXT"),
        ):
            append(component, sdk, relative, "emscripten")
        for component, relative in (
            ("LLVM libunwind", "system/lib/libunwind/LICENSE.TXT"),
            ("LLVM libc", "system/lib/llvm-libc/LICENSE.TXT"),
        ):
            if (sdk / relative).exists():
                append(component, sdk, relative, "emscripten")
        malloc_file = "system/lib/dlmalloc.c"
        with (sdk / malloc_file).open() as stream:
            header = stream.read(16384)
        declaration = re.search(r"/\*\s*(This is a version \(aka dlmalloc\).*?)\s*\* Quickstart", header, re.S)
        if not declaration or "released to the public domain" not in declaration.group(1):
            raise PackageError("dlmalloc public-domain declaration not found in the matching SDK")
        append("dlmalloc — Doug Lea's public-domain declaration", sdk, malloc_file,
               "emscripten", declaration.group(1).strip() + "\n")
        boost_license = "externals/dynarmic/externals/catch/LICENSE.txt"
        if not (root / boost_license).read_text().startswith("Boost Software License - Version 1.0"):
            raise PackageError("bundled canonical Boost Software License text is missing or changed")
        parts.append("\nBoost headers retain their individual copyright notices in the Boost source\n"
                     "distribution. The standard license below includes its exception for\n"
                     "machine-executable object code generated by a source language processor.\n")
        append("Boost headers — Boost Software License 1.0", root, boost_license, "repository")
    except (OSError, UnicodeError, KeyError, TypeError) as error:
        raise PackageError(f"cannot collect complete runtime license notices: {error}") from error
    return "".join(parts), {"emscriptenVersion": version, "files": files}


def write_archive(root: Path, selected: list[Path], output: Path) -> None:
    selected_names = {entry.as_posix() for entry in selected}
    with output.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|") as archive:
            for relative in selected:
                file = root / relative
                before = file.lstat()
                member = tarfile.TarInfo("dynarmic.js/" + relative.as_posix())
                member.uid = member.gid = 0
                member.uname = member.gname = ""
                member.mtime = 0
                if stat.S_ISLNK(before.st_mode):
                    target = os.readlink(file)
                    if os.path.isabs(target):
                        raise PackageError(f"absolute symlink is not portable: {relative}")
                    try:
                        resolved = file.resolve(strict=True).relative_to(root)
                    except (OSError, ValueError) as error:
                        raise PackageError(f"symlink escapes the source tree or is broken: {relative}") from error
                    if resolved.as_posix() not in selected_names or not (root / resolved).is_file():
                        raise PackageError(f"symlink target is not an included regular source file: {relative}")
                    member.type = tarfile.SYMTYPE
                    member.mode = 0o777
                    member.linkname = target
                    archive.addfile(member)
                elif stat.S_ISREG(before.st_mode):
                    member.mode = 0o755 if before.st_mode & stat.S_IXUSR else 0o644
                    member.size = before.st_size
                    with file.open("rb") as stream:
                        archive.addfile(member, stream)
                else:
                    raise PackageError(f"unsupported source file type: {relative}")
                after = file.lstat()
                if (before.st_mtime_ns, before.st_size, before.st_ino) != (after.st_mtime_ns, after.st_size, after.st_ino):
                    raise PackageError(f"source changed while archiving: {relative}; retry when edits finish")


def package_source(root: Path, dist: Path, require_clean: bool = False, emscripten_root: Path | None = None) -> dict:
    root, dist = root.resolve(), dist.resolve()
    if not dist.is_dir() or root == dist:
        raise PackageError("--dist must be an existing build output directory, separate from the source root")
    selected, source = source_files(root, dist)
    selected = distributable_paths(selected)
    for required in ("LICENSE", "THIRD_PARTY_NOTICES.md", "CMakeLists.txt", "externals/dynarmic/LICENSE.txt"):
        if Path(required) not in selected:
            raise PackageError(f"source archive is missing {required}")
    runtime = verify_runtime(root, dist, source, require_clean)
    quickjs = verify_quickjs(root, dist)
    c_bench = verify_c_bench(root, dist)
    official_host = None
    if quickjs and "officialBenchmarks" in quickjs:
        official_host = verify_runtime(root, dist, source, require_clean, "arm_bench_jit", "arm-bench-build-manifest.json")
        if normalized_version(official_host["build"]["emscriptenVersion"]) != normalized_version(runtime["build"]["emscriptenVersion"]):
            raise PackageError("official benchmark host uses a different Emscripten SDK version")
    if quickjs and normalized_version(quickjs["toolchains"]["emscriptenVersion"]) != normalized_version(runtime["build"]["emscriptenVersion"]):
        raise PackageError("QuickJS and Dynarmic use different Emscripten SDK versions; rebuild with the same SDK before packaging runtime notices")
    sdk = emscripten_directory(dist, emscripten_root)
    notices, notices_info = runtime_notices(root, sdk, runtime)
    # Temp outputs stay on the same filesystem for atomic individual replacement.
    # If validation fails, existing package files are left untouched.
    with tempfile.TemporaryDirectory(prefix=".source-package-", dir=dist) as temp:
        temp = Path(temp)
        archive = temp / "source.tar.gz"
        write_archive(root, selected, archive)
        after_selected, after_source = source_files(root, dist)
        if selected != distributable_paths(after_selected) or source != after_source:
            raise PackageError("source checkout changed while archiving; retry when edits finish")
        if runtime != verify_runtime(root, dist, source, require_clean):
            raise PackageError("runtime artifacts changed while archiving; do not rebuild during packaging")
        if official_host is not None and official_host != verify_runtime(root, dist, source, require_clean, "arm_bench_jit", "arm-bench-build-manifest.json"):
            raise PackageError("official benchmark host changed while archiving; do not rebuild during packaging")
        if quickjs != verify_quickjs(root, dist):
            raise PackageError("QuickJS artifacts changed while archiving; do not rebuild during packaging")
        if c_bench != verify_c_bench(root, dist):
            raise PackageError("C benchmark artifacts changed while archiving; do not rebuild during packaging")
        if (notices, notices_info) != runtime_notices(root, sdk, runtime):
            raise PackageError("runtime license inputs changed while archiving")
        (temp / "runtime-licenses.txt").write_text(notices)
        manifest = {
            "schema": "dynarmic.js/pages-source-v1",
            "source": source,
            "runtimeBuild": runtime,
            **({"officialHostBuild": official_host} if official_host is not None else {}),
            **({"quickjsBuild": quickjs} if quickjs is not None else {}),
            **({"cBenchBuild": c_bench} if c_bench is not None else {}),
            "runtimeNotices": {"path": "runtime-licenses.txt", **notices_info, **digest(temp / "runtime-licenses.txt")},
            "correspondenceVerified": not source["dirty"] and not runtime["source"]["dirty"] and
                                      (official_host is None or not official_host["source"]["dirty"]),
            "correspondenceNote": "Clean snapshots and clean runtime builds at matching revisions establish correspondence. Dirty snapshots are for local review only; a revision match does not prove that uncommitted source matches the runtime.",
            "archive": {"path": "source.tar.gz", "root": "dynarmic.js/", "files": len(selected),
                        "excluded": list(EXCLUDED_ARCHIVE_PREFIXES), **digest(archive)},
            "rebuild": {
                "guide": "docs/GettingStarted.md",
                "cmakeCacheOptions": quickjs["rebuildCmakeOptions"] if quickjs else {},
                "commands": [
                    "tar -xzf source.tar.gz",
                    "cd dynarmic.js",
                    "source /path/to/emsdk/emsdk_env.sh",
                    "emcmake cmake -S . -B build-em -DCMAKE_BUILD_TYPE=Release -DBoost_INCLUDE_DIR=/path/to/boost-headers -DDYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON" +
                    ("".join(f" -D{key}={value}" for key, value in quickjs["rebuildCmakeOptions"].items()) if quickjs else ""),
                    *(["mkdir -p build-em/examples/browser/quickjs-cache",
                       "cp ../quickjs-source.tar.xz build-em/examples/browser/quickjs-cache/"] if quickjs else []),
                    *(["cp ../quickjs-extras-source.tar.xz build-em/examples/browser/quickjs-cache/quickjs-extras-2025-09-13.tar.xz"] if official_host else []),
                    "cmake --build build-em --target browser_page --parallel 2",
                ],
                "note": "Install the toolchains from the guide. Match the recorded runtimeBuild settings when rebuilding a non-default configuration; timing and byte-for-byte binary identity are not promised.",
            },
        }
        for source_name, published_name in (("LICENSE", "LICENSE.txt"), ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md")):
            (temp / published_name).write_bytes((root / source_name).read_bytes())
        (temp / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        for name in ("source.tar.gz", "LICENSE.txt", "THIRD_PARTY_NOTICES.md", "runtime-licenses.txt", "source-manifest.json"):
            os.replace(temp / name, dist / name)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, required=True, help="built browser dist directory")
    parser.add_argument("--require-clean", action="store_true", help="reject local or runtime-build changes before publication")
    parser.add_argument("--emscripten-root", type=Path, help="SDK Emscripten directory used for this runtime build")
    args = parser.parse_args()
    try:
        manifest = package_source(Path(__file__).resolve().parents[2], args.dist, args.require_clean, args.emscripten_root)
    except (PackageError, OSError) as error:
        parser.exit(1, f"source packaging failed: {error}\n")
    label = "verified clean source" if manifest["correspondenceVerified"] else "local snapshot; runtime correspondence unverified"
    print(f"Packaged {manifest['archive']['files']} files ({manifest['archive']['bytes']} bytes): {label}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
