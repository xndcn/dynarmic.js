# Examples release-readiness checks — 2026-10-30

This record covers the examples refresh on the local worktree based on
`4153e8adc479041fbd0a075c30908be060b76bfa`, with upstream Dynarmic pinned to
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. The backend and guest source are
unchanged. `source-sha256.json` identifies the changed implementation,
documentation and CI files. This is local preparation evidence; no tag,
publication or successful remote workflow run is claimed.

Environment: Linux x86-64, Node 22.23.2 / 24.19.0, CMake 4.0.2,
Emscripten 6.0.13, Chrome 133.0.6943.53, Firefox 157.0.1,
ARM GCC 12.2.1 and Python ziglang 0.17.0. Both browser builds use Release,
no region focus, and no region diagnostics; profiling is tested OFF and ON.

## Requirements and evidence

| Requirement | Current-state verification |
| --- | --- |
| Research established open-source examples | [Maintainer guide](../../../examples/MAINTAINING.md) records the inspected v86, sql.js, ffmpeg.wasm, Ruffle, Emscripten and MDN sources, pinned GitHub revisions and the resulting design decisions. |
| Reproducible first run | Clean, strict configure and only `browser_page` in a new build directory produced all ten assets. `clean-build-commands.json` records the commands; `clean-build-verification.json` records hashes, exact native output and source/static-file equality. No pre-existing build outputs were reused. |
| Small teaching example | `node examples/node/hello.mjs` checked console output, register 0 and normal halt on ARM32/AArch64 under Node 22 and 24. The clean build and both profiling variants passed. See `node-*.log` and `clean-build-node.log`. |
| Clear, responsive browser experience | Real Chrome checks cover nested-path hosting, status semantics, keyboard entry, three memory modes, a 375px layout with expanded statistics, missing files and retry, cancellation while fetching, and termination of an infinite ARM guest loop followed by a successful rerun. See `chromium-*.json` for results and tested asset hashes. |
| Recoverable errors and bounded work | Host/Worker unit tests cover fetch/startup/postMessage errors, invalid input, empty reference, mismatches, unexpected halt reasons, missing Wasm features, concurrent module loading, poisoned runtime replacement, timeout, cancellation and stale Worker replies. Smoke-driver tests cover process and connection cleanup. |
| Complete build and diagnostics | CMake tests independently exercise six missing-prerequisite configurations and reference capture with spaces, stderr, partial failure and a missing executable. `DYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON` is enabled in the documented browser command and CI matrix. |
| Real engine and configuration coverage | `verify_interactions.mjs` passed for profiling OFF and ON in Chromium. `verify_browser.mjs` passed both in Firefox. `fault-*.log` records 18 normal/fault runs and 216 guest-RAM boundary runs per configuration. |
| Project regression remains green | `native-ctest.log`: 23/23 CTest gates. `strict-regression.log`: `tests/regress.sh --strict build-em build-native` exited 0 with `regression done fail=0`. CoreMark was not requested; no application performance claim is made. |
| Maintainable delivery | CI runs the minimal example, focused tests, real Chromium interaction checks and Firefox checks in both profiling modes; interaction reports retain asset hashes. Public Markdown local links and `git diff --check` passed. |

## Reproduce the focused checks

After the documented strict browser builds:

```sh
node --test examples/browser/*.test.mjs tests/cmake/browser_example.test.mjs
node examples/node/hello.mjs build-em/examples/browser/dist
node examples/browser/fault_test.mjs build-em/examples/browser
node tests/browser/verify_interactions.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist --report chromium-off.json
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist
```

Repeat the runtime checks with `build-em-profile` configured with
`DYNARMIC_JS_PROFILE=ON`, and the unit checks with Node 22 and 24.
Reports refuse to overwrite an existing output file. Clean build logs remain
in the local ignored `test-results/examples-clean-build-cfne88u9/` directory;
the small command/result records are retained here.

The [browser preview](../../../examples/browser/preview.png) is a real Chrome
capture. Desktop and mobile light/dark layouts were inspected; it is not a
performance report. The current runtime continues to be experimental, with
its existing [limitations](../../../docs/Roadmap.md). Follow the
[release procedure](../../../docs/Releasing.md) on the eventual committed
candidate before publishing.
