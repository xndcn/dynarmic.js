# QuickJS extras JavaScript benchmark integration

Local review: **2026-10-31**. Scope confirmed by the user: the V8 version 7
JavaScript benchmarks distributed in QuickJS extras, displayed on the browser
page. This does not add the separate microbench CLI matrix or external
Kraken/SunSpider/Octane runners. No deployment or commit was performed.

## Scope and execution

All **eight suites / ten cases** are available individually and together:
Richards, DeltaBlue, Crypto/Encrypt, Crypto/Decrypt, RayTrace, EarleyBoyer/Earley,
EarleyBoyer/Boyer, RegExp, Splay and NavierStokes. Each runs in browser JavaScript,
QuickJS compiled directly to Wasm, and ARM32 QuickJS executed by dynarmic.js.
The pinned extras source, complete original JavaScript and license headers are
shipped with the page. The [source research](../../../docs/research-quickjs-benchmarks.md)
records the verified URLs, checksums, original assertions and sampling protocol.

The original benchmark bodies, setup, teardown and V8 v7 harness remain active:
a one-second warmup, one-second measurement batches and at least 32 iterations.
Only the harness clock uses a monotonic host hook. Standalone Decrypt prepares
Encrypt's ciphertext outside timing; standalone Encrypt also performs the
original decryption check outside timing. Suite scores require every member of
that suite and use the original geometric-mean formula. Fresh Workers per case
make this an explicitly documented browser adaptation rather than a canonical
single-process V8 score.

The page presents measured time per iteration, counts, complete-suite scores,
preparation/complete-call time, memory capacity and applicable ARM JIT counters.
JSON retains raw batches, both runtime build records, source hashes, browser
context and partial failures/cancellations. All three labs exclude concurrent
runs. Stop terminates the active Worker; each engine/case has a five-minute
execution limit. Errors do not silently remove cases from a full run.

The original Splay parameters are unchanged. Its 8000-node payload needs more
than a 128 MiB QuickJS allocation limit here. Both QuickJS paths use **192 MiB**;
the separate `arm_bench_jit` host supplies **256 MiB guest RAM / 384 MiB initial
Wasm memory**. The original LCG/sieve/pi and C lab retain their 16 MiB guest host.

## Evidence

| Requirement | Evidence |
| --- | --- |
| Complete workload coverage | `native.json`, `wasm.json`, `dynarmic.json`: ten successful official runs in each Node-hosted path, all original windows/counts retained. `final-build-verification.json` proves the tested runtime and adapter bytes survived the final build unchanged. |
| Real Chromium execution | `browser-chromium.json`: all **30** case/runtime combinations completed, exported JSON matches and measured timing/count/build identities were checked. |
| Real Firefox execution | `browser-firefox.log`: full `?official=all` comparison completed with **PASS** and browser process exited successfully. This run took about thirteen minutes on this machine; the per-case deadlines were not shortened or bypassed. |
| Recovery behavior | `browser-recovery.json`: a real active Worker was stopped, all incomplete entries retained their cancelled states, lab controls recovered, and a missing inventory could be reloaded with all ten choices. |
| Final presentation | `presentation.json`, `desktop.png`, `mobile.png`, `dark.png`: final UI replayed the existing Chromium measurements without executing new benchmarks. 320/390/1440 px × light/dark, folded/expanded statistics: **12 axe checks with zero violations**, no document/table overflow, readable foreground contrast, and preserved disclosure/focus/scroll state across updates. |
| Focused regression | `unit-tests.log` (**49/49**) and `package-tests.log` (**16/16**): UI/protocol faults, original harness behavior, exact inventory, official host provenance and source/asset correspondence. The final logs supersede earlier counts. |

The first complete Chromium driver finished execution, export and layout checks,
then failed its cancellation assertion because it stopped during manifest loading
and expected a populated partial report. A manifest-stage cancellation correctly
has no report yet. The test now waits for an active result cell before checking
partial-report behavior. The retained original log shows that test failure; the
independent `browser-recovery.json` verifies the corrected interaction. This
required no change to measured code and did not invalidate the 30 completed
measurements.

The final typography/mobile refinement changed only rendering: narrow screens
show all three engine values under each case without horizontal scrolling.
Source-overlay replay is explicitly identified in `presentation.json` and its
hashes. These screenshots use real saved measurements and are not additional
performance runs. `final-richards.json` records a fresh real Richards comparison on all three
engines after the final static assets were copied. `final-mobile-contract.json`
checks all thirty engine labels are visible at 320/390 px and the result table
has no horizontal scrolling. Legacy C and QuickJS Chromium interactions,
Firefox smoke, Node light/full, Node hello and host fault/bounds checks also
passed; their local reports remain under `test-results/official-v8/` and the
selected regression records are included here.

RegExp and NavierStokes v7 do not expose independent numerical result checks;
completion is not represented as an independent correctness oracle. Timing
observations and historical suite scores describe these local devices only.

## Reproduce

```sh
cmake --build build-em --target browser_page -j2
node examples/quickjs/official-run.mjs build-em/examples/browser/dist \
  --runtime all --case all --report /tmp/official-node.json
node tests/browser/verify_official.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist --report /tmp/official-chromium.json
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist --official
node --test examples/browser/*.test.mjs
python3 -m unittest discover -s tests/browser -p '*_test.py'
python3 examples/browser/package_source.py --dist build-em/examples/browser/dist
```

`source-sha256.json` identifies the implementation reviewed here. Generated
runtime files and additional local reports live under `build-em/` and
`test-results/official-v8/`. The current source package is an uncommitted local
review snapshot; a future publication still requires the clean build and source
correspondence gate.
