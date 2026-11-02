#!/usr/bin/env python3
"""Compile one freestanding scalar C workload with Zig's same Clang frontend."""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

from reference import reference

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
V86_VERSION = "0.5.473"
V86_REVISION = "315b2b0b758703ef0e6cc7b4088d6068f83f3c20"
V86_URL = f"https://registry.npmjs.org/v86/-/v86-{V86_VERSION}.tgz"
V86_SHA512 = "mNHB0DXDmfxn0CrlqCmlgMR1W7aKuD+sHmSctLcTROOwpOvbE4qsAhmaE6Vy+riQfcE4PfA2xvW59/ZXsYnHNw=="
COMMON = ["-O2", "-ffreestanding", "-fno-builtin", "-fno-strict-aliasing", "-fno-fast-math",
          "-ffp-contract=off", "-fno-vectorize", "-fno-slp-vectorize", "-fno-unroll-loops",
          "-fno-stack-protector", "-fno-pic", "-fno-pie", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
          "-ffunction-sections", "-fdata-sections", "-nostdlib"]


def digest(file, name=None):
    data = file.read_bytes()
    return {"path": name or file.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def v86(cache, dist):
    archive = cache / f"v86-{V86_VERSION}.tgz"
    local = ROOT / "build-v86-quickjs/runtime" / archive.name
    if not archive.exists():
        if local.exists():
            shutil.copyfile(local, archive)
        else:
            with urllib.request.urlopen(V86_URL, timeout=60) as response:
                archive.write_bytes(response.read(32 * 1024 * 1024))
    integrity = base64.b64encode(hashlib.sha512(archive.read_bytes()).digest()).decode()
    if integrity != V86_SHA512:
        raise SystemExit("Pinned v86 npm archive integrity mismatch")
    with tarfile.open(archive) as bundle:
        for entry in bundle.getmembers():
            target = (cache / entry.name).resolve()
            if not target.is_relative_to(cache) or entry.issym() or entry.islnk():
                raise SystemExit("Unsafe npm archive member")
        bundle.extractall(cache)
    package = json.loads((cache / "package/package.json").read_text())
    if package["version"] != "0.5.473+g315b2b0":
        raise SystemExit("Unexpected v86 package version")
    for origin, name in [("build/libv86.js", "c-bench-v86.js"), ("build/v86.wasm", "c-bench-v86.wasm")]:
        shutil.copyfile(cache / "package" / origin, dist / name)
    # The npm package's top-level BSD license does not include the notices for
    # SoftFloat, Zstd's embedded FSE/Huff0/xxHash code, or the MIT floppy code.
    notices = HERE / "licenses/v86-third-party.txt"
    (dist / "c-bench-v86-LICENSE.txt").write_bytes(
        (cache / "package/LICENSE").read_bytes() + b"\n\n" + notices.read_bytes())
    return {"version": package["version"], "revision": V86_REVISION, "url": V86_URL,
            "archiveIntegrity": "sha512-" + V86_SHA512, "archive": digest(archive),
            "additionalNotices": digest(notices, "examples/c-bench/licenses/v86-third-party.txt")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=ROOT / "build-c-bench/dist")
    parser.add_argument("--cache", type=Path, default=ROOT / "build-c-bench/cache")
    args = parser.parse_args()
    dist, cache = args.dist.resolve(), args.cache.resolve()
    if cache == dist or cache.is_relative_to(dist):
        raise SystemExit("Build cache must be outside the public dist directory")
    dist.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.find_spec("ziglang")
    if not spec:
        raise SystemExit("Install the Python ziglang package to build all three targets")
    zig = [sys.executable, "-m", "ziglang"]
    version = subprocess.check_output([*zig, "version"], text=True).strip()
    cc_version = subprocess.check_output([*zig, "cc", "--version"], text=True).strip()
    zig_binary = Path(spec.origin).parent / "zig"
    commands = []

    def run(arguments):
        command = [*zig, *map(str, arguments)]
        commands.append(command)
        print("+ " + " ".join(command), flush=True)
        subprocess.run(command, check=True)

    targets = {
        "arm": {"flags": ["-target", "arm-freestanding-eabi", "-mcpu=cortex_a8-neon", "-mfpu=vfpv3", "-mfloat-abi=softfp", "-marm"],
                "startup": "start-arm.S", "linker": "link-arm.ld", "isa": "ARMv7-A, VFPv3 softfp, ARM state"},
        "x86": {"flags": ["-target", "x86-freestanding-none", "-mcpu=pentium4", "-msse2", "-mfpmath=sse"],
                "startup": "start-x86.S", "linker": "link-x86.ld", "isa": "i386 Pentium 4, SSE2 arithmetic, standard i386 ABI"},
        "wasm": {"flags": ["-target", "wasm32-freestanding"], "isa": "WebAssembly scalar wasm32"},
    }
    for target, configuration in targets.items():
        flags = [*COMMON, *configuration["flags"], "-I" + str(HERE)]
        # Retain compiler assembly for auditing scalar work and real branches.
        run(["cc", *flags, "-S", HERE / "kernels.c", "-o", cache / (target + "-kernels.s")])
        if target == "wasm":
            link = ["-Wl,--no-entry", "-Wl,--export=bench_init", "-Wl,--export=bench_run", "-Wl,--export-memory",
                    "-Wl,--initial-memory=16777216", "-Wl,--max-memory=16777216", "-Wl,-z,stack-size=65536"]
            run(["cc", *flags, HERE / "kernels.c", *link, "-o", dist / "c-bench.wasm"])
        else:
            link = ["-Wl,--gc-sections", "-Wl,--build-id=none", "-T", str(HERE / configuration["linker"])]
            elf = dist / "c-bench-x86.elf" if target == "x86" else cache / "c-bench-arm.elf"
            run(["cc", *flags, HERE / "kernels.c", HERE / "guest.c", HERE / configuration["startup"],
                 *link, "-o", elf])
            if target == "arm":
                run(["objcopy", "-O", "binary", elf, dist / "c-bench-arm.bin"])
        configuration["effectiveFlags"] = flags
        configuration["linkFlags"] = link
        configuration["assembly"] = digest(cache / (target + "-kernels.s"))
    # Preserve the source users are measuring, not an unrelated display snippet.
    displayed = "/* Public declarations */\n" + (HERE / "kernels.h").read_text() + "\n/* Shared implementation */\n" + (HERE / "kernels.c").read_text().replace('#include "kernels.h"', "")
    (dist / "c-bench-source.c").write_text(displayed)
    expected = reference()
    (dist / "c-bench-reference.json").write_text(json.dumps(expected, indent=2) + "\n")
    runtime = v86(cache, dist)
    assets = ["c-bench-arm.bin", "c-bench-x86.elf", "c-bench.wasm", "c-bench-reference.json", "c-bench-source.c",
              "c-bench-v86.js", "c-bench-v86.wasm", "c-bench-v86-LICENSE.txt"]
    inputs = ["build.py", "reference.py", "kernels.c", "kernels.h", "guest.c", "start-arm.S", "start-x86.S", "link-arm.ld", "link-x86.ld",
              "licenses/v86-third-party.txt"]
    manifest = {"schema": "dynarmic.js/c-bench-build-v1", "compiler": {"version": cc_version,
                "zigVersion": version, "binary": digest(zig_binary, str(zig_binary)), "frontend": "One Zig-provided Clang frontend for ARM, x86 and Wasm"},
                "commonFlags": COMMON, "targets": targets, "commands": commands,
                "artifacts": [digest(dist / name) for name in assets],
                "sources": [digest(HERE / name, "examples/c-bench/" + name) for name in inputs],
                "v86": runtime, "protocol": expected["protocol"],
                "semantics": {"optimization": "O2", "scalar": True, "vectorization": False, "unrolling": False,
                              "fastMath": False, "fpContraction": False, "libc": False, "malloc": False,
                              "floatingPointOracle": "All dyadic operations are exact in binary64, verified by independent integer numerators",
                              "branchWork": "Data-dependent branch selects distinct noinline callees"}}
    (dist / "c-bench-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Built three C benchmark targets: " + str(dist))


if __name__ == "__main__":
    main()
