# Collection record

Source: `ec4701f20d2e53a133367e4048f3f0fc2b981c82`.
Upstream dynarmic: `a41c380246d3d9f9874f0f792d234dc0cc17c180`.
Both report `git.dirty` values are false. Outputs were written outside the
checkout until both Runs completed.

The existing Release diagnostic host was rebuilt immediately before collection:

```sh
emcmake cmake -S . -B build-em-region-diagnostics \
  -DCMAKE_BUILD_TYPE=Release \
  -DDYNARMIC_JS_REGION_DIAGNOSTICS=ON \
  -DDYNARMIC_JS_PROFILE=OFF
cmake --build build-em-region-diagnostics -j2
```

The workload is the pinned 200,000-iteration CoreMark build at
`/tmp/dynarmic-coremark-long`, identity
`ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
Use fresh output paths; the collector refuses to overwrite reports.

```sh
env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node tests/bench/coremark/region_diagnostics.mjs \
  build-em-region-diagnostics/examples/browser \
  /tmp/dynarmic-coremark-long a64 0 \
  /tmp/dynarmic-region-transfer-ec4701f/a64-flat-window.json

env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node tests/bench/coremark/region_diagnostics.mjs \
  build-em-region-diagnostics/examples/browser \
  /tmp/dynarmic-coremark-long a64 2 \
  /tmp/dynarmic-region-transfer-ec4701f/a64-page-table.json
```

Both Runs completed with normal halt and native/guest CRC `0x4983`.
Uncompressed report SHA-256 values:

```text
e9a68f33456f6819a6b6bc11f8c400bd3abab50ad3aa17fa672f884a49f2c3ba  a64-flat-window.json
0a5d69c2488206c521961b40856456a7b8a18e5059b46cd6c73306f1eeb8e8df  a64-page-table.json
```

The reports and their `runMs` fields are diagnostic evidence, not benchmark
scores or paired performance comparisons.
