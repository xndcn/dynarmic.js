# Collection and analysis record

Both reports were collected from clean source revision
`b0a98606a748d6e4ba9f64bffaa30f823438846b`, with upstream dynarmic at
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. Outputs were written outside the
checkout until both Runs completed. The pinned workload is
`/tmp/dynarmic-coremark-long`, identity
`ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`, with
200,000 iterations, data size 2000 and one context. Numeric memory mode `0` is
callbacks/thunks.

The two existing Release builds have diagnostics and profiling enabled,
examples enabled and test-only exceptions disabled. They were rebuilt at the
collection revision before either Run:

```sh
cmake --build build-em-region-diagnostics-profile -j2
cmake --build build-em-a64-region-focus-coremark -j2
```

Baseline focus is explicitly empty. Candidate focus is exactly:

```text
0x000000000001202c,0x0000000000012370,0x000000000001237c,0x0000000000012418,0x0000000000012424,0x0000000000012428
```

The complete diagnostic Runs were collected sequentially, not concurrently:

```sh
env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node tests/bench/coremark/region_diagnostics.mjs \
  build-em-region-diagnostics-profile/examples/browser \
  /tmp/dynarmic-coremark-long a64 0 \
  /tmp/dynarmic-focused-region-transfer/baseline.json

env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node tests/bench/coremark/region_diagnostics.mjs \
  build-em-a64-region-focus-coremark/examples/browser \
  /tmp/dynarmic-coremark-long a64 0 \
  /tmp/dynarmic-focused-region-transfer/candidate.json
```

Both collectors passed with 111 generations and normal halt/CRC validation:

```text
CoreMark region diagnostics PASS regions=111 duplicateSelections=3315
CoreMark region diagnostics PASS regions=111 duplicateSelections=3316
```

The first analyzer invocation used the initially committed return-site oracle:

```sh
node docs/measurements/2026-10-27-focused-region-transfer/analyze.mjs \
  /tmp/dynarmic-focused-region-transfer/baseline.json \
  /tmp/dynarmic-focused-region-transfer/candidate.json \
  /tmp/dynarmic-focused-region-transfer/analysis.json
```

It failed closed and created no analysis output:

```text
Cannot analyze focused region transfer: missing focused return descriptors: 0x0000000000012418
```

The raw candidate generation actually contains every required return descriptor
with `emitted: true` and `entryReachable: true`; their `returnSite` fields are
false. The synthetic fixture had incorrectly encoded the opposite assumption.
The analyzer and realistic fixture were corrected through TDD to require
presence, emission and entry reachability, not `returnSite: true`. The same
analyzer command was then rerun against the unchanged raw reports and passed:

```text
focused region transfer analysis PASS schema=dynarmic.js/focused-region-transfer-analysis-v1; performance not assessed
```

Uncompressed raw-report SHA-256 values, before and after that analyzer correction:

```text
8d09c91aa2266ed4f36447b6d587f489b13fba7239f8756d3d0e2622d3810ce5  baseline.json
2463f0fdac8a5b32954386114a1afd050f3e1878b5b3832402866ae6f6d2c184  candidate.json
```

Artifact SHA-256 values:

```text
ee7dc93082bcd47a59d2e486dfb8cf6946349d5f82fee78566ec138b3f4f6852  baseline arm_jit.js
a3797eb1111f405007937be4648a4c604b02d74539fa07b7eaf12cff0cc789d5  baseline arm_jit.wasm
ee7dc93082bcd47a59d2e486dfb8cf6946349d5f82fee78566ec138b3f4f6852  candidate arm_jit.js
8d6689b7226237a03ce462bdd98bea9f2bb9e85945163128083825a250f7a868  candidate arm_jit.wasm
```

The raw reports were frozen with deterministic gzip headers:

```sh
gzip -9 -n -c /tmp/dynarmic-focused-region-transfer/baseline.json > evidence/baseline.json.gz
gzip -9 -n -c /tmp/dynarmic-focused-region-transfer/candidate.json > evidence/candidate.json.gz
cp /tmp/dynarmic-focused-region-transfer/analysis.json evidence/analysis.json
```

All durations and compile observations in these reports are diagnostic evidence,
not benchmark scores. No paired performance qualification was run during
collection or analysis.
