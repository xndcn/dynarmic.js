# Four-way CoreMark comparison (2026-10-28)

## Four-way result

A five-round, 20-process comparison of x86-64 native, qemu-user AArch64, native dynarmic and dynarmic.js Wasm completed successfully at harness revision `045107cd21d0e2bbd9fc8cf43baef74e5bea0a31`. Every sample produced the expected individual CRCs and accumulated `crcfinal=0x4983`; every process exited with status zero, and both dynarmic implementations reported normal `UserDefined1` halt. The final analyzer independently reparsed every raw stream and verified native and Wasm build provenance, Node/V8 and runner identity, artifact hashes and the fixed child environment.

| Implementation | Guest-kernel median | MAD | Full range | Throughput from median ticks | Whole-process wall median |
|---|---:|---:|---:|---:|---:|
| x86-64 native, static Linux-musl | 6.295871 s | 0.290333 s | 5.700396–7.211208 s | 31,766.85 CoreMark/s | 6.297861 s |
| AArch64 under qemu-user 6.2.0 | 24.703262 s | 0.567588 s | 23.264529–25.361545 s | 8,096.10 CoreMark/s | 24.711035 s |
| AArch64 under native dynarmic x86-64 JIT, mode 2 | 23.479227 s | 0.678190 s | 21.763835–24.373497 s | 8,518.17 CoreMark/s | 23.572811 s |
| AArch64 under dynarmic.js Wasm JIT, mode 2 | 28.971689 s | 1.228938 s | 26.947749–30.293424 s | 6,903.29 CoreMark/s | 29.172414 s |

Ratios use only median guest-kernel times from this complete four-way cohort, never the older three-way medians, process wall times or diagnostic counters:

- Native dynarmic is **1.0521× qemu-user throughput** (5.21% higher) and **1.2339× dynarmic.js Wasm throughput** (23.39% higher).
- qemu-user is **1.1728× dynarmic.js Wasm throughput**.
- Native dynarmic takes **3.7293× x86-native time**; dynarmic.js Wasm takes **4.6017× x86-native time**.

The native-dynarmic/Wasm comparison uses the same freestanding A64 guest and memory mode 2. Wasm `runMs` surrounds only `_arm_run`, includes cold compilation/publication, and excludes module loading, RAM setup and JIT construction; it is not substituted for guest ticks or whole-process wall time. These are fixed-workload end-to-end throughput comparisons, **not official CoreMark score submissions and not pure emulator-engine overhead ratios**. All x86 samples are below CoreMark's ten-second duration requirement, so the complete cohort is marked `durationQualified: false`.

The original three-way cohort below remains a separate historical result. Its samples and ratios are not combined with this four-way cohort.

## Original three-way result (separate cohort)

A five-round, 15-process comparison completed successfully. Every sample produced the expected individual CRCs and accumulated `crcfinal=0x4983`; every process exited with status zero, and every native dynarmic run reported normal `UserDefined1` halt. The final analyzer independently reparsed all stdout and dynarmic stderr and verified the frozen native-host build provenance.

| Implementation | Guest-kernel median | MAD | Full range | Throughput from median ticks | Whole-process wall median |
|---|---:|---:|---:|---:|---:|
| x86-64 native, static Linux-musl | 5.167138 s | 0.108821 s | 4.941700–5.275959 s | 38,706.15 CoreMark/s | 5.168913 s |
| AArch64 under qemu-user 6.2.0 | 20.287766 s | 0.215622 s | 19.829919–21.094101 s | 9,858.16 CoreMark/s | 20.298417 s |
| AArch64 under native dynarmic x86-64 JIT, mode 2 | 18.368734 s | 0.174393 s | 17.958314–18.543127 s | 10,888.07 CoreMark/s | 18.435858 s |

Ratios use median guest-kernel times, not process wall times or diagnostic counters:

- **Native dynarmic is 1.1045× qemu-user throughput**: 10.45% higher throughput and 9.46% lower elapsed kernel time on this fixed workload.
- Native dynarmic takes **3.5549× the x86-native time**, equivalent to **28.13% of x86-native throughput**.
- x86-native is **3.9263× qemu-user throughput**.

These are fixed-workload end-to-end throughput comparisons, **not official CoreMark score submissions and not pure emulator-engine overhead ratios**. All five x86 samples are below CoreMark's ten-second duration requirement and print the expected short-run error. qemu-user and native dynarmic samples meet that duration requirement, but the complete cohort is therefore marked `durationQualified: false`.

## Workload and execution identity

All paths use CoreMark revision `1f483d5b8316753a742cbf5590caf5bd0a4e4777`, 2K performance seeds, one context, stack allocation and 200,000 compile-time iterations. Clang 22.1.8 and `-O2` are used for all three CoreMark builds.

- x86 native: `x86_64-linux-musl`, static HOST build, SHA-256 `213b8e26ef79812bada9285c270eaf96045b703ea9530ea30dead5604485d709`.
- qemu-user: `aarch64-linux-musl`, static HOST build, SHA-256 `7cf5ba7e8293c9839d96a39b87001c02ea28273a01f6ba4c06b912ccd6b5761c`; emulator `/usr/bin/qemu-aarch64-static`, version 6.2.0, SHA-256 `5f0dc411dcf208a2946db2f07a3ecfee9e4bad1c20ba21197e4d1637de845b21`.
- native dynarmic: pinned `aarch64-freestanding` flat guest, SHA-256 `8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1`; native x86-64 host, SHA-256 `a249e0a7cf161b3f5c59b6b10755f3904e53ce95139c126ea44c0213de96a039`.
- dynarmic.js Wasm: the same pinned freestanding guest and memory mode 2; production Emscripten 6.0.13/Clang 24.0.0 Release artifacts with JavaScript SHA-256 `16747bdfd6585d6fb21ff5c9416a5702853dc3b2b2274ecf971070b60bfd0073` and Wasm SHA-256 `ce0ad762b23e3fdeebd53711920609312d5ba47ca4cf28902044750659375b72`. Each sample used a fresh Node v24.19.0 process with V8 13.6.233.17-node.51.
- dynarmic source revision: `a41c380246d3d9f9874f0f792d234dc0cc17c180`. The standalone native host is built in Release mode by GCC 12.3.0 with configuration flags `-O3 -DNDEBUG`; its post-link manifest binds compiler, flags, sources, revisions and executable hash.

