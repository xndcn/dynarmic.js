#!/usr/bin/env python3
"""Build pinned QuickJS/newlib as a bare-metal i386 Multiboot guest for v86."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
EXAMPLE = HERE.parent
ROOT = EXAMPLE.parent.parent
NEWLIB_VERSION = "3.3.0"
NEWLIB_URL = "https://sourceware.org/pub/newlib/newlib-3.3.0.tar.gz"
NEWLIB_SHA256 = "58dd9e3eaedf519360d92d84205c3deef0b3fc286685d1c562e245914ef72c66"
QUICKJS_VERSION = "2025-09-13"
QUICKJS_SHA256 = "996c6b5018fc955ad4d06426d0e9cb713685a00c825aa5c0418bd53f7df8b0b4"
ARCH_FLAGS = ["-m32", "-march=pentium4", "-msse2", "-mfpmath=sse", "-ffreestanding",
              "-fno-pie", "-fno-stack-protector", "-U__linux__", "-U__linux", "-Ulinux",
              "-U__gnu_linux__", "-U__unix__", "-U__unix", "-Uunix", "-U_FORTIFY_SOURCE"]


def record(path):
    path = Path(path)
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def run(command, *, cwd=None, env=None, log=None):
    command = list(map(str, command))
    print("+ " + shlex.join(command), flush=True)
    if log:
        with Path(log).open("w") as output:
            subprocess.run(command, cwd=cwd, env=env, check=True, stdout=output, stderr=subprocess.STDOUT)
    else:
        subprocess.run(command, cwd=cwd, env=env, check=True)


def executable(name):
    found = shutil.which(name)
    if not found:
        raise SystemExit("Missing required build tool: " + name)
    return str(Path(found).resolve())


def extract(archive, directory):
    directory.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            target = (directory / member.name).resolve()
            if not target.is_relative_to(directory.resolve()):
                raise SystemExit("Unsafe source archive member: " + member.name)
            if member.issym() or member.islnk():
                link = (target.parent / member.linkname if member.issym() else directory / member.linkname).resolve()
                if not link.is_relative_to(directory.resolve()):
                    raise SystemExit("Unsafe source archive link: " + member.name)
        bundle.extractall(directory)


def checked_replace(text, before, after, description):
    if text.count(before) != 1:
        raise SystemExit("Source adaptation no longer matches: " + description)
    return text.replace(before, after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build-v86-quickjs")
    parser.add_argument("--base-dist", type=Path, default=ROOT / "build-em/examples/browser/dist")
    parser.add_argument("--cc", default="gcc-11")
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=2)
    args = parser.parse_args()
    output, base = args.output.resolve(), args.base_dist.resolve()
    if output == base or output.is_relative_to(base):
        raise SystemExit("The v86 output must be outside the existing ARM distribution")
    cache, guest = output / "cache", output / "guest"
    cache.mkdir(parents=True, exist_ok=True)
    guest.mkdir(parents=True, exist_ok=True)
    cc = executable(args.cc)
    tools = {name: executable(name) for name in ["ar", "ranlib", "as", "ld", "nm", "objcopy", "readelf", "make"]}
    libgcc = Path(subprocess.check_output([cc, "-m32", "-print-file-name=libgcc.a"], text=True).strip()).resolve()
    if not libgcc.is_file() or "/32/" not in str(libgcc):
        raise SystemExit("A working i386 libgcc is required; use GCC 11 with its installed multilib runtime")
    compiler_include = subprocess.check_output([cc, "-m32", "-print-file-name=include"], text=True).strip()
    archive = cache / f"newlib-{NEWLIB_VERSION}.tar.gz"
    if not archive.exists():
        print("Downloading pinned newlib source: " + NEWLIB_URL, flush=True)
        with urllib.request.urlopen(NEWLIB_URL, timeout=180) as response:
            data = response.read(32 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != NEWLIB_SHA256:
            raise SystemExit("newlib source checksum mismatch")
        archive.write_bytes(data)
    if record(archive)["sha256"] != NEWLIB_SHA256:
        raise SystemExit("newlib source checksum mismatch")
    newlib_source = cache / f"newlib-{NEWLIB_VERSION}"
    if not (newlib_source / "configure").is_file():
        extract(archive, cache)
    wrappers = output / "toolchain/bin"
    wrappers.mkdir(parents=True, exist_ok=True)
    wrapper_commands = {"gcc": [cc, *ARCH_FLAGS, "-nostdinc", "-isystem", compiler_include], "as": [tools["as"], "--32"],
                        "ld": [tools["ld"], "-melf_i386"]}
    wrapper_commands.update({name: [tools[name]] for name in ["ar", "ranlib", "nm", "objcopy", "readelf"]})
    for name, command in wrapper_commands.items():
        script = wrappers / ("i686-elf-" + name)
        script.write_text("#!/bin/sh\nexec " + shlex.join(command) + ' "$@"\n')
        script.chmod(0o755)
    env = dict(os.environ)
    env["PATH"] = str(wrappers) + os.pathsep + env.get("PATH", "")
    env.update({"CC_FOR_TARGET": str(wrappers / "i686-elf-gcc"),
                "AR_FOR_TARGET": str(wrappers / "i686-elf-ar"),
                "RANLIB_FOR_TARGET": str(wrappers / "i686-elf-ranlib"),
                "AS_FOR_TARGET": str(wrappers / "i686-elf-as"),
                "LD_FOR_TARGET": str(wrappers / "i686-elf-ld"),
                "CFLAGS_FOR_TARGET": "-O2 -g0 -fno-fast-math"})
    newlib_build, prefix = output / "newlib-build", output / "newlib-install"
    newlib_build.mkdir(parents=True, exist_ok=True)
    stamp = output / "newlib-build-manifest.json"
    config = [str(newlib_source / "configure"), "--target=i686-elf", "--prefix=" + str(prefix),
              "--disable-multilib", "--disable-newlib-supplied-syscalls", "--disable-libgloss", "--disable-nls"]
    identity = {"sourceSha256": NEWLIB_SHA256, "compiler": record(cc), "archFlags": ARCH_FLAGS,
                "cflags": env["CFLAGS_FOR_TARGET"], "configure": config}
    installed = [prefix / "i686-elf/lib/libc.a", prefix / "i686-elf/lib/libm.a"]
    previous = json.loads(stamp.read_text()) if stamp.exists() else None
    current = previous and previous.get("identity") == identity and all(p.is_file() for p in installed)
    if current:
        current = previous.get("libraries") == [record(p) for p in installed]
    if not current:
        if (newlib_build / "Makefile").exists():
            shutil.rmtree(newlib_build)
            newlib_build.mkdir()
        run(config, cwd=newlib_build, env=env, log=output / "newlib-configure.log")
        run([tools["make"], "-j" + str(args.jobs), "all-target-newlib"], cwd=newlib_build, env=env, log=output / "newlib-build.log")
        run([tools["make"], "install-target-newlib"], cwd=newlib_build, env=env, log=output / "newlib-install.log")
        stamp.write_text(json.dumps({"identity": identity, "libraries": [record(p) for p in installed]}, indent=2) + "\n")
    qjs_archive = base / "quickjs-source.tar.xz"
    if record(qjs_archive)["sha256"] != QUICKJS_SHA256:
        raise SystemExit("The existing distribution does not contain the pinned QuickJS archive")
    qjs_root = cache / "quickjs-source"
    qjs_source = qjs_root / ("quickjs-" + QUICKJS_VERSION)
    if qjs_source.exists():
        shutil.rmtree(qjs_source)
    extract(qjs_archive, qjs_root)
    engine = qjs_source / "quickjs.c"
    engine.write_text(checked_replace(engine.read_text(),
        "        localtime_r(&ti, &tm);\n        res = -tm.tm_gmtoff / 60;",
        "        (void)tm; (void)ti;\n        res = 0; /* Bare-metal demo: UTC on both targets. */", "UTC timezone"))
    derived = output / "derived"
    derived.mkdir(parents=True, exist_ok=True)
    runner_text = checked_replace((EXAMPLE / "runner.c").read_text(),
        '    register uint32_t value __asm__("r0");\n    __asm__ volatile("svc #4" : "=r"(value) :: "memory");',
        '    uint32_t value;\n    const uint16_t port = 0xe100;\n    __asm__ volatile("inl %w1, %0" : "=a"(value) : "d"(port) : "memory");',
        "host monotonic clock transport")
    (derived / "runner-x86.c").write_text(runner_text)
    syscalls = checked_replace((EXAMPLE / "syscalls.c").read_text(),
        '        register uint32_t value __asm__("r0") = bytes[i];\n        __asm__ volatile("svc #1" :: "r"(value) : "memory");',
        '        __asm__ volatile("outb %0, %w1" :: "a"(bytes[i]), "d"((uint16_t)0xe104) : "memory");',
        "console transport")
    syscalls = checked_replace(syscalls,
        '    register uint32_t value __asm__("r0") = (uint32_t)status;\n    __asm__ volatile("svc #0" :: "r"(value) : "memory");\n    for (;;) {}',
        '    __asm__ volatile("outl %0, %w1" :: "a"((uint32_t)status), "d"((uint16_t)0xe108) : "memory");\n    for (;;) __asm__ volatile("cli; hlt");',
        "exit transport")
    # i686-elf newlib selects MISSING_SYSCALL_NAMES: its reentrant wrappers
    # request write/sbrk/etc. Reuse its canonical ABI mapping without changing
    # any of the ARM host's syscall bodies or bounded heap behavior.
    syscalls = '#define MISSING_SYSCALL_NAMES 1\n#include <_syslist.h>\n' + syscalls
    (derived / "syscalls-x86.c").write_text(syscalls)
    workload = (EXAMPLE / "workloads.js").read_bytes()
    if workload != (base / "quickjs-workloads.js").read_bytes():
        raise SystemExit("The baseline workload asset differs from the runner's embedded source")
    (derived / "workloads.h").write_text("static const unsigned char workloads_js[] = {\n" +
        ",".join(str(byte) for byte in workload + b"\0") + "\n};\n")
    # Match the ARM QuickJS application's hosted/builtin compiler semantics.
    # Bare-metal loading and libc isolation come from our explicit linker and
    # header paths; only newlib's own build uses the freestanding wrapper.
    guest_arch_flags = [flag for flag in ARCH_FLAGS if flag != "-ffreestanding"]
    flags = [*guest_arch_flags, "-O2", "-fwrapv", "-fno-strict-aliasing", "-fno-fast-math", "-D_GNU_SOURCE",
             '-DCONFIG_VERSION="' + QUICKJS_VERSION + '"', "-DEMSCRIPTEN", "-nostdinc",
             "-isystem", str(prefix / "i686-elf/include"), "-isystem", compiler_include,
             "-I" + str(qjs_source), "-I" + str(derived), "-ffunction-sections", "-fdata-sections"]
    sources = [qjs_source / name for name in ["quickjs.c", "dtoa.c", "libregexp.c", "libunicode.c", "cutils.c"]]
    sources += [derived / "runner-x86.c", derived / "syscalls-x86.c", HERE / "start.S"]
    elf = guest / "quickjs-x86.elf"
    command = [cc, *flags, *sources, "-nostdlib", "-nostartfiles", "-no-pie", "-Wl,--gc-sections",
               "-Wl,--build-id=none", "-Wl,-Map=" + str(guest / "quickjs-x86.map"), "-T", str(HERE / "link.ld"),
               "-Wl,--start-group", str(installed[1]), str(installed[0]), str(libgcc), "-Wl,--end-group", "-o", str(elf)]
    run(command, log=output / "quickjs-build.log")
    macro_output = subprocess.check_output([cc, *flags, "-dM", "-E", str(engine)], text=True)
    (derived / "quickjs-platform-macros.txt").write_text(macro_output)
    for required in ["#define EMSCRIPTEN 1", "#define DIRECT_DISPATCH 0", "#define __STDC_HOSTED__ 1"]:
        if required not in macro_output:
            raise SystemExit("Missing intended QuickJS platform macro: " + required)
    for forbidden in ["#define CONFIG_ATOMICS", "#define CONFIG_STACK_CHECK", "#define __linux"]:
        if forbidden in macro_output:
            raise SystemExit("Unexpected QuickJS platform feature: " + forbidden)
    run([tools["readelf"], "-h", "-l", elf], log=guest / "quickjs-x86-elf.txt")
    asset_names = ["quickjs-workloads.js", "official-v8.js", "official-v8-adapter.js", "official-benchmarks.json",
                   "quickjs-extras-LICENSE.txt"]
    for name in asset_names:
        shutil.copyfile(base / name, guest / name)
    shutil.copyfile(newlib_source / "COPYING.NEWLIB", guest / "newlib-LICENSE.txt")
    shutil.copyfile(qjs_source / "LICENSE", guest / "quickjs-LICENSE.txt")
    for name in ["GPL-3.0.txt", "GCC-Runtime-Exception-3.1.txt"]:
        shutil.copyfile(EXAMPLE / "licenses" / name, guest / name)
    manifest = {
        "schema": "dynarmic.js/quickjs-v86-build-v1", "quickjs": {"version": QUICKJS_VERSION, "archive": record(qjs_archive)},
        "newlib": {"version": NEWLIB_VERSION, "url": NEWLIB_URL, "archive": record(archive), "build": json.loads(stamp.read_text())},
        "toolchain": {"version": subprocess.check_output([cc, "--version"], text=True).splitlines()[0], "compiler": record(cc), "libgcc": record(libgcc)},
        "configuration": {"optimization": "O2", "dispatch": "switch", "isa": "i386-pentium4", "lto": False,
                          "stdcHosted": True, "compilerBuiltins": "compiler defaults, matching the ARM QuickJS application",
                          "platformMacro": "EMSCRIPTEN", "atomics": False, "stackChecks": False,
                          "defaultMemoryLimitBytes": 8388608, "officialMemoryLimitBytes": 201326592,
                          "floatingPoint": "SSE2 C arithmetic; standard i386 ABI can use x87 returns and newlib assembly; no fast-math",
                          "x87ControlWord": "0x027f", "mxcsr": "0x1f80", "clock": "host monotonic microseconds, wrapping uint32 extended by the unchanged runner",
                          "ports": {"clockRead32": 57600, "consoleWrite8": 57604, "exitWrite32": 57608},
                          "mailboxAddress": 61440, "loadAddress": 1048576},
        "flags": flags, "command": list(map(str, command)),
        "sources": [record(p) for p in [HERE / "build.py", HERE / "start.S", HERE / "link.ld", EXAMPLE / "runner.c", EXAMPLE / "syscalls.c", EXAMPLE / "workloads.js"]],
        "derivedSources": [record(p) for p in [engine, derived / "runner-x86.c", derived / "syscalls-x86.c", derived / "workloads.h", derived / "quickjs-platform-macros.txt"]],
        "adaptations": ["Same UTC timezone adaptation as ARM", "Runner: replace only SVC clock read with inl(0xE100)",
                        "Syscalls: replace only SVC console/exit with outb(0xE104)/outl(0xE108)",
                        "Use newlib _syslist.h MISSING_SYSCALL_NAMES mapping for its i686-elf syscall symbol ABI",
                        "Multiboot startup/linker for i386; allocator, limits, wrap extension, JS eval and mailbox unchanged"],
        "artifacts": [record(guest / name) for name in ["quickjs-x86.elf", *asset_names, "newlib-LICENSE.txt", "quickjs-LICENSE.txt", "GPL-3.0.txt", "GCC-Runtime-Exception-3.1.txt"]],
        "runtimeValidated": False,
    }
    (guest / "quickjs-x86-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Built " + str(elf), flush=True)


if __name__ == "__main__":
    main()
