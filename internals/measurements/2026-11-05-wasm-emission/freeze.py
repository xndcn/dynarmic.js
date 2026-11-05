"""Copy benchmark artifacts into a new immutable-by-convention directory."""
import hashlib
import json
import pathlib
import shutil
import sys

source, destination, manifest = map(pathlib.Path, sys.argv[1:])
destination.mkdir()
(destination / 'tests').mkdir()
(destination / 'CMakeFiles').mkdir()
shutil.copy2(source / 'CMakeCache.txt', destination / 'CMakeCache.txt')
for file in (source / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
    target = destination / 'CMakeFiles' / file.parent.name
    target.mkdir()
    shutil.copy2(file, target / file.name)
files = {}
for stem in ['jit_bench', 'jit_thumb_bench', 'jit_a64_bench', 'jit_a64_simd_bench',
             'jit_callbench', 'a32_region_alias_test', 'a64_region_sharing_test']:
    for suffix in ['.js', '.wasm']:
        file = source / 'tests' / (stem + suffix)
        target = destination / 'tests' / file.name
        shutil.copy2(file, target)
        files[file.name] = hashlib.sha256(target.read_bytes()).hexdigest()
manifest.write_text(json.dumps(files, indent=2) + '\n')
