# A64 CoreMark region diagnostics — 2026-10-24

This record retains one complete A64 flat-window CoreMark compilation history,
its derived full-descriptor duplication attribution, and the corresponding
production/diagnostic generated Wasm modules. It is profiling infrastructure
evidence, not a performance result.

## Result

| Observation | Value |
| --- | ---: |
| Compiled regions | 111 |
| Selected block instances | 4,177 |
| Unique full `IR::LocationDescriptor` values | 862 |
| Descriptors selected more than once | 587 |
| Duplicate selections | 3,315 |
| Generated module bytes | 708,917 |
| Serialized exported entry-body bytes | 694,277 |

The generated-code comparison covers all 111 module pairs. Every pair has the
same byte length and identical normalized WAT after masking only classified
embedded runtime addresses. There are **zero raw-byte-identical pairs**; raw
byte equality is neither expected nor claimed.

| Masked runtime-address context | Differences |
| --- | ---: |
| Flat-window memory offsets (`223236` versus `225076`) | 2,273 |
| Link-slot table indices | 278 |
| Link-slot miss addresses stored at `ExecutionContext + 8` | 278 |
| Paired RSB fast-dispatch entry addresses | 460 |
| Fast-dispatch table base addresses | 97 |
| Fast-dispatch miss addresses at `ExecutionContext + 12` | 0 |
| **Total** | **3,386** |

Every other WAT line and token must match. Within each module, the ordered
link-slot load and miss-store addresses must also be equal in pairs; matching
counts alone are insufficient. An unclassified numeric difference, non-numeric
difference, size difference, missing module, or unpaired/mismatched address
context fails the comparison. The full verifier changes an ordinary `i64.const`
and separately changes only a link-slot miss-store address in temporary modules.
It requires rejection with `unexpected difference` and `link-slot address
mismatch`, respectively, guarding against normalization hiding arbitrary
guest-code changes or inconsistent dispatch metadata.

## Provenance

- Base revision: `72712285d9f297916b09e82e4951a82abe36a62a`.
- Dynarmic submodule: `a41c380246d3d9f9874f0f792d234dc0cc17c180`.
- The report records a dirty tree because it was collected before the
  diagnostic milestone was committed. The milestone's committed implementation
  and the retained scripts are the authoritative source associated with this
  pre-commit record.
- Release build, Emscripten 6.0.13, profile OFF, region diagnostics ON.
- Node v24.19.0; full host and toolchain details are in the compressed report.
- CoreMark revision `1f483d5b8316753a742cbf5590caf5bd0a4e4777`, 200,000
  iterations, A64 architecture, flat-window memory mode 2.
- Workload-manifest SHA256:
  `ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
- A64 executable SHA256:
  `8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1`.
- Native and guest CoreMark CRCs match. The native oracle finishes in less
  than CoreMark's ten-second validity threshold; this does not invalidate its
  use as a CRC oracle. The diagnostic guest Run is not a benchmark score.

## Files

- `a64-flat.json.gz`: complete raw report, immutable region history, derived
  duplication, workload manifest, native/guest output and provenance.
- `generated-wasm.tar.gz`: `production/` and `diagnostics/`, each containing
  `block_1.wasm` through `block_111.wasm`.
- `generated-wasm-comparison.json`: per-module sizes, raw SHA256 hashes,
  normalized-WAT hashes and classified runtime-address counts.
- `compare_generated_wasm.mjs`: fail-closed structural comparator.
- `verify.mjs`: inventory, CRC, duplication, module/hash and optional fresh WAT
  verification.
- `SHA256SUMS`: hashes of the three retained data artifacts.

## Reproduction

Use the same source, workload, Emscripten and V8 versions when exact comparison
is required. Configure separate production and diagnostic builds so their cache
keys and generated modules cannot be confused:

```sh
emcmake cmake -S . -B build-em -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR=/path/to/boost-headers \
  -DDYNARMIC_JS_TESTS=ON -DDYNARMIC_JS_EXAMPLES=ON \
  -DDYNARMIC_JS_PROFILE=OFF -DDYNARMIC_JS_REGION_DIAGNOSTICS=OFF
