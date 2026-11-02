# Native Dynarmic comparison, 2026-10-30

The final `report.json.gz` contains a complete five-pair comparison of all 12
fixed guest cases against the pinned upstream Dynarmic x86_64 backend. Every
process passed its guest checks; all 1,080 cold/warm samples and raw process
stdout/stderr are retained. Memory kernels also reset output sentinels and
verify stores after each timed Run. The six distinct guest instruction streams
were independently executed and counted by Unicorn; that audit caught and
corrected the existing A32 instruction-count formula before this final cohort.

**This is a shared-workstation example, not a release speed guarantee or a
controlled hardware baseline.** The machine was not isolated, CPU affinity
covered all 20 logical CPUs, and several cases show substantial dispersion.
The repository's own builds were paused for collection; other system activity
was not controlled. Five process pairs satisfy the protocol's observation
count requirement, not statistical significance or completed Wasm tiering.

Environment: Intel Core i7-12700, Linux `6.8.0-138-generic`, affinity `0-19`,
Node `v24.19.0` / V8 `13.6.233.17-node.51`, Emscripten `6.0.13`, native GCC
`12.3.0`. Both builds were Release, with production Wasm profiling/diagnostics
disabled. The upstream revision was
`a41c380246d3d9f9874f0f792d234dc0cc17c180` and its worktree was pristine.
The dynarmic.js checkout was dirty during release preparation; exact executable,
collector and workload source hashes, compiler flags and build settings are
inside the artifact. They identify this observation, not a build-time source
attestation.

Times below are medians of process warm medians. The ratio is the median of
the **paired** `wasm ms / native ms` values, so it is not necessarily the
quotient of the two displayed time medians. A ratio above one favors native;
below one favors Wasm for this particular kernel. MAD is the median absolute
deviation of those five paired ratios. No overall emulator score is inferred.

| Case | Native warm ms | Wasm warm ms | Native/Wasm throughput | Ratio MAD |
| --- | ---: | ---: | ---: | ---: |
| memory-thunk | 2.6550 | 6.8576 | 2.616 | 0.530 |
| memory-page-table | 1.0638 | 1.4847 | 1.234 | 0.185 |
| memory-flat-window | 1.0214 | 1.2814 | 1.130 | 0.299 |
| call-return | 3.2571 | 5.6098 | 1.640 | 0.192 |
| a64-memory-thunk | 2.4626 | 5.1806 | 2.127 | 0.232 |
| a64-memory-page-table | 1.1761 | 1.7667 | 1.218 | 0.045 |
| a64-memory-flat-window | 0.8996 | 0.7048 | 0.812 | 0.048 |
| a64-neon | 2.8590 | 0.6800 | 0.251 | 0.016 |
| a64-scalar | 1.3435 | 0.7274 | 0.527 | 0.065 |
| thumb-memory-thunk | 0.0293 | 0.1627 | 5.396 | 1.203 |
| thumb-memory-page-table | 0.0139 | 0.0481 | 3.419 | 0.291 |
| thumb-memory-flat-window | 0.0129 | 0.0262 | 2.334 | 0.351 |

Cold Run times, all eight warm indices and every individual process are in the
report. Native compilation-phase counters are unavailable and remain `null`.
The flat-memory cases share in-range guest addresses; native fastmem and the
Wasm checked flat window have different fault/checking policies. These fixed
microkernels do not represent application, browser or CoreMark performance.

Reproduce with the [native benchmark instructions](../../../tests/bench/native/README.md).
The final collector invocation was:

```sh
node tests/bench/native_run.mjs build-em/tests build-dynarmic-native \
  --runs 5 --output /tmp/dynarmic-native-comparison-release-v2.json
node tests/bench/native_validate.mjs /tmp/dynarmic-native-comparison-release-v2.json --require-comparison
```

Verify retained evidence without rebuilding or running a performance test:

```sh
node internals/measurements/2026-10-30-native-comparison/verify.mjs
gzip -dc internals/measurements/2026-10-30-native-comparison/report.json.gz > /tmp/native-comparison.json
node tests/bench/native_validate.mjs /tmp/native-comparison.json --require-comparison
```

The verifier pins both compressed and uncompressed SHA256, recomputes every
summary/ratio through the public validator and checks the independently audited
instruction count of every sample. The compressed final artifact SHA256 is
`1c497db12e9debd88159823c0adcc23ac6ae4375abafaa2855bd543ee3a91e9c`.

`invalid-instruction-count.json.gz` is the untouched initial cohort, retained
only for audit. It reported 3,080,003 A32 instructions, while the actual stream
executes 3,082,002: the outer back edge executes `mov r0,#0` another 1,999 times.
Its throughput denominators are wrong, so it is **rejected performance
evidence** and is not used in the table or any comparison conclusion. The
corrected cohort was collected afresh after fixing the source and rebuilding
both implementations; the original samples were not relabeled or repaired.
