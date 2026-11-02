#!/usr/bin/env python3
"""Assemble verified GitHub release assets, or unpack a version for Pages.

No compilation, registry access, Git mutation or publication happens here.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "dynarmic.js/release-v1"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text())


def identity(data):
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def version_for(tag):
    version = read_json(ROOT / "packages/dynarmic/package.json")["version"]
    require(re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", version), "unsupported package version")
    require(tag == "v" + version, f"tag must match package.json: v{version}")
    return version


def check_source(manifest, revision, runtime_key):
    require(manifest["correspondenceVerified"] is True, "unverified source correspondence")
    for source in (manifest["source"], manifest[runtime_key]["source"]):
        require(source["dirty"] is False and source["revision"] == revision,
                "release requires clean source and runtime at the tagged revision")


def check_files(files, read):
    for name, expected in files.items():
        safe_path(name)
        require(identity(read(name)) == expected, f"checksum mismatch: {name}")


def safe_path(name):
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and ".." not in path.parts and
            "\\" not in name and path.as_posix() == name and name != ".",
            f"unsafe archive path: {name}")
    return path


def members(archive):
    files = {}
    for member in archive.getmembers():
        safe_path(member.name)
        require(member.isfile(), f"non-regular archive entry: {member.name}")
        require(member.name not in files, f"duplicate archive entry: {member.name}")
        files[member.name] = member
    return files


def bundle(package, pages, output, tag, revision, repository, run_url):
    version = version_for(tag)
    require(re.fullmatch(r"[0-9a-f]{40}", revision), "expected full Git revision")
    require(not output.exists(), "release output directory already exists")
    with tarfile.open(package, "r:gz") as archive:
        entries = members(archive)

        def read(name):
            return archive.extractfile(entries["package/" + name]).read()

        metadata = json.loads(read("package.json"))
        require(metadata["name"] == "dynarmic.js" and metadata["version"] == version,
                "npm package name/version differs from the release tag")
        npm = json.loads(read("source-manifest.json"))
        require(npm["schema"] == "dynarmic.js/npm-source-v1", "unsupported npm source manifest")
        check_source(npm, revision, "runtime")
        require(set(entries) == {"package/" + name for name in [*npm["files"], "source-manifest.json"]},
                "npm archive inventory differs from its manifest")
        check_files(npm["files"], read)
    source = read_json(pages / "source-manifest.json")
    require(source["schema"] == "dynarmic.js/pages-source-v1", "unsupported Pages source manifest")
    check_source(source, revision, "runtimeBuild")
    require(source["source"] == npm["source"], "npm and Pages were built from different recursive sources")
    read = lambda name: (pages / name).read_bytes()
    check_files({"source.tar.gz": {key: source["archive"][key] for key in ("sha256", "bytes")}}, read)
    for key in ("runtimeBuild", "officialHostBuild", "quickjsBuild", "cBenchBuild"):
        build = source[key]  # Release sites must contain every lab, not a partial demo.
        check_files(build["artifacts"], read)
        if key == "officialHostBuild":
            check_source(source, revision, key)
    check_files({"runtime-licenses.txt": {key: source["runtimeNotices"][key]
                                         for key in ("sha256", "bytes")}}, read)
    for name in ("index.html", "LICENSE.txt", "THIRD_PARTY_NOTICES.md"):
        require((pages / name).stat().st_size > 0, f"missing Pages asset: {name}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".release-", dir=output.parent) as directory:
        staged = Path(directory)
        package_name = f"dynarmic.js-{version}.tgz"
        site_name = f"dynarmic.js-{version}-pages.tar.gz"
        shutil.copyfile(package, staged / package_name)
        # Stable metadata makes repeat assembly of the same tested files identical.
        with (staged / site_name).open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|") as archive:
                for file in sorted(pages.rglob("*")):
                    require(not file.is_symlink(), f"symlink in Pages output: {file}")
                    if file.is_dir():
                        continue
                    require(file.is_file(), f"non-regular Pages asset: {file}")
                    name = file.relative_to(pages).as_posix()
                    safe_path(name)
                    member = tarfile.TarInfo(name)
                    member.mode = 0o644
                    member.size = file.stat().st_size
                    with file.open("rb") as stream:
                        archive.addfile(member, stream)
        manifest = {"schema": SCHEMA, "tag": tag, "version": version, "revision": revision,
                    "repository": repository, "workflowRun": run_url,
                    "assets": {name: identity((staged / name).read_bytes())
                               for name in (package_name, site_name)}}
        (staged / "release.json").write_text(json.dumps(manifest, indent=2) + "\n")
        checksums = {**manifest["assets"], "release.json": identity((staged / "release.json").read_bytes())}
        (staged / "SHA256SUMS").write_text("".join(f"{value['sha256']}  {name}\n" for name, value in sorted(checksums.items())))
        shutil.copytree(staged, output)
    return manifest


def extract_pages(assets, output, tag, revision, repository):
    require(not output.exists(), "Pages output directory already exists")
    manifest = read_json(assets / "release.json")
    require(manifest["schema"] == SCHEMA and manifest["tag"] == tag and
            manifest["revision"] == revision and manifest["repository"] == repository,
            "release identity does not match the requested repository/tag/commit")
    version = manifest["version"]
    require(re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", version) and tag == "v" + version,
            "invalid release version")
    name = f"dynarmic.js-{version}-pages.tar.gz"
    check_files({name: manifest["assets"][name]}, lambda name: (assets / name).read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".pages-", dir=output.parent) as directory:
        staged = Path(directory)
        with tarfile.open(assets / name, "r:gz") as archive:
            entries = members(archive)
            require("index.html" in entries and "source.tar.gz" in entries, "incomplete Pages archive")
            for name, member in entries.items():
                target = staged / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("wb") as destination:
                    shutil.copyfileobj(source, destination)
        source = read_json(staged / "source-manifest.json")
        check_source(source, revision, "runtimeBuild")
        # This is a versioned download of the previously tested site, unchanged.
        shutil.copytree(staged, output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    tag_parser = commands.add_parser("check-tag")
    tag_parser.add_argument("--tag", required=True)
    for command in ("bundle", "extract-pages"):
        sub = commands.add_parser(command)
        for option in ("tag", "revision", "repository"):
            sub.add_argument("--" + option, required=True)
        sub.add_argument("--output", type=Path, required=True)
        if command == "bundle":
            sub.add_argument("--package", type=Path, required=True)
            sub.add_argument("--pages", type=Path, required=True)
            sub.add_argument("--run-url", required=True)
        else:
            sub.add_argument("--assets", type=Path, required=True)
    args = vars(parser.parse_args())
    command = args.pop("command")
    try:
        if command == "check-tag":
            print(version_for(args["tag"]))
        else:
            result = (bundle if command == "bundle" else extract_pages)(**args)
            print(f"Verified {result['tag']} at {result['revision']}")
    except (ValueError, KeyError, OSError, tarfile.TarError) as error:
        parser.exit(1, f"release packaging failed: {error}\n")


if __name__ == "__main__":
    main()
