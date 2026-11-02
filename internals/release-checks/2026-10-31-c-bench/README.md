# C benchmark Pages acceptance — 2026-10-31

The page adds five fixed C workloads through ARM/dynarmic.js, x86/v86 and
direct Wasm, compiled with the same Clang frontend and scalar O2 options.
There is no libc or allocation in the kernels. The independent Python oracle
checks every call, including the first call and two unmeasured warmups.

Final Chromium acceptance executed **45 observations per profile** (five
cases × three paths × three fresh Workers), for **90 observations and 540
checksums** across Quick and Extended. Each observation retains one first
call, two warmups and three measured calls. The primary values summarize
round medians, with paired ratios; the page does not promise steady state.
Quick completed in roughly ten seconds and Extended in roughly twenty to
thirty seconds on this development workstation; these are local UX checks,
not cross-device speed claims.

Firefox passed the complete Quick auto-run using the same page protocol.
Chromium also exercised export, 320–1440px layouts, dark mode, cancellation,
visibility invalidation, missing-binary handling and retry. Production scripts
instantiate the exact checked JS/Wasm bytes; a separate fault probe checks
missing host hashes and altered binary bytes before guest execution.
Profiling or region diagnostics disable performance ratios while retaining
correctness results. Samples below the timing floor also withhold ratios.

All 72 Node protocol/UI/driver tests and 22 source-package tests passed.
The existing page passed its 17 Chromium interaction checks and 8 QuickJS
interaction checks. All 11 existing host/QuickJS/official workload byte hashes
match the preceding baseline; host build records were refreshed after CMake
integration. No backend translation code changed.

`chromium-reviewed.json` retains final raw measurements and all artifact
identities. The screenshot files show the completed Extended panel in desktop,
mobile, small and dark layouts. `c-bench-manifest.json` identifies the shared
compiler and all target flags/source hashes; `c-bench-reference.json` contains
the exact inputs and independently generated outputs.

A previous development browser check failed before its harness retained a
per-case report; its cause could not be established. The harness now persists
the report before asserting success. The recorded final Quick/Extended runs
and the subsequent independent Quick auto-run completed successfully. This
is functional acceptance evidence, not a browser stability claim.

The source package is a local dirty snapshot until the changes are committed
and rebuilt. The page is available locally; no deployment was performed.

The final rebuild adds the complete v86 runtime dependency notices (SoftFloat,
Zstd/FSE/Huff0/xxHash and QEMU MIT code). Only the notice file and its C build
manifest, plus the host manifest's CMake source record, changed after browser
acceptance. All measured binaries, workload inputs and UI/Worker bytes are
unchanged; `final-build-verification.json` records this check. The final source
package verifier validates ten C inputs and eight generated assets.
