# A64 alias static-gate command log

All collections ran locally on 2026-10-24. The before/after sources were
independent clones under `/tmp/dynarmic-a64-alias-qual`, at revisions
`f96fe7a0b4bc7a098ef9badf64919930e2cf8a71` and
`6a6c6d79b2c21d6590ebda65848d8883f94f8a31`. Both retained Dynarmic revision
`a41c380246d3d9f9874f0f792d234dc0cc17c180` and were clean. No merge, push,
release or publication was performed.

The two diagnostic builds used Unix Makefiles, Release, profile OFF and region
diagnostics ON, with the same Emscripten 6.0.13 toolchain and Boost headers.
Their configured source roots were the corresponding isolated clones.

```sh
cmake -S /tmp/dynarmic-a64-alias-qual/before-src \
  -B /tmp/dynarmic-a64-alias-qual/before-diagnostic \
  -DCMAKE_TOOLCHAIN_FILE=/home/xiong/sandbox/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DDYNARMIC_JS_PROFILE=OFF \
  -DDYNARMIC_JS_REGION_DIAGNOSTICS=ON \
  -DBoost_INCLUDE_DIR=/tmp/boost-headers
cmake --build /tmp/dynarmic-a64-alias-qual/before-diagnostic -j2 --target arm_jit

cmake -S /tmp/dynarmic-a64-alias-qual/after-src \
  -B /tmp/dynarmic-a64-alias-qual/after-diagnostic \
  -DCMAKE_TOOLCHAIN_FILE=/home/xiong/sandbox/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DDYNARMIC_JS_PROFILE=OFF \
  -DDYNARMIC_JS_REGION_DIAGNOSTICS=ON \
  -DBoost_INCLUDE_DIR=/tmp/boost-headers
cmake --build /tmp/dynarmic-a64-alias-qual/after-diagnostic -j2 --target arm_jit
```

The frozen workload was CoreMark revision
`1f483d5b8316753a742cbf5590caf5bd0a4e4777`, 200,000 iterations, data size 2,000,
one context, A64 flat-window mode 2. Its manifest SHA-256 is
`ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
The A64 guest SHA-256 is
`8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1`;
the native oracle SHA-256 is
`53180426be095cc3f5ca764f24f0209e1d449c79e4129ad41f89ee3b58766a3a`.

```sh
env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node tests/bench/coremark/region_diagnostics.mjs \
  /tmp/dynarmic-a64-alias-qual/before-diagnostic/examples/browser \
  /tmp/dynarmic-coremark-long a64 2 \
  /tmp/dynarmic-a64-alias-qual/before-a64-flat.json

env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node tests/bench/coremark/region_diagnostics.mjs \
  /tmp/dynarmic-a64-alias-qual/after-diagnostic/examples/browser \
  /tmp/dynarmic-coremark-long a64 2 \
  /tmp/dynarmic-a64-alias-qual/after-a64-flat.json
```

Observed collector output:

```text
before: CoreMark region diagnostics PASS regions=111 duplicateSelections=3315
after:  CoreMark region diagnostics PASS regions=99 duplicateSelections=2901
```

The diagnostic JavaScript artifact was identical across implementations:
`247964ce64d6c4de94eb09b865faffb7f0737343e30e6787b4cb6009835e94e6`.
Diagnostic Wasm SHA-256 values were
`45125719cde0ae5ef9e8964321aaf88b88ffa108293859638316ecd72d36a76e` before and
`157d310d3b1d442105359d4f99db43cce9854518c2a202e66a02da0c76a0c05e` after.
Separate production builds were compiled for implementation identity, but no
production CoreMark timing report was collected.

The preregistered static gate required exactly 95 candidate regions (111 minus
16 covered later entries). It failed with 99. No short paired screen or
complete-duration paired collector was started. Diagnostic phase and Run times
inside the raw reports are not benchmark samples and are not compared.

Preserve and audit the evidence with:

```sh
gzip -9 -n -c /tmp/dynarmic-a64-alias-qual/before-a64-flat.json \
  >docs/measurements/2026-10-24-region-alias-static-gate/before-a64-flat.json.gz
gzip -9 -n -c /tmp/dynarmic-a64-alias-qual/after-a64-flat.json \
  >docs/measurements/2026-10-24-region-alias-static-gate/after-a64-flat.json.gz
node docs/measurements/2026-10-24-region-alias-static-gate/analyze.mjs \
  /tmp/dynarmic-a64-alias-qual/before-a64-flat.json \
  /tmp/dynarmic-a64-alias-qual/after-a64-flat.json \
  docs/measurements/2026-10-24-region-alias-static-gate/analysis.json
node --test docs/measurements/2026-10-24-region-alias-static-gate/*.test.mjs
node docs/measurements/2026-10-24-region-alias-static-gate/verify.mjs
```
