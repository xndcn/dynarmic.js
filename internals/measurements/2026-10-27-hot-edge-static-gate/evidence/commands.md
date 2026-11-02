# Reproduction and provenance commands

All commands below are run from the repository root unless an absolute output
path is shown. No command executes the CoreMark workload or a generated module.

## Input and tool identity

The retained transfer report identifies source
`ec4701f20d2e53a133367e4048f3f0fc2b981c82` and upstream dynarmic
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. The `block_dump` binary was built in
this workspace at `37a9a7aaba03bd7bcd8dfc40ac6f429a6b4f4d78`; the following
check passed, establishing that its runtime/tool sources were unchanged from
the report source:

```sh
git diff --exit-code ec4701f20d2e53a133367e4048f3f0fc2b981c82 -- \
  externals/dynarmic src tests/tools/block_dump.cpp
sha256sum build-native/tests/tools/block_dump
```

Observed tool SHA-256:

```text
d59bc78bbf7058ef103428ac3f8b1e5d12dd95944d4e4218e89afa18b010c5e7
```

The workload manifest and binary were checked against the identities in the
README. The probe independently hashes the binary and tool, validates the
report identity, and requires load base `10000` hexadecimal. Local source and
binary hashes are reproducibility evidence, not independent build-time
provenance attestation.

## Static selection probe

The reviewed analysis was produced with:

```sh
node docs/measurements/2026-10-27-hot-edge-static-gate/probe.mjs \
  build-native/tests/tools/block_dump \
  /tmp/dynarmic-coremark-long/coremark_a64.bin 0x10000 \
  docs/measurements/2026-10-27-region-transfer-diagnostics/evidence/a64-flat-window.json.gz \
  /tmp/dynarmic-hot-edge-static/node-analysis-reviewed.json
sha256sum /tmp/dynarmic-hot-edge-static/node-analysis-reviewed.json
```

Observed output:

```text
static probe PASS full=88/701 baseline=64/511 required=17/127 final=64/524
abdb7d273fe0cba7cd70943ee79a3c73c503588191e1f541aec073e30c4eae04  /tmp/dynarmic-hot-edge-static/node-analysis-reviewed.json
```

That exact output was copied to `evidence/analysis.json` without editing. To
reproduce it, choose a new output path because the probe refuses to overwrite.
With the same tool binary, `cmp` must succeed:

```sh
cmp /tmp/dynarmic-hot-edge-static/node-analysis-reviewed.json \
  docs/measurements/2026-10-27-hot-edge-static-gate/evidence/analysis.json
```

A rebuilt tool can legitimately have a different `blockDumpSha256`; in that
case compare the semantic analysis fields and record the new tool identity
rather than altering the retained evidence.

Focused and integrated tests run during review:

```sh
node --test docs/measurements/2026-10-27-hot-edge-static-gate/probe.test.mjs \
  docs/measurements/2026-10-27-hot-edge-static-gate/verify.test.mjs
node docs/measurements/2026-10-27-hot-edge-static-gate/verify.mjs
ctest --test-dir build-native -R '^benchmark_protocol$' --output-on-failure
ctest --test-dir build-native --output-on-failure
tests/regress.sh --strict build-em build-native
```

Final results were 29/29 focused tests, a successful real evidence verification,
1/1 protocol CTest, and 6/6 native CTest. The strict backend regression finished
with `regression done fail=0`; CoreMark was not requested, so its application
gate was not covered. These are correctness/reproduction results, not
performance results.

## Offline emission probe

`evidence/emit_probe_original.cpp` is the exact scratch source that produced
`emission-original.json`, `baseline.wasm`, and `candidate.wasm`. Its SHA-256 is
`23628ab9988006a17e9d4f2c6d2fdaa2d750bcb441d2d2720a7e436a1e716ff3`.
The original native emitter binary SHA-256 was
`b50cd119e5cf8f2614094f1bd57d954473f4832077b26d430b40c3a7b406b1cc`.

The portable retained `emit_probe.cpp` differs by taking input/output paths as
arguments, naming the emission-only timing field accurately, deriving the
printed block count, refusing to overwrite either module, and checking output
open/write/close failures. It was compiled with:

```sh
/usr/bin/c++ -O3 -DNDEBUG -std=c++20 -DBOOST_ALL_NO_LIB \
  -I/home/xiong/workspace/dynarmic.js/externals/dynarmic/src \
  -I/home/xiong/workspace/dynarmic.js/src \
  -I/home/xiong/workspace/dynarmic.js/externals/dynarmic/externals/mcl/include \
  -I/home/xiong/workspace/dynarmic.js/externals/dynarmic/externals/fmt/include \
  docs/measurements/2026-10-27-hot-edge-static-gate/emit_probe.cpp \
  -o /tmp/dynarmic-hot-edge-static/emit_probe_repro \
  build-native/externals/dynarmic/src/dynarmic/libdynarmic.a \
  build-native/externals/dynarmic/externals/mcl/src/libmcl.a \
  build-native/externals/dynarmic/externals/fmt/libfmt.a -lrt
```

The portable source has separate native integration tests requiring the compiled
tool and pinned workload as explicit inputs (they are not environment-independent
CTest cases):

```sh
EMIT_PROBE=/tmp/dynarmic-hot-edge-static/emit_probe_repro \
EMIT_PROBE_INPUT=/tmp/dynarmic-coremark-long/coremark_a64.bin@10000 \
node --test docs/measurements/2026-10-27-hot-edge-static-gate/emit_probe.test.mjs
```

All 3/3 tests passed: stale output remained untouched, an unwritable output
directory returned nonzero, and fresh output matched both retained Wasm hashes.

The portable source was run once to validate mechanism identity:

```sh
mkdir -p /tmp/dynarmic-hot-edge-static/repro-emission
/tmp/dynarmic-hot-edge-static/emit_probe_repro \
  /tmp/dynarmic-coremark-long/coremark_a64.bin@10000 \
  /tmp/dynarmic-hot-edge-static/repro-emission \
  > /tmp/dynarmic-hot-edge-static/repro-emission/result.json
sha256sum /tmp/dynarmic-hot-edge-static/repro-emission/{baseline,candidate}.wasm
```

Both generated-module hashes matched the retained originals exactly:

```text
3e5a89aa815d658b85988cebbea49475f7ec4bde907c916c98098f877b29a306  baseline.wasm
9effb79067b9a658cc6c94a872f8cb093fa134e94d7b4c8baab4e492b7405102  candidate.wasm
```

The reproduction's new timing samples were not used to replace or supplement
the original observations. Native timers start after translation and surround
only `EmitWASM`; both native and V8 tables use the 11th sorted value of 20
(the upper median). The original `emission-original.json` configuration string
and `v8-compile-original.txt` field name retain their historical wording;
README terminology is authoritative for the actual timer scope.

The original V8 output came from a fixed-20-trial inline Node invocation. That
inline source was not separately retained. `compile_probe.mjs` is the retained
equivalent reproduction helper with an explicit upper-median field name:

```sh
node docs/measurements/2026-10-27-hot-edge-static-gate/compile_probe.mjs \
  docs/measurements/2026-10-27-hot-edge-static-gate/evidence/baseline.wasm \
  docs/measurements/2026-10-27-hot-edge-static-gate/evidence/candidate.wasm
```

This helper validates and compiles modules but never instantiates or executes
them. Compile samples are static sanity observations, not benchmark evidence.
