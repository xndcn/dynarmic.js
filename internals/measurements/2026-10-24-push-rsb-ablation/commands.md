# PushRSB diagnostic probe command log

All paths are local. Main source remains at 9731e9a; candidate source is an isolated clone.

```sh
git clone --branch a64 --no-hardlinks /home/xiong/workspace/dynarmic.js \
  /tmp/dynarmic-return-probe.x0YvH5/source
git -C /tmp/dynarmic-return-probe.x0YvH5/source submodule update --init \
  --reference /home/xiong/workspace/dynarmic.js/externals/dynarmic

cmake -S /tmp/dynarmic-return-probe.x0YvH5/source \
  -B /tmp/dynarmic-return-probe.x0YvH5/build-native \
  -DCMAKE_BUILD_TYPE=Release -DDYNARMIC_JS_TESTS=ON \
  -DDYNARMIC_JS_EXAMPLES=OFF -DDYNARMIC_JS_PROFILE=OFF \
  -DDYNARMIC_JS_REGION_DIAGNOSTICS=OFF
cmake --build /tmp/dynarmic-return-probe.x0YvH5/build-native -j2 \
  --target region_selector_test
/tmp/dynarmic-return-probe.x0YvH5/build-native/tests/region_selector_test

source /home/xiong/sandbox/emsdk/emsdk_env.sh
emcmake cmake -S /tmp/dynarmic-return-probe.x0YvH5/source \
  -B /tmp/dynarmic-return-probe.x0YvH5/build-em \
  -DCMAKE_BUILD_TYPE=Release -DBoost_INCLUDE_DIR=/tmp/boost-headers \
  -DDYNARMIC_JS_TESTS=ON -DDYNARMIC_JS_EXAMPLES=ON \
  -DDYNARMIC_JS_PROFILE=OFF -DDYNARMIC_JS_REGION_DIAGNOSTICS=ON
cmake --build /tmp/dynarmic-return-probe.x0YvH5/build-em -j2 \
  --target region_diagnostics_test arm_jit browser_page
node /tmp/dynarmic-return-probe.x0YvH5/build-em/tests/region_diagnostics_test.js
node /tmp/dynarmic-return-probe.x0YvH5/source/tests/bench/coremark/region_diagnostics.mjs \
  /tmp/dynarmic-return-probe.x0YvH5/build-em/examples/browser \
  /tmp/dynarmic-coremark-long a64 2 \
  /tmp/dynarmic-return-probe.x0YvH5/baseline.json
```

Baseline collector output:

```text
PASS region diagnostics publication and compiled regions
CoreMark region diagnostics PASS regions=111 duplicateSelections=3315
```

The regenerated baseline `regionDiagnostics` and `duplication` objects compare
`assert.deepEqual` with the retained `a64-flat.json.gz` objects. Totals are
111 regions, 4,177 selections, 862 unique descriptors, 587 duplicated
descriptors, 3,315 duplicate selections, 708,917 module bytes and 694,277
entry-body bytes. The 28 targets total 451 selections and 423 duplicates.

The synthetic test was run before the selector change and failed with:

```text
all 28 PushRSB appends must be suppressed before candidate accounting
```

After filtering only the 28 exact `PushRSB` successors:

