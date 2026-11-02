"""Source distribution checks using disposable Git repositories and real tar files."""

import importlib.util
import io
import json
import shlex
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "examples/browser/package_source.py"
spec = importlib.util.spec_from_file_location("package_source", SCRIPT)
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def git(root, *args):
    return subprocess.check_output(["git", "-c", "protocol.file.allow=always", "-C", str(root), *args], stderr=subprocess.PIPE).decode().strip()


def initialize(root, files):
    root.mkdir(parents=True)
    git(root, "init", "-q")
    git(root, "config", "user.name", "Source Package Test")
    git(root, "config", "user.email", "source@example.invalid")
    for name, text in files.items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    git(root, "add", ".")
    git(root, "commit", "-qm", "fixture")


class SourceArchiveTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pages-source-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "source checkout"
        self.upstream = self.base / "upstream"
        initialize(self.upstream, {"LICENSE.txt": "Upstream license\n", "source.cpp": "int upstream = 1;\n",
                                  "externals/catch/LICENSE.txt": "Boost Software License - Version 1.0\nTest license fixture\n"})
        initialize(self.root, {"LICENSE": "Project license\n", "THIRD_PARTY_NOTICES.md": "Notices\n",
                               "CMakeLists.txt": "# fixture\n", ".gitignore": "/build*/\n*.tmp\n",
                               "src/host.cpp": "int host = 1;\n", "examples/browser/index.html": "<p>Hello</p>\n"})
        git(self.root, "submodule", "add", "-q", str(self.upstream), "externals/dynarmic")
        git(self.root, "commit", "-qam", "upstream")
        self.dist = self.root / "build-pages/examples/browser/dist"
        self.dist.mkdir(parents=True)
        (self.dist / "arm_jit.js").write_text("runtime javascript\n")
        (self.dist / "arm_jit.wasm").write_bytes(b"\x00asm\x01\x00\x00\x00")
        self.sdk = self.base / "sdk/emscripten"
        self.sdk.mkdir(parents=True)
        (self.sdk / "emscripten-version.txt").write_text('"6.0.13"\n')
        for name in ("LICENSE", "system/lib/libc/musl/COPYRIGHT", "system/lib/libcxx/LICENSE.TXT",
                     "system/lib/libcxxabi/LICENSE.TXT", "system/lib/compiler-rt/LICENSE.TXT",
                     "system/lib/libunwind/LICENSE.TXT", "system/lib/llvm-libc/LICENSE.TXT"):
            target = self.sdk / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"Copyright retained for {name}\nTest license fixture\n")
        (self.sdk / "system/lib/dlmalloc.c").write_text("/*\n This is a version (aka dlmalloc) of malloc/free/realloc written by\n Doug Lea and released to the public domain.\n * Version 2.8.6\n * Quickstart\n*/\nint allocator;\n")
        self.write_build_manifest()

    def write_build_manifest(self, dirty=False):
        self.build = {"schema": "dynarmic.js/wasm-coremark-host-build-v1",
                      "source": {"revision": git(self.root, "rev-parse", "HEAD"),
                                 "upstream": git(self.root / "externals/dynarmic", "rev-parse", "HEAD"), "dirty": dirty},
                      "build": {"type": "Release", "profile": False, "emscriptenVersion": '"6.0.13"'},
                      "configuration": {}, "buildEnvironment": {},
                      "artifacts": {kind: pack.digest(self.dist / name) for kind, name in
                                    (("js", "arm_jit.js"), ("wasm", "arm_jit.wasm"))}}
        self.save_manifest()

    def save_manifest(self):
        (self.dist / "wasm-build-manifest.json").write_text(json.dumps(self.build))

    def package(self, clean=False):
        return pack.package_source(self.root, self.dist, clean, self.sdk)

    def read_archive(self):
        with tarfile.open(self.dist / "source.tar.gz", "r:gz") as archive:
            return {entry.name: archive.extractfile(entry).read() for entry in archive if entry.isfile()}

    def test_clean_recursive_source_matches_runtime_and_roundtrips(self):
        manifest = self.package(clean=True)
        files = self.read_archive()
        self.assertEqual(files["dynarmic.js/src/host.cpp"], b"int host = 1;\n")
        self.assertEqual(files["dynarmic.js/externals/dynarmic/LICENSE.txt"], b"Upstream license\n")
        self.assertEqual(files["dynarmic.js/externals/dynarmic/source.cpp"], b"int upstream = 1;\n")
        self.assertFalse(any("/.git/" in name or name.endswith("/.git") for name in files))
        self.assertFalse(any("/build-pages/" in name for name in files))
        self.assertTrue(manifest["correspondenceVerified"])
        self.assertEqual(manifest["archive"]["sha256"], pack.digest(self.dist / "source.tar.gz")["sha256"])
        self.assertEqual((self.dist / "LICENSE.txt").read_text(), "Project license\n")
        self.assertEqual((self.dist / "THIRD_PARTY_NOTICES.md").read_text(), "Notices\n")
        self.assertEqual(manifest["runtimeNotices"]["emscriptenVersion"], "6.0.13")
        self.assertEqual(manifest["runtimeNotices"]["sha256"], pack.digest(self.dist / "runtime-licenses.txt")["sha256"])
        notices = (self.dist / "runtime-licenses.txt").read_text()
        self.assertIn("Copyright retained for system/lib/libc/musl/COPYRIGHT", notices)
        self.assertIn("Doug Lea and released to the public domain", notices)
        self.assertIn("Boost Software License - Version 1.0", notices)
        self.assertNotIn(str(self.base), notices)
        self.assertNotIn(str(self.base), json.dumps(manifest["runtimeNotices"]))
        first = (self.dist / "source.tar.gz").read_bytes()
        self.package(clean=True)
        self.assertEqual((self.dist / "source.tar.gz").read_bytes(), first, "same tree packages reproducibly")

    def test_dirty_snapshot_uses_actual_files_including_new_source_and_deletions(self):
        (self.root / "src/host.cpp").write_text("int host = 2;\n")
        (self.root / "examples/browser/new.js").write_text("new code\n")
        (self.root / "examples/browser/index.html").unlink()
        (self.root / "ignored.tmp").write_text("temporary\n")
        (self.root / "externals/dynarmic/source.cpp").write_text("int upstream = 2;\n")
        manifest = self.package()
        files = self.read_archive()
        self.assertEqual(files["dynarmic.js/src/host.cpp"], b"int host = 2;\n")
        self.assertEqual(files["dynarmic.js/externals/dynarmic/source.cpp"], b"int upstream = 2;\n")
        self.assertIn("dynarmic.js/examples/browser/new.js", files)
        self.assertNotIn("dynarmic.js/examples/browser/index.html", files)
        self.assertNotIn("dynarmic.js/ignored.tmp", files)
        self.assertTrue(manifest["source"]["dirty"])
        self.assertFalse(manifest["runtimeBuild"]["source"]["dirty"], "runtime source state is not rewritten")
        self.assertFalse(manifest["correspondenceVerified"])
        with self.assertRaisesRegex(pack.PackageError, "clean checkout AND a clean runtime"):
            self.package(clean=True)

    def test_history_is_excluded_without_omitting_build_inputs_or_hiding_dirty_source(self):
        for name in ("internals/measurements/run/report.json", "internals/release-checks/run/output.txt",
                     "internals/CMakeLists.txt", "tests/guest/demo.c"):
            file = self.root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("fixture\n")
        manifest = self.package()
        files = self.read_archive()
        self.assertNotIn("dynarmic.js/internals/measurements/run/report.json", files)
        self.assertNotIn("dynarmic.js/internals/release-checks/run/output.txt", files)
        self.assertIn("dynarmic.js/internals/CMakeLists.txt", files)
        self.assertIn("dynarmic.js/tests/guest/demo.c", files)
        self.assertEqual(manifest["archive"]["excluded"], list(pack.EXCLUDED_ARCHIVE_PREFIXES))
        self.assertFalse(manifest["correspondenceVerified"])
        with self.assertRaisesRegex(pack.PackageError, "clean checkout"):
            self.package(clean=True)

    def test_clean_snapshot_cannot_relabel_a_dirty_runtime_build(self):
        self.write_build_manifest(dirty=True)
        manifest = self.package()
        self.assertFalse(manifest["source"]["dirty"])
        self.assertTrue(manifest["runtimeBuild"]["source"]["dirty"])
        self.assertFalse(manifest["correspondenceVerified"])
        with self.assertRaisesRegex(pack.PackageError, "clean checkout AND a clean runtime"):
            self.package(clean=True)

    def test_nested_submodule_source_is_included(self):
        leaf = self.base / "leaf"
        initialize(leaf, {"leaf.c": "int leaf = 3;\n", "LICENSE": "Leaf license\n"})
        upstream = self.root / "externals/dynarmic"
        git(upstream, "config", "user.name", "Source Package Test")
        git(upstream, "config", "user.email", "source@example.invalid")
        git(upstream, "submodule", "add", "-q", str(leaf), "externals/leaf")
        git(upstream, "commit", "-qam", "nested dependency")
        git(self.root, "add", "externals/dynarmic")
        git(self.root, "commit", "-qm", "updated dependency")
        self.write_build_manifest()
        manifest = self.package(clean=True)
        files = self.read_archive()
        self.assertEqual(files["dynarmic.js/externals/dynarmic/externals/leaf/leaf.c"], b"int leaf = 3;\n")
        self.assertEqual([entry["path"] for entry in manifest["source"]["submodules"]],
                         ["externals/dynarmic", "externals/dynarmic/externals/leaf"])

    def test_uninitialized_submodule_fails_instead_of_omitting_sources(self):
        git(self.root, "submodule", "deinit", "-q", "-f", "externals/dynarmic")
        with self.assertRaisesRegex(pack.PackageError, "initialize recursive submodules"):
            self.package()

    def test_runtime_tamper_revision_and_upstream_mismatches_fail(self):
        self.package(clean=True)
        original = (self.dist / "source.tar.gz").read_bytes()
        for field in ("revision", "upstream"):
            self.write_build_manifest()
            self.build["source"][field] = "0" * 40
            self.save_manifest()
            with self.assertRaisesRegex(pack.PackageError, "revision"):
                self.package()
        self.write_build_manifest()
        (self.dist / "arm_jit.wasm").write_bytes(b"tampered")
        with self.assertRaisesRegex(pack.PackageError, "does not match its runtime build manifest"):
            self.package()
        self.assertEqual((self.dist / "source.tar.gz").read_bytes(), original, "failed packaging preserves prior artifact")

    def test_git_configuration_cannot_hide_untracked_source(self):
        git(self.root, "config", "status.showUntrackedFiles", "no")
        (self.root / "src/extra.cpp").write_text("int extra;\n")
        with self.assertRaisesRegex(pack.PackageError, "clean checkout"):
            self.package(clean=True)
        self.package()
        self.assertIn("dynarmic.js/src/extra.cpp", self.read_archive())

    def test_safe_symlink_is_preserved_and_escaping_symlink_rejected(self):
        (self.root / "src/link.cpp").symlink_to("host.cpp")
        self.package()
        with tarfile.open(self.dist / "source.tar.gz", "r:gz") as archive:
            link = archive.getmember("dynarmic.js/src/link.cpp")
            self.assertTrue(link.issym())
            self.assertEqual(link.linkname, "host.cpp")
        (self.root / "src/link.cpp").unlink()
        (self.root / "src/link.cpp").symlink_to(self.base / "outside")
        (self.base / "outside").write_text("must not be packaged\n")
        with self.assertRaisesRegex(pack.PackageError, "absolute symlink"):
            self.package()
        (self.root / "src/link.cpp").unlink()
        (self.root / "src/link.cpp").symlink_to("../../outside")
        with self.assertRaisesRegex(pack.PackageError, "escapes the source tree"):
            self.package()

    def test_sdk_version_and_all_required_licenses_must_match_even_for_local_snapshots(self):
        self.package(clean=True)
        original = (self.dist / "source-manifest.json").read_bytes()
        (self.sdk / "emscripten-version.txt").write_text('"6.0.12"\n')
        for clean in (False, True):
            with self.assertRaisesRegex(pack.PackageError, "differs from runtime build"):
                self.package(clean=clean)
        (self.sdk / "emscripten-version.txt").write_text('"6.0.13"\n')
        for relative in ("LICENSE", "system/lib/libc/musl/COPYRIGHT", "system/lib/libcxx/LICENSE.TXT",
                         "system/lib/libcxxabi/LICENSE.TXT", "system/lib/compiler-rt/LICENSE.TXT"):
            file = self.sdk / relative
            content = file.read_text()
            file.unlink()
            with self.assertRaisesRegex(pack.PackageError, "cannot collect complete runtime license notices"):
                self.package(clean=True)
            file.write_text(content)
        self.assertEqual((self.dist / "source-manifest.json").read_bytes(), original)

    def test_sdk_discovery_uses_explicit_directory_then_emcc_then_build_cache(self):
        self.assertEqual(pack.emscripten_directory(self.dist, self.sdk), self.sdk)
        with mock.patch.object(pack.shutil, "which", return_value=str(self.sdk / "emcc")):
            self.assertEqual(pack.emscripten_directory(self.dist, None), self.sdk)
        cache = self.dist.parents[2] / "CMakeCache.txt"
        cache.write_text(f"CMAKE_TOOLCHAIN_FILE:FILEPATH={self.sdk}/cmake/Modules/Platform/Emscripten.cmake\n")
        with mock.patch.object(pack.shutil, "which", return_value=None):
            self.assertEqual(pack.emscripten_directory(self.dist, None), self.sdk)
            pack.package_source(self.root, self.dist, True)
            cache.unlink()
            with self.assertRaisesRegex(pack.PackageError, "pass --emscripten-root"):
                pack.package_source(self.root, self.dist, True)

    def c_bench_fixture(self):
        source = self.root / "examples/c-bench/build.py"
        source.parent.mkdir(parents=True)
        source.write_text("# C benchmark build fixture\n")
        kernel = source.parent / "kernels.c"
        kernel.write_text("unsigned benchmark(void) { return 42; }\n")
        sources = [source, kernel]
        for name in ("reference.py", "kernels.h", "guest.c", "start-arm.S", "start-x86.S", "link-arm.ld", "link-x86.ld",
                     "licenses/v86-third-party.txt"):
            file = source.parent / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(f"fixture source: {name}\n")
            sources.append(file)
        names = ("c-bench-arm.bin", "c-bench-x86.elf", "c-bench.wasm", "c-bench-reference.json",
                 "c-bench-source.c", "c-bench-v86.js", "c-bench-v86.wasm", "c-bench-v86-LICENSE.txt")
        for name in names:
            (self.dist / name).write_bytes(f"fixture: {name}\n".encode())
        manifest = {"schema": "dynarmic.js/c-bench-build-v1", "compiler": {"version": "fixture"},
                    "artifacts": [{"path": name, **pack.digest(self.dist / name)} for name in names],
                    "sources": [{"path": path.relative_to(self.root).as_posix(), **pack.digest(path)}
                                for path in sources]}
        (self.dist / "c-bench-manifest.json").write_text(json.dumps(manifest))
        return manifest

    def test_c_benchmark_packages_current_sources_artifacts_and_compiler_metadata(self):
        self.assertNotIn("cBenchBuild", self.package(), "Older source fixtures may omit the C lab")
        build = self.c_bench_fixture()
        manifest = self.package()
        self.assertEqual(manifest["cBenchBuild"]["compiler"], build["compiler"])
        for entry in build["artifacts"]:
            self.assertEqual(manifest["cBenchBuild"]["artifacts"][entry["path"]],
                             pack.digest(self.dist / entry["path"]))
        self.assertEqual(manifest["cBenchBuild"]["manifest"]["sha256"],
                         pack.digest(self.dist / "c-bench-manifest.json")["sha256"])
        self.assertIn("dynarmic.js/examples/c-bench/kernels.c", self.read_archive())
        self.assertIn("dynarmic.js/examples/c-bench/licenses/v86-third-party.txt", self.read_archive())

    def test_c_benchmark_missing_manifest_or_artifact_cannot_silently_omit_the_lab(self):
        build = self.c_bench_fixture()
        self.package()
        original = (self.dist / "source-manifest.json").read_bytes()
        target = self.dist / "c-bench-manifest.json"
        target.unlink()
        with self.assertRaisesRegex(pack.PackageError, "cannot validate C benchmark"):
            self.package()
        target.write_text(json.dumps(build))
        for entry in build["artifacts"]:
            file = self.dist / entry["path"]
            content = file.read_bytes()
            file.unlink()
            with self.assertRaisesRegex(pack.PackageError, "cannot validate C benchmark"):
                self.package()
            file.write_bytes(content)
        self.assertEqual((self.dist / "source-manifest.json").read_bytes(), original)

    def test_c_benchmark_rejects_artifact_tampering_and_stale_sources(self):
        build = self.c_bench_fixture()
        for entry in build["artifacts"]:
            file = self.dist / entry["path"]
            content = file.read_bytes()
            file.write_bytes(b"changed")
            with self.assertRaisesRegex(pack.PackageError, "does not match the C benchmark"):
                self.package()
            file.write_bytes(content)
        for entry in build["sources"]:
            file = self.root / entry["path"]
            content = file.read_bytes()
            file.write_bytes(b"changed source")
            with self.assertRaisesRegex(pack.PackageError, "changed since the build"):
                self.package()
            file.write_bytes(content)

    def test_c_benchmark_requires_complete_safe_manifest_and_nonempty_license(self):
        build = self.c_bench_fixture()
        target = self.dist / "c-bench-manifest.json"
        for change, message in (
            ({"sources": build["sources"][1:]}, "omits its build script"),
            ({"sources": build["sources"][:-1]}, "omits required kernel/reference/guest sources"),
            ({"artifacts": build["artifacts"][:-1]}, "omits required runtime/source/license"),
            ({"artifacts": [build["artifacts"][0], build["artifacts"][0]]}, "invalid C benchmark artifact path"),
            ({"artifacts": [{"path": "../outside", "sha256": "0" * 64, "bytes": 1}]}, "unsafe source path"),
            ({"sources": [{"path": "src/host.cpp", **pack.digest(self.root / "src/host.cpp")}]}, "invalid C benchmark source path"),
        ):
            target.write_text(json.dumps({**build, **change}))
            with self.assertRaisesRegex(pack.PackageError, message):
                self.package()
        license_file = self.dist / "c-bench-v86-LICENSE.txt"
        license_file.write_text("\n")
        for entry in build["artifacts"]:
            if entry["path"] == license_file.name:
                entry.update(pack.digest(license_file))
        target.write_text(json.dumps(build))
        with self.assertRaisesRegex(pack.PackageError, "v86 license is empty"):
            self.package()

    def test_c_benchmark_mutation_during_archiving_preserves_previous_package(self):
        self.c_bench_fixture()
        self.package()
        original = (self.dist / "source-manifest.json").read_bytes()
        write_archive = pack.write_archive

        def change_after_archive(*args):
            write_archive(*args)
            (self.dist / "c-bench.wasm").write_bytes(b"rebuilt during packaging")

        with mock.patch.object(pack, "write_archive", side_effect=change_after_archive):
            with self.assertRaisesRegex(pack.PackageError, "does not match the C benchmark"):
                self.package()
        self.assertEqual((self.dist / "source-manifest.json").read_bytes(), original)

    def quickjs_fixture(self):
        source = self.root / "examples/quickjs/build.py"
        source.parent.mkdir(parents=True)
        source.write_text("# QuickJS build fixture\n")
        names = ("quickjs-arm.bin", "quickjs-wasm.js", "quickjs-wasm.wasm",
                 "quickjs-workloads.js", "quickjs-LICENSE.txt", "quickjs-source.tar.xz", "quickjs-runtime-licenses.txt")
        for name in names:
            (self.dist / name).write_bytes(f"fixture: {name}\n".encode())
        manifest = {"schema": "dynarmic.js/quickjs-build-v1",
                    "upstream": {"archive": "quickjs-source.tar.xz",
                                 "sha256": pack.digest(self.dist / "quickjs-source.tar.xz")["sha256"]},
                    "artifacts": [{"path": name, **pack.digest(self.dist / name)} for name in names],
                    "sources": [{"path": "examples/quickjs/build.py", **pack.digest(source)}],
                    "toolchains": {"emscriptenVersion": "6.0.13"}, "flags": {}, "runtimeNotices": {}}
        (self.dist / "quickjs-manifest.json").write_text(json.dumps(manifest))
        return manifest

    def test_quickjs_build_requires_matching_runtime_and_complete_upstream_source(self):
        self.quickjs_fixture()
        manifest = self.package()
        self.assertIn("quickjsBuild", manifest)
        self.assertEqual(manifest["rebuild"]["cmakeCacheOptions"], {})
        self.assertFalse(any("DYNARMIC_QUICKJS_ARM_" in command for command in manifest["rebuild"]["commands"]))
        self.assertIn("dynarmic.js/examples/quickjs/build.py", self.read_archive())
        self.assertEqual(manifest["quickjsBuild"]["artifacts"]["quickjs-source.tar.xz"],
                         pack.digest(self.dist / "quickjs-source.tar.xz"))
        for name in ("quickjs-arm.bin", "quickjs-wasm.js", "quickjs-wasm.wasm",
                     "quickjs-workloads.js", "quickjs-LICENSE.txt", "quickjs-source.tar.xz", "quickjs-runtime-licenses.txt"):
            file = self.dist / name
            original = file.read_bytes()
            file.write_bytes(b"changed")
            with self.assertRaisesRegex(pack.PackageError, "does not match the QuickJS build manifest"):
                self.package()
            file.write_bytes(original)
        (self.root / "examples/quickjs/build.py").write_text("# modified build\n")
        with self.assertRaisesRegex(pack.PackageError, "changed since the build"):
            self.package()

    def test_quickjs_manifest_cannot_omit_source_or_escape_the_distribution(self):
        manifest = self.quickjs_fixture()
        target = self.dist / "quickjs-manifest.json"
        for change, message in (
            ({"sources": []}, "omits its build script"),
            ({"artifacts": manifest["artifacts"][:-1]}, "omits required runtime/source"),
            ({"artifacts": [{"path": "../outside", "sha256": "0" * 64, "bytes": 1}]}, "unsafe source path"),
            ({"toolchains": {"emscriptenVersion": "6.0.12"}}, "different Emscripten SDK"),
        ):
            target.write_text(json.dumps({**manifest, **change}))
            with self.assertRaisesRegex(pack.PackageError, message):
                self.package()
        target.unlink()
        with self.assertRaisesRegex(pack.PackageError, "cannot validate QuickJS"):
            self.package()

    def test_custom_arm_configuration_is_rebuilt_explicitly_and_unsafe_values_are_rejected(self):
        build = self.quickjs_fixture()
        arm = {"optimization": "O3", "dispatch": "threaded", "isa": "thumb", "lto": True,
               "noGcse": False, "noCrossjumping": True,
               "engineSource": {"path": "quickjs-arm.c", "sha256": "a" * 64},
               "ignoredArgument": "; unexpected shell text"}
        build["configuration"] = {"targets": {"arm": arm, "wasm": {"optimization": "O2"}}}
        target = self.dist / "quickjs-manifest.json"
        target.write_text(json.dumps(build))
        packaged = self.package()
        expected = {"DYNARMIC_QUICKJS_ARM_OPT": "O3", "DYNARMIC_QUICKJS_ARM_DISPATCH": "threaded",
                    "DYNARMIC_QUICKJS_ARM_ISA": "thumb", "DYNARMIC_QUICKJS_ARM_LTO": "ON",
                    "DYNARMIC_QUICKJS_ARM_NO_GCSE": "OFF", "DYNARMIC_QUICKJS_ARM_NO_CROSSJUMPING": "ON"}
        self.assertEqual(packaged["rebuild"]["cmakeCacheOptions"], expected)
        self.assertEqual(packaged["quickjsBuild"]["configuration"]["targets"]["arm"], arm)
        command = next(command for command in packaged["rebuild"]["commands"] if command.startswith("emcmake "))
        arguments = shlex.split(command)
        self.assertEqual([argument for argument in arguments if argument.startswith("-DDYNARMIC_QUICKJS_ARM_")],
                         [f"-D{key}={value}" for key, value in expected.items()])
        self.assertNotIn("unexpected shell text", command)
        original_package = (self.dist / "source-manifest.json").read_bytes()
        for field, value in (("optimization", "O3; injected"), ("dispatch", "$(injected)"),
                             ("isa", "thumb\ncommand"), ("lto", "ON"), ("noGcse", 1),
                             ("noCrossjumping", None)):
            with self.subTest(field=field, value=value):
                build["configuration"]["targets"]["arm"] = {**arm, field: value}
                target.write_text(json.dumps(build))
                with self.assertRaisesRegex(pack.PackageError, "invalid QuickJS ARM"):
                    self.package()
                self.assertEqual((self.dist / "source-manifest.json").read_bytes(), original_package)

    def official_fixture(self):
        quickjs = self.quickjs_fixture()
        cases = [("Richards", ["Richards"]), ("DeltaBlue", ["DeltaBlue"]),
                 ("Crypto", ["Encrypt", "Decrypt"]), ("RayTrace", ["RayTrace"]),
                 ("EarleyBoyer", ["Earley", "Boyer"]), ("RegExp", ["RegExp"]),
                 ("Splay", ["Splay"]), ("NavierStokes", ["NavierStokes"])]
        original = "\n".join("new BenchmarkSuite('" + suite + "', 1, [" +
                              ",".join("new Benchmark('" + name + "', run)" for name in names) + "]);"
                              for suite, names in cases).encode()
        archive = self.dist / "quickjs-extras-source.tar.xz"
        with tarfile.open(archive, "w:xz") as tar:
            member = tarfile.TarInfo("quickjs-2025-09-13/tests/bench-v8/combined.js")
            member.size = len(original)
            tar.addfile(member, io.BytesIO(original))
        (self.dist / "official-v8-upstream.js").write_bytes(original)
        (self.dist / "official-v8.js").write_bytes(original)
        (self.dist / "official-v8-adapter.js").write_text("// fixture adapter\n")
        license_source = self.root / "examples/quickjs/licenses/GPL-2.0.txt"
        license_source.parent.mkdir(parents=True)
        canonical_license = Path(pack.__file__).parents[1] / "quickjs/licenses/GPL-2.0.txt"
        license_source.write_bytes(canonical_license.read_bytes())
        (self.root / "examples/quickjs/official_benchmarks.py").write_text("# official fixture\n")
        license_text = self.dist / "quickjs-extras-LICENSE.txt"
        license_text.write_bytes(license_source.read_bytes())
        inventory = {"schema": "dynarmic.js/official-benchmarks-v1", "sources": [{"archive": archive.name,
            "sha256": pack.digest(archive)["sha256"], "upstreamCombinedSha256": pack.digest(self.dist / "official-v8-upstream.js")["sha256"]}],
            "suites": [{"id": suite, "benchmarks": names} for suite, names in cases],
            "cases": [{"id": suite + "/" + name} for suite, names in cases for name in names]}
        (self.dist / "official-benchmarks.json").write_text(json.dumps(inventory))
        quickjs["artifacts"].extend({"path": name, **pack.digest(self.dist / name)} for name in
            ["official-v8-upstream.js", "official-v8.js", "official-v8-adapter.js", "official-benchmarks.json",
             archive.name, license_text.name])
        quickjs["sources"].extend({"path": name, **pack.digest(self.root / name)} for name in
            ["examples/quickjs/official_benchmarks.py", "examples/quickjs/licenses/GPL-2.0.txt"])
        (self.dist / "quickjs-manifest.json").write_text(json.dumps(quickjs))
        for old, new in [("arm_jit.js", "arm_bench_jit.js"), ("arm_jit.wasm", "arm_bench_jit.wasm")]:
            (self.dist / new).write_bytes((self.dist / old).read_bytes())
        (self.dist / "arm-bench-build-manifest.json").write_text(json.dumps(self.build))
        return inventory, quickjs

    def test_official_suite_packages_pristine_source_complete_inventory_and_second_host(self):
        self.official_fixture()
        manifest = self.package()
        self.assertEqual(len(manifest["quickjsBuild"]["officialBenchmarks"]["inventory"]["cases"]), 10)
        self.assertIn("arm_bench_jit.wasm", manifest["officialHostBuild"]["artifacts"])
        (self.dist / "arm_bench_jit.wasm").write_bytes(b"wrong host")
        with self.assertRaisesRegex(pack.PackageError, "does not match its runtime build manifest"):
            self.package()

    def test_official_suite_requires_the_distributed_deltablue_gpl2_text(self):
        self.official_fixture()
        self.package()
        target = self.dist / "quickjs-extras-LICENSE.txt"
        target.write_text("GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n")
        manifest_file = self.dist / "quickjs-manifest.json"
        manifest = json.loads(manifest_file.read_text())
        for entry in manifest["artifacts"]:
            if entry["path"] == target.name:
                entry.update(pack.digest(target))
        manifest_file.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(pack.PackageError, "DeltaBlue GPL-2.0 license"):
            self.package()

    def test_official_suite_rejects_a_truncated_deltablue_gpl2_source(self):
        _, quickjs = self.official_fixture()
        self.package()
        source = self.root / "examples/quickjs/licenses/GPL-2.0.txt"
        source.write_text("GNU GENERAL PUBLIC LICENSE\nVersion 2, June 1991\n")
        target = self.dist / "quickjs-extras-LICENSE.txt"
        target.write_bytes(source.read_bytes())
        for entry in quickjs["artifacts"]:
            if entry["path"] == target.name:
                entry.update(pack.digest(target))
        for entry in quickjs["sources"]:
            if entry["path"] == "examples/quickjs/licenses/GPL-2.0.txt":
                entry.update(pack.digest(source))
        (self.dist / "quickjs-manifest.json").write_text(json.dumps(quickjs))
        with self.assertRaisesRegex(pack.PackageError, "DeltaBlue GPL-2.0 license source is incomplete"):
            self.package()

    def test_official_inventory_cannot_omit_duplicate_or_relabel_an_upstream_case(self):
        inventory, quickjs = self.official_fixture()
        for cases in [inventory["cases"][:-1], inventory["cases"][:-1] + inventory["cases"][:1],
                      [{"id": "different/case"}] + inventory["cases"][1:]]:
            (self.dist / "official-benchmarks.json").write_text(json.dumps({**inventory, "cases": cases}))
            for entry in quickjs["artifacts"]:
                if entry["path"] == "official-benchmarks.json": entry.update(pack.digest(self.dist / entry["path"]))
            (self.dist / "quickjs-manifest.json").write_text(json.dumps(quickjs))
            with self.assertRaisesRegex(pack.PackageError, "inventory omits, duplicates or changes"):
                self.package()

    def test_official_upstream_bytes_must_match_the_distributed_extras_archive(self):
        _, quickjs = self.official_fixture()
        (self.dist / "official-v8-upstream.js").write_bytes(b"silently changed benchmark")
        for entry in quickjs["artifacts"]:
            if entry["path"] == "official-v8-upstream.js": entry.update(pack.digest(self.dist / entry["path"]))
        (self.dist / "quickjs-manifest.json").write_text(json.dumps(quickjs))
        with self.assertRaisesRegex(pack.PackageError, "differs from its pristine extras archive"):
            self.package()

    def test_dirty_official_host_cannot_be_certified_by_a_clean_small_host(self):
        self.official_fixture()
        git(self.root, "add", ".")
        git(self.root, "commit", "-qm", "official example")
        self.write_build_manifest()
        official = {**self.build, "source": {**self.build["source"], "dirty": True}}
        (self.dist / "arm-bench-build-manifest.json").write_text(json.dumps(official))
        manifest = self.package()
        self.assertFalse(manifest["source"]["dirty"])
        self.assertFalse(manifest["runtimeBuild"]["source"]["dirty"])
        self.assertTrue(manifest["officialHostBuild"]["source"]["dirty"])
        self.assertFalse(manifest["correspondenceVerified"])
        with self.assertRaisesRegex(pack.PackageError, "clean checkout AND a clean runtime"):
            self.package(clean=True)


if __name__ == "__main__":
    unittest.main()
