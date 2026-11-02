# JavaScript benchmarks distributed with QuickJS

Research: **2026-10-30–31**. The user's clarified scope is the pure JavaScript
benchmark suite suitable for the GitHub Pages comparison: **V8 Benchmark Suite
version 7, distributed in QuickJS extras 2025-09-13**. These are V8 benchmarks
shipped by QuickJS, not benchmarks invented by the QuickJS project.

## Source and exact scope

The [official QuickJS website](https://bellard.org/quickjs/) identifies extras as
containing the bench-v8 benchmark and links the
[official GitHub repository](https://github.com/bellard/quickjs). The pinned
[2025-09-13 Makefile](https://github.com/bellard/quickjs/blob/19abf1888db5884a5758036ff6e7fa2b340acedc/Makefile)
has a `bench-v8` target that builds `tests/bench-v8/combined.js` and executes it
with `./qjs -d`. The fetched extras archive contains all sources and its harness.

| Artifact | Verified identity |
| --- | --- |
| [Official extras archive](https://bellard.org/quickjs/quickjs-extras-2025-09-13.tar.xz) | 716,732 bytes; SHA-256 `7e5aa54376aad91c952b6a6f44f150856073da71973c8914f041932969e9f4e8` |
| `tests/bench-v8/combined.js` in that archive | 457,790 bytes; SHA-256 `784c263ca755de0d6ed90b1ed2bb09663313bd9f2dbbd2bbae1f4f4b80be8594` |
| `tests/bench-v8/base.js` | SHA-256 `74c49b06d43d014bf392fed549d4d9acce89e76c901117617816254df1e6d1fd`; declares `BenchmarkSuite.version = '7'` |

The archived `combined.js` exactly equals the concatenation named in its
Makefile: `base.js`, `richards.js`, `deltablue.js`, `crypto.js`, `raytrace.js`,
`earley-boyer.js`, `regexp.js`, `splay.js`, `navier-stokes.js`, `run_harness.js`.
The inventory is **eight suites and ten executable cases**, not eight cases:

| Suite | Exact case IDs | Existing upstream correctness checks |
| --- | --- | --- |
| Richards | `Richards/Richards` | Scheduler queue count 2322 and hold count 928; throws on mismatch. |
| DeltaBlue | `DeltaBlue/DeltaBlue` | Required-constraint/cycle failures and chain/projection result checks call `alert`. The Worker host must surface these as failure, not a dismissible dialog. |
| Crypto | `Crypto/Encrypt`, `Crypto/Decrypt` | Decrypt checks that the recovered message equals `TEXT`. It depends on ciphertext from Encrypt. Encryption alone has no equivalent final assertion. |
| RayTrace | `RayTrace/RayTrace` | Render checksum must equal 2321; throws otherwise. |
| EarleyBoyer | `EarleyBoyer/Earley`, `EarleyBoyer/Boyer` | The shared `RunBenchmark` checks each algorithm's supplied result predicate and throws on an incorrect rewrite result. |
| RegExp | `RegExp/RegExp` | Setup initializes and runs the workload; teardown releases it. This version does not expose an independent result checksum. |
| Splay | `Splay/Splay` | Teardown checks 8000 keys in strictly increasing order. Skipping teardown skips these checks. |
| NavierStokes | `NavierStokes/NavierStokes` | Setup configures the 128×128 solver and 20 solver iterations; teardown releases it. This version does not include a numerical output checksum. |

The main QuickJS source archive separately contains `tests/microbench.js`
(62 base entries, three BigInt arithmetic entries and `sort_bench`, with 14
sort patterns). Its full CLI/options matrix is **outside this clarified page
scope**. The external `benchmarks` Makefile target references a sibling
`quickjs-benchmarks` corpus containing Kraken, SunSpider and Octane. Those
external corpora, Dromaeo and newer V8/Octane variants are also **out of scope**.
The page must not imply it covers every benchmark target in the QuickJS
Makefile; “all ten V8 v7 cases shipped in the pinned extras archive” is precise.

## Preserve the upstream workload and harness

`base.js` runs setup once, measures a **one-second warmup**, then accumulates
**one-second measurement batches until at least 32 benchmark invocations** have
completed. It reports total measured elapsed time divided by those invocations
in microseconds. Setup and teardown are outside that measured interval. A slow
case can therefore exceed two seconds by a large margin; reducing the minimum
invocations changes the official sampling protocol.

The suite replaces `Math.random` with a deterministic generator. A fresh
context gives all three execution paths the same initial generator state.
When exposing `Crypto/Decrypt` as a standalone selection, first run Encrypt
outside the measured interval to supply its required ciphertext. An isolated
Encrypt selection can run Decrypt outside timing to retain the original
round-trip correctness check. Report those dependencies explicitly.

Using the host's monotonic clock in place of `new Date()` is a host adaptation
that must be listed in the source manifest. Preserve the one-second thresholds,
the 32-invocation minimum and the benchmark bodies. The initial page prototype's
250 ms warmup/750 ms sample was shorter than upstream and must not be presented
as the original harness. Whole-case elapsed time, module loading, source parsing
and JIT statistics are useful separate measurements; they are not the upstream
per-invocation result.

The historical suite score is **100 × reference / geometric mean of its case
times**, using microseconds throughout. Crypto and EarleyBoyer each have two
cases. A standalone Encrypt, Decrypt, Earley or Boyer time is not a complete
suite score. Report case timings directly and compute suite scores only when
all required members are present. With one retained official run, “time per
invocation” is clearer than suggesting a statistically meaningful multi-sample
median. Retain counts, elapsed batches and raw observations in JSON.

“Upstream checks preserved” is accurate for this suite. “Every numerical result
independently verified” is not: RegExp and NavierStokes lack output oracles in
this version. Errors in setup, workload, teardown and dependency preparation
must all prevent a successful result or score.

## Packaging and browser requirements

The suite is JavaScript and can run in a Worker without a shell filesystem or
DOM. Keep the complete upstream source accessible locally, preserve source
order, disable only the automatic CLI entry and connect the selected case to
the original harness. The browser path should load scripts as files rather
than require dynamic code generation merely to start the adapter. QuickJS can
evaluate the equivalent concatenated source through the embedding bridge.

Retain the full extras archive and original source notices. The collection has
mixed per-file notices: V8 BSD-style terms, MIT-style notices, and DeltaBlue's
GPL-2.0-or-later notice. It is not accurate to label every contained benchmark
MIT because the QuickJS interpreter is MIT licensed. The complete original
combined source preserves these notices and the archive includes the README
and component files for inspection. The upstream archive does not include the
GPL 2.0 license text, so the browser distribution also ships that complete text
as `quickjs-extras-LICENSE.txt`.

Splay's original 8000-node tree with depth-five payloads is intentionally large.
Size the guest memory and interpreter allocation limit for the actual workload;
do not lower the tree size to make the demo pass. Both QuickJS targets need an
explicit, comparable allocation policy. Source buffers, heap and stack must not
overlap. Stop must terminate the current Worker even while QuickJS runs a
synchronous benchmark.

## Acceptance criteria and independent implementation review

Completion requires the actual **ten cases × three runtimes** to execute,
including standalone Crypto/Decrypt, with the exact pinned inventory and intact
upstream assertions. A case failing or exceeding a documented time limit must
remain visibly failed/incomplete rather than disappearing from the results.
Inspect real results on desktop and mobile, plus cancel, retry, missing source
and invalid result states. Recalculate displayed times, sample statistics and
any suite scores from retained observations.

The initial controller/Worker review identified these concrete issues to close
before acceptance:

- The initial standalone Decrypt omitted its Encrypt dependency. Preserve the
  dependency outside timing and test it in a fresh context.
- The first Worker computed `loadMs` by subtracting a just-computed elapsed
  time from the same interval, effectively reporting zero while including load
  time in `runMs`. Use distinct preparation and execution boundaries and expose
  a real executing progress state.
- The official lab must coordinate busy state with the existing C and
  LCG/sieve/pi labs; concurrent experiments compete for the same CPU. Retain
  runtime order, browser/build provenance and whether the page was backgrounded.
- Clear stale exported reports when starting another run. Cancellation and
  failure must leave explicit partial-result states and restore controls.
- Validate case identity, runtime identity, finite nonnegative timing values,
  retained counts and recomputed summaries before displaying or exporting a
  successful row. An array-shaped `samplesMs` alone is insufficient evidence.
- Keep a per-case execution limit and cancellation boundary. “Run all” must
  report progress across all 30 executions; one slow case must not silently
  hold the page in a permanent loading state.

These are source-review findings and acceptance requirements. The final local
run reports and rendered screenshots establish which have been resolved; this
research record does not substitute for that runtime evidence.

The follow-up source review confirmed corresponding implementation changes:
the adapter now calls the upstream `RunStep`/`RunSingleBenchmark`, retains its
one-second batches and 32-invocation minimum, prepares standalone Decrypt and
checks standalone Encrypt through Decrypt. It explicitly states the absence of
independent numerical checks for RegExp/NavierStokes. The dedicated official
Worker separates preparation from execution and uses the same 192 MiB QuickJS
allocation limit on ARM and Wasm. The controller validates the exact ten-case
inventory and raw timing relationship, coordinates the other labs, retains
partial failures/cancellations, records browser/build provenance and applies a
ten-minute limit per case/runtime. Complete multi-case suite scores use the
geometric mean and are labelled as a browser adaptation because the cases use
separate Workers. Extras extraction also uses a separate cache directory so it
cannot delete the interpreter's source tree. These source corrections still
need the all-30-executions acceptance run and rendered-state checks.

The original five-minute guard proved too short on a hosted AMD EPYC 7763
runner with Firefox: the retained report from Pages run `41468093784` identifies
only `RegExp/RegExp` on ARM as terminated at the limit; the other 29 measurements
passed. The same cloud-built RegExp guest completed locally in Firefox in 239
seconds, and the earlier Chromium run took 273 seconds. The ten-minute guard
accommodates slower hosts without reducing the fixed upstream work or accepting
timeouts as successful measurements. Stop still terminates the Worker, and the
browser acceptance runner retains its 30-minute overall reporting limit. This
change requires a fresh full acceptance run; those observations do not establish
a performance guarantee.

## Static presentation and embedding review

The first official-lab desktop and 320 px screenshots were reviewed while the
full ARM acceptance run was active; no competing CPU benchmark was started.
The desktop establishes a clear selection/action/status hierarchy and explains
the original sampling constraints before execution. The narrow initial capture
predated the revised full-width primary action, so that layout needs a fresh
capture. Two explicit minimum widths also needed mobile overrides: the select's
16 rem minimum can exceed the card's content width, and the notes' 18 rem
minimum can exceed the page width with a classic scrollbar. Set their mobile
minimum width to zero and verify against `clientWidth`, not `innerWidth`.

A further interaction issue in the initial controller is rebuilding the entire
result subtree for every case/runtime transition. That discards open statistic
disclosures, horizontal table scroll and keyboard focus while the visitor is
reading a long-running comparison. Preserve those states when rendering updates
or update the relevant cells in place. This needs a runtime interaction check
in addition to the initial screenshots.

The generic embedding review covered `qjs_eval`, the ARM mailbox/startup path,
allocator accounting and Worker source placement. For the shipped trusted
benchmark bundle, the bridge checks its source length and NUL termination,
creates a fresh QuickJS runtime/context and disposes values/context/runtime on
completion or a script error. The official path uses 256 MiB guest RAM with
192 MiB tracked QuickJS allocations; source starts at RAM size minus 8 MiB and
is limited to less than 4 MiB, while the stack starts near the RAM end. The
existing basic host remains 16 MiB. These are appropriate bounds for this
fixed-source integration, not a general-purpose untrusted-code sandbox or a
claim that arbitrary externally supplied mailbox pointers are validated.

## Final presentation review with recorded real measurements

On **2026-10-31**, `test-results/official-v8/browser-chromium.json` contained
successful real results for all **30 case/runtime combinations**. Its full
execution, export and layout checks passed; a later cancellation-test race was
recorded as a verifier error and then checked separately in
`browser-recovery.json`. `browser-chromium-validation.json` records that
distinction rather than hiding the initial verifier error.

The final independent visual review replayed those **unchanged Worker result
objects** into the current page/controller. It executed no benchmark workloads,
changed no distribution files and produced no new timing measurements. The
purpose was to verify the latest responsive layout and interaction refinements
while Firefox's real benchmark run used the CPU. Evidence and the replay driver
are in `test-results/official-v8/presentation-replay/`.

The archived measurement report's SHA-256 is
`44b07090c55046df112e430844f3b672439a0e3805dc491fad8715e6fc72f2a5`.
The reviewed source overlay was:

| Source | SHA-256 |
| --- | --- |
| `examples/browser/index.html` | `1a2c9dd8409552bae815a970c82e4192a9cc538145db3941b76ec490eee1788c` |
| `examples/browser/styles.css` | `fc67703d9409d96c3edf38897ef6956516706ce767037258362c11de4526264d` |
| `examples/browser/official-benchmarks-app.js` | `c6abbc0cdce10df38e805157751483392c77a155348f3bcca40fdcdc38e63930` |

The six reviewed combinations—320, 390 and 1440 CSS pixels, each in light and
dark mode—had no page or result-table horizontal overflow. Mobile shows all
three engines vertically under each case with explicit labels. The two minimum
width issues from the initial review are corrected. Desktop retains the compact
comparison table; the single-name suites no longer repeat their name twice.

Actual replay-driven updates preserved open case disclosures, focused summary
controls, table-region focus and a deliberately established 90 px horizontal
scroll position. All 30 final result objects compared equal to the archived
measurements. Thus the state-preservation finding is closed by interaction
evidence rather than only a source change.

The initial expanded-state accessibility review caught a heading-level jump
from the section's `h2` to runtime `h4` headings. After changing those headings
to `h3`, axe reported **zero violations in all six collapsed and all six
expanded cases**. The only incomplete item concerned the decorative,
`aria-hidden` arrow character inside Run, not an unresolved text contrast
failure. A separate computed-color check covered up to **571 visible text
elements** with expanded statistics: minimum contrast was **5.77:1 in light
mode** and **8.04:1 in dark mode**. These checks supplement manual inspection;
they do not constitute accessibility certification.

The final desktop/mobile/dark screenshots in that directory are presentation
replays of authentic local measurements. They should be labelled accordingly
when retained as documentation previews. No new visual or interaction defect
was found in this final independent review; final built-artifact verification
and cross-browser execution remain separate evidence.