```sh
cmake --build /tmp/dynarmic-return-probe.x0YvH5/build-native -j2 \
  --target region_selector_test
/tmp/dynarmic-return-probe.x0YvH5/build-native/tests/region_selector_test
cmake --build /tmp/dynarmic-return-probe.x0YvH5/build-native -j2
ctest --test-dir /tmp/dynarmic-return-probe.x0YvH5/build-native --output-on-failure

git -C /tmp/dynarmic-return-probe.x0YvH5/source diff --binary -- \
  src/dynarmic/backend/wasm/region_selector.cpp \
  tests/unit/region_selector_test.cpp \
  > /tmp/dynarmic-return-probe.x0YvH5/candidate.patch

emcmake cmake -S /tmp/dynarmic-return-probe.x0YvH5/source \
  -B /tmp/dynarmic-return-probe.x0YvH5/build-em-candidate \
  -DCMAKE_BUILD_TYPE=Release -DBoost_INCLUDE_DIR=/tmp/boost-headers \
  -DDYNARMIC_JS_TESTS=ON -DDYNARMIC_JS_EXAMPLES=ON \
  -DDYNARMIC_JS_PROFILE=OFF -DDYNARMIC_JS_REGION_DIAGNOSTICS=ON
cmake --build /tmp/dynarmic-return-probe.x0YvH5/build-em-candidate -j2 \
  --target region_diagnostics_test arm_jit browser_page
node /tmp/dynarmic-return-probe.x0YvH5/build-em-candidate/tests/region_diagnostics_test.js
node /tmp/dynarmic-return-probe.x0YvH5/source/tests/bench/coremark/region_diagnostics.mjs \
  /tmp/dynarmic-return-probe.x0YvH5/build-em-candidate/examples/browser \
  /tmp/dynarmic-coremark-long a64 2 \
  /tmp/dynarmic-return-probe.x0YvH5/candidate.json
```

Candidate native tests passed 5/5; the Emscripten diagnostic host passed.
The candidate collector completed once with normal/native CRC validation:

```text
CoreMark region diagnostics PASS regions=111 duplicateSelections=2353
```

Raw candidate totals are 111 regions, 3,208 selections, 855 unique descriptors,
533 duplicated descriptors, 2,353 duplicate selections, 690,520 module bytes
and 676,174 entry-body bytes.

The final comparator requires every target in the baseline, permits ablated
candidate targets to disappear, recomputes native/guest metrics from retained
stdout, accepts identical JS for this C++-only change, and requires a changed
Wasm artifact. Entry-history comparison ignores only region IDs. Run it with:

```sh
node --test /tmp/dynarmic-return-probe.x0YvH5/source/docs/measurements/2026-10-24-push-rsb-ablation/analyze.test.mjs
node /tmp/dynarmic-return-probe.x0YvH5/source/docs/measurements/2026-10-24-push-rsb-ablation/analyze.mjs \
  /tmp/dynarmic-return-probe.x0YvH5/baseline.json \
  /tmp/dynarmic-return-probe.x0YvH5/candidate.json \
  /tmp/dynarmic-return-probe.x0YvH5/verified-comparison.json
cmp /tmp/dynarmic-return-probe.x0YvH5/final-comparison.json \
  /tmp/dynarmic-return-probe.x0YvH5/verified-comparison.json
node --test /tmp/dynarmic-return-probe.x0YvH5/source/tests/bench/*.test.mjs
```

Final validation: comparator tests passed 31/31, existing benchmark protocol
tests passed 210/210, and two independent real comparisons were byte-identical.
The static comparison verdict is PASS: module bytes -18,397 and entry-body bytes
-18,103. The entry and region-ID sequences are identical. All six retained hot
histories are unchanged; 36 entry histories changed. The targets retain 25
selections and two duplicate selections; five targets disappear completely.
This is a static screening result, not optimization acceptance. No timing
collection, dynamic counters or performance comparison was run.

The candidate patch SHA256 is
`3a1c2849f8c39ab51307191dd46870f39521b3db41986a2c067e8468ce4483b0`.
`git apply --check candidate.patch` passed against clean main revision 9731e9a;
the patch was never applied to the main tree. Compressed raw reports were made
with `gzip -9 -n`. Their SHA256 values are
`a3b23ff7102d3630f7b29b855a61fbb790d2e260cafda16bc57f71bc74e89a75` (baseline)
and `095be8434b9e2e61cd2d27ab11c2b02ca2b6fe5be7ac5926dd81ab4eb158c342`
(candidate). The independently reproduced comparison SHA256 is
`0c4cb6dbb93b397fb605a787d1df9e05274eb32318e48310872902050c496b29`.
