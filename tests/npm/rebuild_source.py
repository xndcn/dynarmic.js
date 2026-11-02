#!/usr/bin/env python3
"""Rebuild and execute the source shipped inside an npm tarball, without Git.

The resulting private test tarball is not a publishable release artifact.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


def extract(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    with tarfile.open(archive, 'r:gz') as source:
        source.extractall(destination, filter='data')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tarball', type=Path)
    parser.add_argument('--boost-include', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    tarball, output = args.tarball.resolve(), args.output.resolve()
    tarball_sha256 = hashlib.sha256(tarball.read_bytes()).hexdigest()
    if output.exists():
        parser.error('--output must be a new directory; preserved outputs are not overwritten')
    output.mkdir(parents=True)
    args.report.unlink(missing_ok=True)
    commands = []

    def run(command: list[str], cwd: Path = output, capture: bool = False) -> str:
        commands.append({'argv': command, 'cwd': str(cwd)})
        print('+ ' + ' '.join(command), flush=True)
        return subprocess.check_output(command, cwd=cwd, text=True) if capture else subprocess.run(command, cwd=cwd, check=True).stdout

    extract(tarball, output / 'unpacked')
    package = output / 'unpacked/package'
    run(['node', 'verify.mjs'], cwd=package)
    extract(package / 'source.tar.gz', output / 'source')
    source = output / 'source/dynarmic.js'
    if (source / '.git').exists():
        raise RuntimeError('The source distribution must not depend on Git metadata')
    run(['emcmake', 'cmake', '-S', str(source), '-B', str(output / 'build'), '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=Release', '-DBoost_INCLUDE_DIR=' + str(args.boost_include.resolve()),
         '-DDYNARMIC_JS_TESTS=OFF', '-DDYNARMIC_JS_EXAMPLES=OFF', '-DDYNARMIC_JS_NPM_PACKAGE=ON'])
    run(['cmake', '--build', str(output / 'build'), '--target', 'dynarmic_package_runtime', '-j2'])
    # Consumer packaging is intentionally private and carries no release claim.
    # Its JS and runtime both come from the extracted distribution and rebuild.
    consumer_package = output / 'consumer-package'
    consumer_package.mkdir()
    metadata = json.loads((source / 'packages/dynarmic/package.json').read_text())
    metadata.pop('scripts', None)
    metadata.pop('publishConfig', None)
    metadata['private'] = True
    metadata['files'] = ['index.js', 'index.d.ts', 'runtime.mjs', 'runtime.wasm']
    (consumer_package / 'package.json').write_text(json.dumps(metadata, indent=2) + '\n')
    for name in ('index.js', 'index.d.ts'):
        shutil.copy2(source / 'packages/dynarmic' / name, consumer_package / name)
    for name in ('runtime.mjs', 'runtime.wasm'):
        shutil.copy2(output / 'build/packages/dynarmic' / name, consumer_package / name)
    pack = json.loads(run(['npm', 'pack', str(consumer_package), '--pack-destination', str(output), '--json', '--ignore-scripts'], capture=True))
    rebuilt_tarball = output / pack[0]['filename']
    consumer_report = output / 'consumer.json'
    run(['node', str(source / 'tests/npm/smoke.mjs'), str(rebuilt_tarball), '--report', str(consumer_report)])
    if hashlib.sha256(tarball.read_bytes()).hexdigest() != tarball_sha256:
        raise RuntimeError('Original tarball changed during the source rebuild; repeat with an immutable artifact')
    report = {
        'status': 'pass', 'originalTarball': str(tarball),
        'sha256': tarball_sha256,
        'sourceArchiveSha256': hashlib.sha256((package / 'source.tar.gz').read_bytes()).hexdigest(),
        'sourceHasGitMetadata': False,
        'rebuiltRuntime': json.loads((output / 'build/packages/dynarmic/runtime-build-manifest.json').read_text())['artifacts'],
        'consumer': json.loads(consumer_report.read_text()), 'commands': commands,
        'note': 'Runtime rebuilt from distributed recursive source and tested through a private local tarball. Byte identity is not claimed.',
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: distributed source rebuilt and executed through an isolated package consumer')


if __name__ == '__main__':
    main()
