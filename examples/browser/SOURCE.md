# Source included with the browser distribution

After building `browser_page`, package its corresponding source and notices:

```sh
python3 examples/browser/package_source.py --dist build-em/examples/browser/dist
```

The command writes `source.tar.gz`, `source-manifest.json`, `LICENSE.txt`,
`THIRD_PARTY_NOTICES.md` and `runtime-licenses.txt` beside the Wasm runtime.
The runtime notices retain the matching Emscripten, musl, libc++, libc++abi,
compiler-rt, libunwind/LLVM libc (when present), dlmalloc and Boost license
texts used by this distribution. Pass `--emscripten-root` when `emcc` is not on
`PATH`; the script otherwise discovers the SDK from `emcc` or the build cache.
The archive contains the actual
working files from the root repository and every initialized recursive
submodule, including their licenses. Git metadata, ignored files and the
distribution directory are excluded. Historical measurements and release reports
under `internals/measurements/` and `internals/release-checks/` are also excluded,
using the same rule as the npm package. These are not runtime build inputs;
public tests, examples, build scripts and dependencies remain included. The
manifest records these exclusions. Non-ignored new files are included in
local snapshots, and deleted files remain absent. Relative symlinks to included
files are preserved; broken or escaping links are rejected.

QuickJS also ships its complete, unmodified upstream archive as
`quickjs-source.tar.xz`, its MIT license as `quickjs-LICENSE.txt`, and linked ARM
runtime notices as `quickjs-runtime-licenses.txt`. The project harness and its
reproducible UTC adaptation are in `source.tar.gz`. `quickjs-manifest.json`
records the source checksums, both compiler versions, flags and output hashes;
the packager verifies them against the current files and includes them in the
source manifest. Both Wasm engines must use the same Emscripten version so the
SDK notices correspond to the complete distribution.

`quickjs-extras-source.tar.xz` contains the complete upstream extras archive,
including the V8 v7 benchmark's original copyright and license headers. Because
that archive does not contain the GPL 2.0 text required by DeltaBlue,
`quickjs-extras-LICENSE.txt` ships a complete independent copy.
`official-v8-upstream.js` preserves its combined JavaScript unchanged; the
separate adapter and generated copy document the CLI and monotonic-clock hooks.
The packager checks the original against the archive, verifies the full ten-case
inventory, and validates `arm_bench_jit.js`/`.wasm` against their own host manifest.

The archive extracts into `dynarmic.js/`. Follow `docs/GettingStarted.md` inside
it to install the toolchains and rebuild. The source manifest retains the
runtime's build options, Emscripten version, source revisions and artifact
hashes, together with the archive's SHA-256 digest. Generated files still record
unknown Git provenance when rebuilt from an extracted source archive.
Before rebuilding, copy the shipped `quickjs-source.tar.xz` into
`build-em/examples/browser/quickjs-cache/` to reuse it without downloading. Its
checksum is verified again. See the commands in `source-manifest.json`.
Copy `quickjs-extras-source.tar.xz` to the same cache as
`quickjs-extras-2025-09-13.tar.xz` to rebuild the official suite without downloads.

For publication, use the strict form after committing and rebuilding the
candidate checkout:

```sh
python3 examples/browser/package_source.py \
  --dist build-pages/examples/browser/dist --require-clean
```

This rejects an uncommitted checkout, uninitialized or modified dependencies,
a runtime built from dirty source, mismatched source revisions and changed
runtime artifacts. Local dirty snapshots remain useful for inspection; their
manifest sets `correspondenceVerified` to `false`. The runtime build's recorded
source state is kept separate from the current snapshot: matching a commit ID
alone does not establish which uncommitted code produced the binary.

The command does not build the runtime or deploy the site. Run it after the
final build and do not edit sources or rebuild artifacts while packaging.
