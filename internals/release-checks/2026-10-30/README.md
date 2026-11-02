# Open-source preparation verification — 2026-10-30

These results cover the local release-preparation worktree based on
`e33352f959ac745f18ec9c9854efeda4e8a88131`, with Dynarmic pinned to
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. The tree was intentionally dirty;
no tag, public release, or remote CI run is claimed. `source-sha256.json` binds
the public files to these checks. Rerun the release procedure on the eventual
committed candidate before publishing.

Environment: Linux x86-64, GCC 12.3.0, CMake 4.0.2, Emscripten 6.0.13,
Node 22.23.2 and 24.19.0, Chrome 133, Firefox 157.0.1, ziglang 0.17.0,
Unicorn 2.1.4 and Capstone 5.0.9.

| Gate | Result |
| --- | --- |
| `node --test tests/bench/*.test.mjs tests/cmake/*.test.mjs examples/browser/*.test.mjs` | 314/314 on each Node version |
| `ctest --test-dir build-native --output-on-failure` | 17/17 |
| Debug ASan/UBSan CTest, leak detection and halt-on-error enabled | 17/17 |
| `tests/install/smoke.sh` with Release and sanitizer archives | Both construct, reset and destroy A32/A64 JITs through the installed package |
| External parent-project Emscripten embedding | Guest `mov r0,#42; svc #7` executed successfully |
| `tests/regress.sh --strict build-em build-native` | Exit 0, `regression done fail=0`; all guest modes, reference logs, seeded fuzz/corpus, IR and runtime gates |
| Browser host fault tests | 18 normal/fault runs, 216 RAM boundary runs, API argument checks |
| Chromium and Firefox Worker pages, profile OFF and ON | All four checks passed with matching A32/A64 native output |
| CoreMark correctness, profile OFF and ON | Six architecture/memory cases per build; CRC/schema validated; not duration-qualified |
| No-`.git` source copy, including submodule sources | Complete Emscripten Release build and host fault tests passed; unknown provenance is `null` |
| Internal experiment tools | 428 tests passed; 97 retained hash entries checked |
| Migration inventory | All archived files accounted for; 137 compressed/binary artifacts byte-identical |
| Public documentation | 17 Markdown files checked for local link targets; no old internal paths in public executable code |

The compiler corpus baseline was reviewed with WABT 1.0.39. Only two modules
changed, each replacing `local.tee 10` with `local.set 10; local.get 10`.
Independent guest-level execution checks cover all five corpus modules before
the strict byte comparator. The manifest records this review.

One earlier strict run was interrupted by editing its shell script while Bash
was reading it. Its partial output was not accepted. The retained strict log
comes from a complete rerun after freezing scripts and build artifacts.

Known floating-point and threading limitations remain explicitly documented in
[`docs/Roadmap.md`](../../../docs/Roadmap.md). These checks establish the tested
source distribution and examples, not full ARM conformance, a stable JavaScript
API, performance leadership, or successful execution on untested browser
versions. Follow [`docs/Releasing.md`](../../../docs/Releasing.md) for publication.