The two Linux executables use the same HOST `CLOCK_MONOTONIC` microsecond port. The dynarmic guest uses the same algorithm sources and `svc #4` monotonic microsecond timer. qemu-user executes a hosted Linux-musl ELF while dynarmic executes the freestanding guest with the existing `svc #0/#1/#4` ABI, 16 MiB RAM, load address `0x10000`, and flat-window fastmem mode 2. Consequently, their AArch64 instruction streams and timed helper implementations are not claimed to be identical; matching CRCs establish workload correctness, not instruction-for-instruction equivalence.

Native dynarmic modes 0 and 1 also passed independent correctness runs with `0x4983` and normal halt. Their raw streams are under `validation/`; neither was substituted into the declared mode-2 performance cohort.

## Timing and host methodology

CoreMark's microsecond timer surrounds `iterate`, includes cold JIT/TCG translation during the workload, and excludes most process startup and report output. The collector separately records whole-process wall time. For native dynarmic it also records `DYNARMIC_RUN_MS`, which surrounds `jit.Run()`, includes cold compilation, and must contain the guest-kernel interval. Wasm separately records `runMs` around `_arm_run` with the equivalent containment requirement. These timing scopes are not mixed in the ratios.

The host is a 12th Gen Intel Core i7-12700 running Linux 6.8.0-138. Every sample is a fresh process with CPU affinity `0,2,4,6`. Implementations rotate within each of five rounds, so no implementation's complete cohort is run first. Host snapshots before and after the cohort are preserved. This is one declared, non-idle host environment, not a fully isolated CPU or a statistical guarantee about other machines.

## Evidence inventory

- `evidence-four-way/run-config.json`: preregistered 20-process order, all four command lines, artifact/workload hashes, source revisions, frozen native/Wasm builds, Node/V8 and runner provenance, and fixed child environment.
- `evidence-four-way/observations.jsonl` and `00-*` through `19-*`: per-process records plus unmodified stdout/stderr, including exact stream hashes and byte counts.
- `evidence-four-way/native-build-manifest.json` and `wasm-build-manifest.json`: post-link build identities; the Wasm manifest binds actual JS/Wasm artifacts, production flags/configuration, clean source and empty configure/build `EMCC_CFLAGS`.
- `evidence-four-way/host-before.txt`, `host-after.txt`, `completion.json`: host observations and successful post-cohort identity verification.
- `evidence-four-way/analysis.json`: independently reproduced v2 analysis, SHA-256 `d3c7bed8ae3bb431b1111a33db765c783ed17d9f1caeb6371e0ad224b8a4c5fc`.
- `evidence/run-config.json`: preregistered commands, artifact hashes, five-round order, source revisions, compiler/runtime identity and frozen native build description.
- `evidence/observations.jsonl`: 15 per-process records with exit status, wall time, parsed metrics and dynarmic diagnostics.
- `evidence/00-*` through `evidence/14-*`: unmodified stdout/stderr for every process.
- `evidence/native-build-manifest.json`: post-link native-host provenance, including Release optimization and executable SHA-256.
- `evidence/linux-build-manifest.json` and `base-workload-manifest.json`: exact Linux compile commands, source hashes and pinned workload identity.
- `evidence/host-before.txt`, `host-after.txt`, `completion.json`: host observations and artifact-reverification completion record.
- `evidence/analysis.json`: validated median/MAD/range and ratio analysis, plus every sample's derived observations.
- `scripts/`: matched Linux builder, collector, independent analyzer, comparison utilities and tests.
- `native/`: standalone native x86-64 dynarmic host, optimized CMake configuration and post-link manifest generator.
- `commands.md`: exact build, test, collection, analysis and reproduction commands.

## Review history and rejected cohort

The first complete five-round cohort is preserved unchanged under `pre-review-cohort/`; it is not used for the reported result. Independent review found that the original analyzer did not revalidate dynarmic stderr, its summary spread overwrote the analysis schema, and native-host optimization/binary provenance was not bound to the evidence. Those are protocol/provenance defects, not workload CRC failures.

The fixes add analysis-level tamper tests, independent stderr/Run/halt validation, correct schema handling, and a post-link native build manifest that the collector requires and freezes. A new one-round integration run passed before the final cohort. Re-review approved the fixes with no remaining CRITICAL, HIGH or result-invalidating MEDIUM findings. The pre-review observations were neither edited nor selectively discarded.

See `commands.md` for reproduction. No performance conclusion is drawn from compile counts, emitted sizes or other diagnostics.

For the four-way extension, 240 unit/protocol tests pass with 91.03% line, 92.36% branch and 96.51% function coverage. Independent code and security reviews found no remaining CRITICAL, HIGH or MEDIUM issues before formal collection. The original v1 evidence still reanalyzes byte-for-byte to its frozen report.
