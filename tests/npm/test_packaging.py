"""Exercise npm staging provenance with real isolated files and archives.

The fixtures do not compile a runtime: runtime execution is covered by the
installed-tarball consumers. Only Git identity and SDK notice collection are
substituted here; hashing, archive creation and the shipped verifier are real.
"""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "packages/dynarmic"))
SPEC = importlib.util.spec_from_file_location("dynarmic_npm_pack", REPOSITORY / "packages/dynarmic/pack.py")
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)


class PackageProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dynarmic-pack-provenance-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "source"
        self.build = self.base / "build"
        self.output = self.base / "package"
        self.runtime = self.build / "packages/dynarmic"
        self.runtime.mkdir(parents=True)
        self.source = {"revision": "a" * 40, "dirty": False, "submodules": [
            {"path": "externals/dynarmic", "revision": "b" * 40, "indexRevision": "b" * 40, "dirty": False}
        ]}
        for name in (
            "CMakeLists.txt", "src/backend.cpp", "cmake/Host.cmake", "patches/portability.patch",
            "packages/dynarmic/host.cpp", "packages/dynarmic/CMakeLists.txt",
            "packages/dynarmic/write_build_manifest.py", "externals/dynarmic/CMakeLists.txt",
            "externals/dynarmic/LICENSE.txt", "externals/dynarmic/externals/fmt/LICENSE.rst",
            "externals/dynarmic/externals/mcl/LICENSE", "externals/dynarmic/externals/robin-map/LICENSE",
            "LICENSE", "THIRD_PARTY_NOTICES.md",
        ):
            self.write(name, f"fixture source: {name}\n")
        for name in pack.PUBLIC_FILES:
            self.write("packages/dynarmic/" + name, (REPOSITORY / "packages/dynarmic" / name).read_text())
        (self.runtime / "runtime.mjs").write_text("export default async () => ({});\n")
        (self.runtime / "runtime.wasm").write_bytes(b"\0asm\1\0\0\0")
        self.manifest = {
            "schema": "dynarmic.js/npm-runtime-build-v1",
            "source": copy.deepcopy(self.source),
            "inputs": pack.runtime_inputs(self.root),
            "build": {"type": "Release", "emscriptenVersion": "6.0.13", "profile": False,
                      "regionDiagnostics": False, "a64RegionFocus": ""},
            "artifacts": {name: pack.digest(self.runtime / name) for name in ("runtime.mjs", "runtime.wasm")},
        }
        self.record_manifest()
        self.patch = mock.patch.multiple(pack, ROOT=self.root,
            source_files=mock.Mock(side_effect=self.source_files),
            runtime_notices=mock.Mock(return_value=("Fixture SDK license\n", {"files": []})))
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def write(self, relative, text):
        file = self.root / relative
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)

    def source_files(self, root, output):
        return sorted(file.relative_to(root) for file in root.rglob("*") if file.is_file()), copy.deepcopy(self.source)

    def record_manifest(self):
        (self.runtime / "runtime-build-manifest.json").write_text(json.dumps(self.manifest))

    def stage(self, require_clean=False):
        return pack.stage(self.build, self.output, require_clean, self.base / "sdk")

    def test_matching_clean_snapshot_stages_with_actual_archive_and_verifier(self):
        report = self.stage(require_clean=True)
        self.assertTrue(report["correspondenceVerified"])
        self.assertEqual(report["runtime"]["inputSha256"], self.manifest["inputs"]["sha256"])
        with tarfile.open(self.output / "source.tar.gz") as archive:
            self.assertEqual(archive.extractfile("dynarmic.js/src/backend.cpp").read(),
                             (self.root / "src/backend.cpp").read_bytes())

    def test_dirty_local_snapshot_stages_but_cannot_claim_release(self):
        self.source["dirty"] = True
        self.manifest["source"]["dirty"] = True
        self.record_manifest()
        self.assertFalse(self.stage()["correspondenceVerified"])
        with self.assertRaisesRegex(pack.PackageError, "runtime linked from a clean checkout"):
            self.stage(require_clean=True)

    def test_changed_source_at_same_revision_requires_rebuild(self):
        self.write("src/backend.cpp", "new implementation\n")
        with self.assertRaisesRegex(pack.PackageError, "compiler inputs changed"):
            self.stage()
        self.assertFalse(self.output.exists())

    def test_new_and_removed_compiler_inputs_require_rebuild(self):
        header = self.root / "src/new.hpp"
        header.write_text("new header\n")
        with self.assertRaisesRegex(pack.PackageError, "compiler inputs changed"):
            pack.check_runtime(self.build, False)
        header.unlink()
        (self.root / "src/backend.cpp").unlink()
        with self.assertRaisesRegex(pack.PackageError, "compiler inputs changed"):
            pack.check_runtime(self.build, False)

    def test_changed_runtime_pair_is_rejected(self):
        for name in ("runtime.mjs", "runtime.wasm"):
            with self.subTest(name=name):
                file = self.runtime / name
                original = file.read_bytes()
                file.write_bytes(original + b"changed")
                with self.assertRaisesRegex(pack.PackageError, "does not match its build manifest"):
                    self.stage()
                file.write_bytes(original)

    def test_nonproduction_configurations_are_rejected(self):
        for key, value in (("type", "Debug"), ("type", "RelWithDebInfo"), ("profile", True),
                           ("regionDiagnostics", True), ("a64RegionFocus", "0x0000000000001000")):
            with self.subTest(key=key, value=value):
                original = self.manifest["build"][key]
                self.manifest["build"][key] = value
                self.record_manifest()
                with self.assertRaisesRegex(pack.PackageError, "require a Release runtime"):
                    self.stage()
                self.manifest["build"][key] = original
        self.record_manifest()

    def test_unknown_runtime_cleanliness_cannot_pass_release_gate(self):
        self.manifest["source"]["dirty"] = None
        self.record_manifest()
        with self.assertRaisesRegex(pack.PackageError, "runtime linked from a clean checkout"):
            pack.check_runtime(self.build, True)

    def test_dirty_checkout_cannot_reuse_clean_runtime_for_publication(self):
        self.source["dirty"] = True
        with self.assertRaisesRegex(pack.PackageError, "requires a clean recursive checkout"):
            self.stage(require_clean=True)

    def test_checkout_or_submodule_revision_change_requires_rebuild(self):
        self.source["revision"] = "c" * 40
        with self.assertRaisesRegex(pack.PackageError, "runtime revisions differ"):
            self.stage()
        self.source["revision"] = self.manifest["source"]["revision"]
        self.source["submodules"][0]["revision"] = "c" * 40
        with self.assertRaisesRegex(pack.PackageError, "runtime revisions differ"):
            self.stage()

    def test_output_without_package_marker_is_preserved(self):
        self.output.mkdir()
        unrelated = self.output / "keep.txt"
        unrelated.write_text("unrelated files")
        with self.assertRaisesRegex(pack.PackageError, "refusing to replace"):
            self.stage()
        self.assertEqual(unrelated.read_text(), "unrelated files")

    def test_compiler_input_omitted_by_git_ignore_is_rejected(self):
        def omit_input(root, output):
            selected, source = self.source_files(root, output)
            return [path for path in selected if path != Path("src/backend.cpp")], source
        with mock.patch.object(pack, "source_files", side_effect=omit_input):
            with self.assertRaisesRegex(pack.PackageError, "archive omits compiler/package input"):
                self.stage()
        self.assertFalse(self.output.exists())

    def test_source_change_and_revert_during_archiving_is_rejected(self):
        original_writer = pack.write_archive
        def changed_archive(root, selected, output):
            file = root / "src/backend.cpp"
            original = file.read_bytes()
            try:
                file.write_text("transient wrong compiler source\n")
                original_writer(root, selected, output)
            finally:
                file.write_bytes(original)
        with mock.patch.object(pack, "write_archive", side_effect=changed_archive):
            with self.assertRaisesRegex(pack.PackageError, "archive input differs from runtime/package"):
                self.stage()
        self.assertFalse(self.output.exists())

    def test_wrapper_change_and_revert_during_archiving_is_rejected(self):
        original_writer = pack.write_archive
        def changed_archive(root, selected, output):
            file = root / "packages/dynarmic/index.js"
            original = file.read_bytes()
            try:
                file.write_text("export const wrongWrapper = true;\n")
                original_writer(root, selected, output)
            finally:
                file.write_bytes(original)
        with mock.patch.object(pack, "write_archive", side_effect=changed_archive):
            with self.assertRaisesRegex(pack.PackageError, "archive input differs from runtime/package"):
                self.stage()

    def test_runtime_changed_during_archiving_is_rejected(self):
        original_writer = pack.write_archive
        def changed_runtime(root, selected, output):
            original_writer(root, selected, output)
            (self.runtime / "runtime.wasm").write_bytes(b"replaced during archive generation")
        with mock.patch.object(pack, "write_archive", side_effect=changed_runtime):
            with self.assertRaisesRegex(pack.PackageError, "does not match its build manifest"):
                self.stage()

    def test_new_checkout_file_during_archiving_is_rejected(self):
        original_writer = pack.write_archive
        def changed_checkout(root, selected, output):
            original_writer(root, selected, output)
            self.write("new-file.md", "created while packaging\n")
        with mock.patch.object(pack, "write_archive", side_effect=changed_checkout):
            with self.assertRaisesRegex(pack.PackageError, "source or runtime changed while packaging"):
                self.stage()


if __name__ == "__main__":
    unittest.main()
