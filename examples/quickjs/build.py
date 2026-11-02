#!/usr/bin/env python3
"""Build identical pinned QuickJS engines for bare-metal ARM32 and direct Wasm."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
from runtime_notices import write_notices
from official_benchmarks import prepare_bench_v8

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VERSION = "2025-09-13"
URL = "https://bellard.org/quickjs/quickjs-2025-09-13-2.tar.xz"
SHA256 = "996c6b5018fc955ad4d06426d0e9cb713685a00c825aa5c0418bd53f7df8b0b4"
ENGINE_FILES = ["quickjs.c", "dtoa.c", "libregexp.c", "libunicode.c", "cutils.c"]
LOCAL_INPUTS = ["build.py", "workloads.js", "runner.c", "syscalls.c", "start.S", "arm_newlib.h", "link.ld", "reference.py", "runtime_notices.py", "official_benchmarks.py", "official-v8-adapter.js"]


def record(path, name):
    data = path.read_bytes()
    return {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def run(command):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run([str(x) for x in command], check=True)


def tool(name):
    resolved = shutil.which(name)
    if not resolved:
        raise SystemExit(f"Missing {name}; see examples/quickjs/README.md")
    return str(Path(resolved).resolve())


def arm_dispatch_source(engine_path, cache, dispatch):
    """Override only interpreter dispatch; retain all EMSCRIPTEN platform guards."""
    if dispatch == "switch":
        return engine_path
    original = engine_path.read_text()
    upstream = "#if defined(EMSCRIPTEN)\n#define DIRECT_DISPATCH  0\n#else\n#define DIRECT_DISPATCH  1\n#endif"
    if original.count(upstream) != 1:
        raise SystemExit("QuickJS DIRECT_DISPATCH guard changed; review the ARM override")
    override = "#if defined(DYNARMIC_QUICKJS_ARM_DISPATCH)\n#define DIRECT_DISPATCH DYNARMIC_QUICKJS_ARM_DISPATCH\n#elif defined(EMSCRIPTEN)\n#define DIRECT_DISPATCH  0\n#else\n#define DIRECT_DISPATCH  1\n#endif"
    # Preserve upstream __FILE__/__LINE__ in diagnostics. The direct Wasm
    # compiler reads the original engine_path and never sees this derived file.
    continuation_line = original[:original.index(upstream) + len(upstream)].count("\n") + 2
    derived = '#line 1 ' + json.dumps(str(engine_path)) + '\n' + original.replace(
        upstream, override + '\n#line ' + str(continuation_line) + ' ' + json.dumps(str(engine_path)))
    path = cache / "quickjs-arm.c"
    path.write_text(derived)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--emcc", default="emcc")
    parser.add_argument("--arm-cc", default="arm-none-eabi-gcc")
    parser.add_argument("--arm-objcopy", default="arm-none-eabi-objcopy")
    parser.add_argument("--arm-opt", choices=["O2", "O3", "Os"], default="O2",
                        help="ARM guest optimization level; direct Wasm always uses O2")
    parser.add_argument("--arm-dispatch", choices=["switch", "threaded"], default="switch",
                        help="ARM interpreter dispatch only; all platform feature guards remain unchanged")
    parser.add_argument("--arm-lto", action="store_true", help="Enable ARM guest link-time optimization")
    parser.add_argument("--arm-isa", choices=["arm", "thumb"], default="arm",
                        help="ARM guest C instruction set; startup remains ARM with linker interworking")
    parser.add_argument("--arm-no-gcse", action="store_true",
                        help="Disable ARM GCC's GCSE pass only; does not disable GCSE after reload or other merging passes")
    parser.add_argument("--arm-no-crossjumping", action="store_true",
                        help="Disable ARM GCC's cross-jumping pass only; other tail-merging passes keep compiler defaults")
    args = parser.parse_args()
    dist = args.dist.resolve()
    cache = (args.cache or dist.parent / "quickjs-build").resolve()
    arm_cc, emcc, objcopy = tool(args.arm_cc), tool(args.emcc), tool(args.arm_objcopy)
    if cache == dist or cache.is_relative_to(dist):
        raise SystemExit("The build cache must be outside the public dist directory")
    dist.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / "quickjs-source.tar.xz"
    if not archive.exists():
        print(f"Downloading pinned QuickJS source: {URL}", flush=True)
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read(4 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise SystemExit("QuickJS source checksum mismatch")
        temporary = archive.with_suffix(".download")
        temporary.write_bytes(data)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit(f"QuickJS source checksum mismatch: remove {archive} and retry")
    # Re-extract verified bytes on every build so stale/edited caches cannot
    # quietly diverge from the source archive shipped with the demo.
    source = cache / f"quickjs-{VERSION}"
    if source.exists():
        shutil.rmtree(source)
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            target = (cache / member.name).resolve()
            if not target.is_relative_to(cache) or member.issym() or member.islnk():
                raise SystemExit("Unsafe QuickJS source archive member")
        bundle.extractall(cache)
    # Newlib has no tm_gmtoff. This embedding has no timezone service: apply
    # identical UTC-only Date behavior to both targets, without changing the
    # benchmark interpreter or kernels. Preserve the pristine archive in dist.
    engine_path = source / "quickjs.c"
    engine = engine_path.read_text()
    timezone_code = "        localtime_r(&ti, &tm);\n        res = -tm.tm_gmtoff / 60;"
    if engine.count(timezone_code) != 1:
        raise SystemExit("QuickJS timezone adaptation no longer matches upstream")
    engine_path.write_text(engine.replace(timezone_code, "        (void)tm; (void)ti;\n        res = 0; /* Bare-metal demo: UTC on both targets. */"))
    workloads = (HERE / "workloads.js").read_bytes()
    (cache / "workloads.h").write_text("static const unsigned char workloads_js[] = {\n" +
        ",".join(str(byte) for byte in workloads + b"\0") + "\n};\n")
    common = ["-fwrapv", "-fno-strict-aliasing", "-D_GNU_SOURCE", '-DCONFIG_VERSION="' + VERSION + '"',
              "-DEMSCRIPTEN", "-I" + str(source), "-I" + str(cache)]
    sources = [source / name for name in ENGINE_FILES] + [HERE / "runner.c"]
    arm_common = ["-" + args.arm_opt, *common]
    wasm_common = ["-O2", *common]
    arm_engine = arm_dispatch_source(engine_path, cache, args.arm_dispatch)
    arm_sources = [arm_engine, *sources[1:]]
    arm_flags = ["-march=armv7-a", "-mfpu=vfpv3", "-mfloat-abi=softfp", "-m" + args.arm_isa, "-nostartfiles",
                 # GCC's bundled stdint.h omits Newlib's __int64_t_defined
                 # guard, which inttypes.h uses for PRI*64. Newlib 4 stdio.h
                 # no longer includes sys/types.h indirectly; include the
                 # public type header explicitly instead of inventing formats.
                 "-include", str(HERE / "arm_newlib.h"),
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-T", str(HERE / "link.ld")]
    if args.arm_dispatch == "threaded":
        arm_flags.append("-DDYNARMIC_QUICKJS_ARM_DISPATCH=1")
    if args.arm_lto:
        arm_flags.append("-flto")
    if args.arm_no_gcse:
        arm_flags.append("-fno-gcse")
    if args.arm_no_crossjumping:
        arm_flags.append("-fno-crossjumping")
    wasm_flags = ["--no-entry", "-sMODULARIZE=1", "-sEXPORT_NAME=createQuickJS",
                  "-sENVIRONMENT=web,worker,node", '-sEXPORTED_FUNCTIONS=["_qjs_bench","_qjs_eval","_qjs_set_memory_limit","_malloc","_free"]',
                  '-sEXPORTED_RUNTIME_METHODS=["HEAPU8"]', "-sINITIAL_MEMORY=16777216",
                  "-sSTACK_SIZE=1048576", "-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=268435456",
                  "-sFILESYSTEM=0"]
    run([arm_cc, *arm_common, *arm_flags, *arm_sources, HERE / "start.S", HERE / "syscalls.c",
         "-lm", "-lc", "-lgcc", "-o", cache / "quickjs-arm.elf"])
    run([objcopy, "-O", "binary", cache / "quickjs-arm.elf", dist / "quickjs-arm.bin"])
    run([emcc, *wasm_common, *wasm_flags, *sources, "-lm", "-o", dist / "quickjs-wasm.js"])
    shutil.copyfile(HERE / "workloads.js", dist / "quickjs-workloads.js")
    shutil.copyfile(source / "LICENSE", dist / "quickjs-LICENSE.txt")
    shutil.copyfile(archive, dist / "quickjs-source.tar.xz")
    official_v8 = prepare_bench_v8(cache, dist, VERSION)
    runtime_notices = write_notices(dist, arm_cc)
    artifacts = ["quickjs-arm.bin", "quickjs-wasm.js", "quickjs-wasm.wasm", "quickjs-workloads.js",
                 "quickjs-LICENSE.txt", "quickjs-source.tar.xz", "quickjs-runtime-licenses.txt",
                 "official-v8-upstream.js", "official-v8.js", "official-v8-adapter.js", "official-benchmarks.json",
                 "quickjs-extras-source.tar.xz", "quickjs-extras-LICENSE.txt"]
    local_inputs = LOCAL_INPUTS + [str(p.relative_to(HERE)) for p in sorted((HERE / "licenses").glob("*")) if p.is_file()]
    manifest = {
        "schema": "dynarmic.js/quickjs-build-v1",
        "upstream": {"version": VERSION, "url": URL, "sha256": SHA256, "archive": "quickjs-source.tar.xz"},
        "artifacts": [record(dist / name, name) for name in artifacts],
        "sources": [record(HERE / name, "examples/quickjs/" + name) for name in local_inputs],
        "runtimeNotices": runtime_notices,
        "officialBenchmarks": {"inventory": "official-benchmarks.json", "suites": official_v8["inventory"]["suites"]},
        "toolchains": {"arm": subprocess.check_output([arm_cc, "--version"], text=True).splitlines()[0],
                       "wasm": subprocess.check_output([emcc, "--version"], text=True).splitlines()[0],
                       "emscriptenVersion": (Path(emcc).parent / "emscripten-version.txt").read_text().strip()},
        "flags": {"common": common, "arm": ["-" + args.arm_opt, *arm_flags], "wasm": ["-O2", *wasm_flags],
                  "armEffective": arm_common + arm_flags, "wasmEffective": wasm_common + wasm_flags},
        "configuration": {"dispatcher": {"arm": args.arm_dispatch, "wasm": "switch"}, "atomics": False,
                          "quickjsMemoryLimitBytes": 8388608, "officialQuickjsMemoryLimitBytes": 201326592,
                          "filesystem": False, "timezone": "UTC-only adaptation on both targets",
                          "targets": {
                              "arm": {"optimization": args.arm_opt, "dispatch": args.arm_dispatch,
                                      "lto": args.arm_lto, "isa": args.arm_isa, "architecture": "armv7-a",
                                      "noGcse": args.arm_no_gcse, "noCrossjumping": args.arm_no_crossjumping,
                                      "fpu": "vfpv3", "floatAbi": "softfp", "platformMacro": "EMSCRIPTEN",
                                      "atomics": False, "stackChecks": False,
                                      "engineSource": record(arm_engine, arm_engine.name)},
                              "wasm": {"optimization": "O2", "dispatch": "switch", "lto": False,
                                       "isa": "wasm32", "platformMacro": "EMSCRIPTEN", "atomics": False,
                                       "stackChecks": False, "engineSource": record(engine_path, engine_path.name)},
                          }},
        "reference": {"source": "examples/quickjs/reference.py", "method": "Independent Python integer LCG, trial-division prime count and Decimal Leibniz sum", "results": {
            "light": {"lcg": 2778886796, "sieve": 1028, "pi": 3141542},
            "full": {"lcg": 428440788, "sieve": 6542, "pi": 3141587}}},
    }
    (dist / "quickjs-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built QuickJS {VERSION} ARM32 + Wasm in {dist}")


if __name__ == "__main__":
    main()
