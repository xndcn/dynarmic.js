# Region-alias coverage command log

All commands ran locally on 2026-10-24. The source revision was
`d21e6b6e6741b66600a915e7dcfa68eec3e929c3` with the uncommitted diagnostic-v2
changes; the Dynarmic submodule remained
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. This was one complete diagnostic
Run for compile-history evidence, not a throughput collection.

```sh
cmake --build build-em-region-diagnostics -j2 \
  --target region_diagnostics_test arm_jit
node build-em-region-diagnostics/tests/region_diagnostics_test.js
cmake --build build-em-region-diagnostics-profile -j2 \
  --target region_diagnostics_test
node build-em-region-diagnostics-profile/tests/region_diagnostics_test.js

node tests/bench/coremark/region_diagnostics.mjs \
  build-em-region-diagnostics/examples/browser \
  /tmp/dynarmic-coremark-long a64 2 \
  docs/measurements/2026-10-24-region-alias-coverage/a64-flat-v2.json
gzip -9 -n docs/measurements/2026-10-24-region-alias-coverage/a64-flat-v2.json
```

The collector reported:

```text
CoreMark region diagnostics PASS regions=111 duplicateSelections=3315
```

The native and guest CRCs match and the guest halted normally. The full
200,000-iteration A64 flat-window workload is identical to the retained v1
record. The diagnostic `runMs` is deliberately excluded from the analysis.

The retained analysis was regenerated from decompressed inputs with:

```sh
node docs/measurements/2026-10-24-region-alias-coverage/analyze.mjs \
  /tmp/dynarmic-v2-baseline.json \
  /tmp/dynarmic-v2-current.json \
  docs/measurements/2026-10-24-region-alias-coverage/analysis.json
node --test docs/measurements/2026-10-24-region-alias-coverage/analyze.test.mjs \
  docs/measurements/2026-10-24-region-alias-coverage/verify.test.mjs
node docs/measurements/2026-10-24-region-alias-coverage/verify.mjs
```

The analyzer requires the complete ordered v2-to-v1 projection to deep-equal
the retained v1 history, recomputes native/guest CoreMark metrics and
duplication, checks compile totals, and retains every classifier row. The
registered cohort is exactly 82 later entries; 16 have at least one live,
statically eligible A64 fallback return-site owner. The decision is GO for
bounded prototype planning/TDD only. It is not a performance claim.

## Generated-Wasm equivalence screen

Fresh production and diagnostic processes dumped their generated modules before
the same complete guest Run. Both halted normally with native/guest CRC
`0x4983`, 111 compiled regions and 708,917 module bytes:

```sh
DYN_DUMP_DIR=/tmp/dynarmic-v2-wasm/production \
  node tests/bench/coremark/sample.mjs \
  build-em/examples/browser/dist/arm_jit.js \
  /tmp/dynarmic-coremark-long/coremark_a64.bin a64 2 \
  >/tmp/dynarmic-v2-wasm/production.log
DYN_DUMP_DIR=/tmp/dynarmic-v2-wasm/diagnostics \
  node tests/bench/coremark/sample.mjs \
  build-em-region-diagnostics/examples/browser/dist/arm_jit.js \
  /tmp/dynarmic-coremark-long/coremark_a64.bin a64 2 \
  >/tmp/dynarmic-v2-wasm/diagnostics.log
```

The official WABT 1.0.39 prebuilt Linux binary required a newer glibc than this
host provides. WABT 1.0.39 was therefore built from the official source archive
after verifying its published SHA-256. The source and build remained under
`/tmp/dynarmic-v2-wabt-source`; no downloaded build script ran from the
repository. The structural comparison was:

```sh
node docs/measurements/2026-10-24-region-diagnostics/compare_generated_wasm.mjs \
  /tmp/dynarmic-v2-wasm/production \
  /tmp/dynarmic-v2-wasm/diagnostics \
  /tmp/dynarmic-v2-wabt-source/build/wasm2wat \
  docs/measurements/2026-10-24-region-alias-coverage/generated-wasm-comparison.json
```

Expected and observed result:

```text
generated-Wasm comparison PASS modules=111 rawIdentical=0 runtimeAddressDifferences=3386
```

The retained archive contains `production/` and `diagnostics/`, each with
`block_1.wasm` through `block_111.wasm`. It was created with deterministic
metadata:

```sh
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
  -czf docs/measurements/2026-10-24-region-alias-coverage/generated-wasm.tar.gz \
  -C /tmp/dynarmic-v2-wasm production diagnostics
```

The verifier checks archive types/paths and every module's size and raw SHA-256
against both the comparison report and the v2 region history. It independently
pins the four frozen evidence payloads; `commands.md` may be updated with a
corresponding inventory refresh without altering those payloads.