cmake --build build-em -j

emcmake cmake -S . -B build-em-region-diagnostics -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR=/path/to/boost-headers \
  -DDYNARMIC_JS_TESTS=ON -DDYNARMIC_JS_EXAMPLES=ON \
  -DDYNARMIC_JS_PROFILE=OFF -DDYNARMIC_JS_REGION_DIAGNOSTICS=ON
cmake --build build-em-region-diagnostics -j
```

Collect the complete diagnostic history (the output must not already exist):

```sh
node tests/bench/coremark/region_diagnostics.mjs \
  build-em-region-diagnostics/examples/browser \
  /path/to/coremark-workload a64 2 /tmp/a64-regions.json
```

Dump corresponding modules through fresh, otherwise identical guest Runs:

```sh
mkdir -p /tmp/region-wasm/production /tmp/region-wasm/diagnostics
DYN_DUMP_DIR=/tmp/region-wasm/production \
  node tests/bench/coremark/sample.mjs \
  build-em/examples/browser/dist/arm_jit.js \
  /path/to/coremark-workload/coremark_a64.bin a64 2 \
  >/tmp/production-sample.txt
DYN_DUMP_DIR=/tmp/region-wasm/diagnostics \
  node tests/bench/coremark/sample.mjs \
  build-em-region-diagnostics/examples/browser/dist/arm_jit.js \
  /path/to/coremark-workload/coremark_a64.bin a64 2 \
  >/tmp/diagnostics-sample.txt
```

`DYN_DUMP_DIR` is a Node-only debug aid. Do not use these dumped Runs for
benchmark timing. Compare with wabt 1.0.39 or a compatible later version:

```sh
node internals/measurements/2026-10-24-region-diagnostics/compare_generated_wasm.mjs \
  /tmp/region-wasm/production /tmp/region-wasm/diagnostics \
  /path/to/wabt/bin/wasm2wat /tmp/generated-comparison.json
```

Expected summary for the retained modules:

```text
generated-Wasm comparison PASS modules=111 rawIdentical=0 runtimeAddressDifferences=3386
```

The compressor was invoked as `gzip -9 -n`; `SHA256SUMS` covers the compressed
report, raw-module archive and comparison report.

## Verification

From the repository root, verify inventory hashes, native/guest output and CRC,
workload identity, raw and derived diagnostics, compile totals, archive types
and paths, and every raw module hash:

```sh
node internals/measurements/2026-10-24-region-diagnostics/verify.mjs
```

For an independent fresh `wasm2wat` comparison and both mutation negative tests,
provide the directory containing `wasm2wat` and `wat2wasm`:

```sh
node internals/measurements/2026-10-24-region-diagnostics/verify.mjs \
  --wabt-bin /path/to/wabt/bin
```

Expected output:

```text
region-diagnostics evidence PASS regions=111 descriptors=862 modules=111 files=3
generated-Wasm fresh comparison and mutation rejection PASS
```

The second line appears only with `--wabt-bin`.

## Interpretation and limits

- Duplicate selections are static compilation-history observations. They are
  not dynamic direct calls, RSB returns, other tail transfers, or live-cache
  residency counts.
- A repeated descriptor does not prove byte-identical emitted Wasm. Module and
  entry-body bytes are retained per region; body bytes are never divided evenly
  among selected guest blocks.
- The structural comparison applies to these 111 module pairs and their explicit
  runtime-address contexts. It is not a general Wasm semantic-equivalence proof.
- This is one A64 flat-window workload. It does not establish A32, Thumb, other
  memory-mode, browser, or application-wide behavior.
- Diagnostic publication and module dumping can change timing. The retained
  `runMs` is not a benchmark score and must not feed performance comparisons.
- Use this attribution to formulate one bounded hot-edge hypothesis. Do not
  resume global block-cap sweeps or add dynamic transfer counters unless a
  concrete hot-edge decision still requires them.
