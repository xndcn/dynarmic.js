"""Release handoff tests use real archives; no compilers or GitHub credentials."""
import copy
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("release", Path(__file__).resolve().parents[2] / "scripts/release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.pages = self.root / "pages"
        self.pages.mkdir()
        self.output = self.root / "release"
        self.package = self.root / "package.tgz"
        self.revision = "a" * 40
        self.version = release.read_json(release.ROOT / "packages/dynarmic/package.json")["version"]
        self.tag = "v" + self.version
        self.repository = "example/dynarmic.js"
        source = {"revision": self.revision, "dirty": False, "submodules": []}
        self.npm_files = {"package.json": json.dumps({"name": "dynarmic.js", "version": self.version}).encode(),
                          "runtime.wasm": b"\0asm", "source.tar.gz": b"matching source"}
        self.npm = {"schema": "dynarmic.js/npm-source-v1", "source": source,
                    "runtime": {"source": source}, "correspondenceVerified": True,
                    "files": {name: release.identity(data) for name, data in self.npm_files.items()}}
        self.write_package()
        for name in ("index.html", "LICENSE.txt", "THIRD_PARTY_NOTICES.md", "runtime-licenses.txt",
                     "source.tar.gz", "arm_jit.wasm", "arm_bench_jit.wasm", "quickjs-arm.bin", "c-bench.wasm"):
            (self.pages / name).write_text("fixture: " + name)
        self.source = {"schema": "dynarmic.js/pages-source-v1", "source": source,
                       "correspondenceVerified": True,
                       "archive": release.identity((self.pages / "source.tar.gz").read_bytes()),
                       "runtimeNotices": release.identity((self.pages / "runtime-licenses.txt").read_bytes())}
        for key, name in (("runtimeBuild", "arm_jit.wasm"), ("officialHostBuild", "arm_bench_jit.wasm"),
                          ("quickjsBuild", "quickjs-arm.bin"), ("cBenchBuild", "c-bench.wasm")):
            self.source[key] = {"source": copy.deepcopy(source), "artifacts": {
                name: release.identity((self.pages / name).read_bytes())}}
        self.write_source()

    def write_package(self):
        self.write_tar(self.package, {**self.npm_files, "source-manifest.json": json.dumps(self.npm).encode()}, "package/")

    def write_source(self):
        (self.pages / "source-manifest.json").write_text(json.dumps(self.source))

    def write_tar(self, path, files, prefix=""):
        with tarfile.open(path, "w:gz") as archive:
            for name, data in files.items():
                member = tarfile.TarInfo(prefix + name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))

    def bundle(self, output=None):
        return release.bundle(self.package, self.pages, output or self.output, self.tag, self.revision,
                              self.repository, "https://example.invalid/run/1")

    def extract(self, tag=None, revision=None):
        return release.extract_pages(self.output, self.root / "deployed", tag or self.tag,
                                     revision or self.revision, self.repository)

    def test_roundtrip_preserves_tested_bytes_and_portable_checksums(self):
        manifest = self.bundle()
        self.extract()
        for file in self.pages.iterdir():
            self.assertEqual(file.read_bytes(), (self.root / "deployed" / file.name).read_bytes())
        self.assertEqual(self.package.read_bytes(), (self.output / f"dynarmic.js-{self.version}.tgz").read_bytes())
        second = self.root / "second"
        self.assertEqual(manifest, self.bundle(second))
        for file in self.output.iterdir():
            self.assertEqual(file.read_bytes(), (second / file.name).read_bytes())
        for line in (self.output / "SHA256SUMS").read_text().splitlines():
            sha, name = line.split("  ")
            self.assertEqual(sha, release.identity((self.output / name).read_bytes())["sha256"])

    def test_mismatched_tag_version_and_revision_fail(self):
        with self.assertRaisesRegex(ValueError, "tag must match"):
            release.version_for("v99.0.0")
        self.npm_files["package.json"] = b'{"name":"dynarmic.js","version":"99.0.0"}'
        self.write_package()
        with self.assertRaisesRegex(ValueError, "name/version"):
            self.bundle()

    def test_dirty_or_mismatched_site_cannot_be_released(self):
        self.source["officialHostBuild"]["source"]["dirty"] = True
        self.write_source()
        with self.assertRaisesRegex(ValueError, "clean source"):
            self.bundle()
        self.assertFalse(self.output.exists())
        self.source["officialHostBuild"]["source"]["dirty"] = False
        self.source["source"]["revision"] = "b" * 40
        self.write_source()
        with self.assertRaisesRegex(ValueError, "tagged revision"):
            self.bundle()

    def test_modified_npm_and_pages_files_fail_before_release(self):
        self.npm_files["runtime.wasm"] = b"tampered"
        self.write_package()
        with self.assertRaisesRegex(ValueError, "checksum mismatch: runtime.wasm"):
            self.bundle()
        self.npm_files["runtime.wasm"] = b"\0asm"
        self.write_package()
        (self.pages / "quickjs-arm.bin").write_text("tampered")
        with self.assertRaisesRegex(ValueError, "checksum mismatch: quickjs-arm.bin"):
            self.bundle()

    def test_deployment_rejects_wrong_release_and_corrupt_download(self):
        self.bundle()
        for tag, revision in (("v99.0.0", None), (None, "b" * 40)):
            with self.assertRaisesRegex(ValueError, "release identity"):
                self.extract(tag, revision)
        name = f"dynarmic.js-{self.version}-pages.tar.gz"
        with (self.output / name).open("ab") as file:
            file.write(b"corrupt download")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.extract()
        self.assertFalse((self.root / "deployed").exists())

    def test_even_a_checksummed_archive_cannot_escape_deployment_directory(self):
        manifest = self.bundle()
        name = f"dynarmic.js-{self.version}-pages.tar.gz"
        for unsafe in ("../escaped", "/absolute", "a/../../escaped"):
            with self.subTest(unsafe=unsafe):
                self.write_tar(self.output / name, {unsafe: b"bad"})
                manifest["assets"][name] = release.identity((self.output / name).read_bytes())
                (self.output / "release.json").write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, "unsafe archive path"):
                    self.extract()
        self.assertFalse((self.root / "escaped").exists())

    def test_symlink_and_duplicate_archive_members_are_rejected(self):
        for kind in ("symlink", "duplicate"):
            with self.subTest(kind=kind):
                with tarfile.open(self.package, "w:gz") as archive:
                    member = tarfile.TarInfo("package/index.html")
                    if kind == "symlink":
                        member.type = tarfile.SYMTYPE
                        member.linkname = "/tmp/escape"
                    archive.addfile(member)
                    if kind == "duplicate":
                        archive.addfile(member)
                with self.assertRaisesRegex(ValueError, "non-regular|duplicate"):
                    self.bundle()


if __name__ == "__main__":
    unittest.main()
