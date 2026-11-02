# Release experience research: QuickJS comparison

Reviewed **2026-10-30**, using the public websites and GitHub source links below.
This supplements [PagesDesign.md](PagesDesign.md), which contains the broader
visual direction and local acceptance criteria, and
[the example maintenance guide](../examples/MAINTAINING.md). It records design
and measurement decisions, not an award certification or completed test report.

## Primary sources checked

All URLs in this table were retrieved successfully over HTTPS during this
review. GitHub source links pin the inspected revision; those pinned files were
also fetched and compared with the branch versions inspected during research.

| Source | Observed practice | Application to dynarmic.js |
| --- | --- | --- |
| [Awwwards evaluation](https://www.awwwards.com/about-evaluation/) | Published weights: design 40%, usability 30%, creativity 20%, content 10%. | Judge the complete composition and visitor journey, not only a functional demo or a performance score. |
| [Webby website judging criteria](https://www.webbyawards.com/judging-criteria/) | Content, structure/navigation, visual design, functionality, interactivity, innovation and overall experience. Functionality explicitly covers working links, fast loading, different platforms, access needs and bandwidth. | Loading, cancellation, failure recovery, keyboard operation and the path from demo to documentation are part of the design. |
| [FWA About](https://thefwa.com/about/about-fwa/) and [Live Judging](https://thefwa.com/live-judging/) | Public descriptions emphasize innovation in digital design/development and showing what technology makes possible. The client-rendered HTML includes these descriptions in its configuration. | Make the distinctive achievement tangible: JavaScript inside an ARM QuickJS interpreter, dynamically translated to WebAssembly in the visitor's browser. Do not infer unpublished category weights. |
| [Vite hero source](https://github.com/vitejs/vite/blob/90bc034a97e7eccd0f3cbc69904fffa625a4c34d/docs/.vitepress/theme/landing/Hero.vue) | Concise category statement; adjacent Get Started and GitHub actions; a bounded illustration column; real starter commands. | Keep an unmistakable first action and a direct adoption path. A compelling illustration must not displace the controls or headline. |
| [Astro landing source](https://github.com/withastro/astro.build/blob/26c37fb0096f2857b6dacd96334465bed50d8bf0/src/pages/index.astro) | The page composes separate hero, feature, showcase and adoption sections. | Maintain a deliberate narrative and separate components instead of accumulating unrelated cards and CSS overrides. |
| [Speedometer source and protocol](https://github.com/WebKit/Speedometer/blob/b0bc16efc667dc8ded72397a29c903468dc279e0/README.md), [live benchmark](https://browserbench.org/Speedometer3.1/) | The protocol explicitly identifies unmeasured preparation and measured steps. It retains multiple iterations and explains aggregation. The project distinguishes end-to-end responsiveness from tight feature loops. | Publish the timing boundary; show repeated observations; call LCG/sieve/pi microkernels, not a representative browser or application benchmark. Do not copy Speedometer's score formula into unrelated workloads. |
| [QuickJS official site](https://bellard.org/quickjs/), [C API documentation](https://bellard.org/quickjs/quickjs.html) | QuickJS is an embeddable interpreter with explicit runtime/context ownership. The API provides memory, stack and interruption limits. Sources are MIT licensed. | Use a pinned, checksummed release; document initialization and cleanup; retain its license; bound execution. |
| [quickjs-emscripten README](https://github.com/justjake/quickjs-emscripten/blob/7b7af98e4e69757c64c27aac46a74e1e07229545/README.md) | Documents explicit handle/context disposal, memory/stack limits and interruption. Distinguishes release/debug, synchronous/Asyncify, and original QuickJS/quickjs-ng variants. | Compare the same QuickJS revision and appropriate build variants. An unpinned npm default is not evidence of equivalent engines. Keep runtime loading separate from evaluation, and clean up on every exit path. |
| [MDN: performance.now](https://developer.mozilla.org/en-US/docs/Web/API/Performance/now) | Uses a monotonic time origin and returns elapsed milliseconds. Each Worker has its own time origin. | Measure elapsed intervals within the executing Worker. Do not subtract unrelated Window and Worker timestamps or report precision unsupported by the timer. |

## Comparison contract

The three requested paths must execute the **same JavaScript source and input**:

1. **QuickJS on ARM → dynarmic.js → WebAssembly.** The ARM guest contains the
   QuickJS interpreter. dynarmic.js translates the interpreter's ARM instructions;
   it does not compile JavaScript directly.
2. **QuickJS → WebAssembly.** The same interpreter source release is compiled
   directly with Emscripten. Its Wasm execution still uses the browser engine.
3. **Browser JavaScript.** The same workload runs directly in a Worker using
   the browser's JavaScript engine.

Use one canonical workload file and deterministic parameters. Record its hash
with the QuickJS release, archive hash, compiler options and output artifact
hashes. Share LCG integer arithmetic, sieve bound and pi series across the
paths; even small arithmetic differences such as omitted unsigned coercion or
different rounding make a comparison invalid. Validate all outputs against
independent, known answers rather than letting one unverified path define truth.

The directly compiled interpreter and ARM interpreter have different ABIs,
memory access implementations and compiler backends. Browser JavaScript has a
different language implementation entirely. These are execution-path
comparisons, not a controlled isolation of one compiler optimization.

### Timing and statistics

Expose these distinctions in the result details and exported observations:

| Metric | Meaning and reporting rule |
| --- | --- |
| Asset preparation | Fetching and runtime initialization. Keep separate from measured execution; cached and uncached fetches are not interchangeable. |
| First execution | Label the exact boundary. If it starts at the host entry, it can include QuickJS initialization, script parsing, execution and cleanup. The ARM path additionally includes dynarmic translation and browser compilation of emitted modules. |
| Repeated execution | State whether the same JIT/interpreter/context is reused. A new Worker for every run is repeated cold execution, not warm throughput. |
| Samples and dispersion | Retain individual elapsed durations. Use a named summary such as median with min/max or MAD and show the sample count. One sample must be presented as one observation. Do not imply a confidence interval from a range. |
| Output verification | Per-path LCG value, prime count and pi result, checked before a sample is accepted. Report mismatch as failure; never rank a wrong or timed-out result. |
| dynarmic statistics | Report available compile phases, emitted module count/bytes and cache counters from actual runtime snapshots, with units and scope. They apply only to the dynarmic path; use “Not applicable” for other paths rather than zero. |
| Memory | Identify allocated linear memory, guest RAM and QuickJS allocator usage separately if available. They are different quantities and must not share an ambiguous “memory usage” label. |
| Environment | Browser/user-agent, workload parameters, memory mode, build provenance, sample order and timestamp. A browser cannot reliably infer the physical CPU model; do not invent it. |

Run paths sequentially so they do not compete for CPU. If collecting repeated
comparisons, rotate order to reduce consistent first/last bias. Explain when a
run is interrupted by tab backgrounding, cancellation or a time limit; retain
that state rather than reporting a completed benchmark. Avoid automatic heavy
execution on page load. A quick preset can keep the first encounter practical,
with larger deterministic workloads available explicitly.

The primary result should answer three questions at a glance: **Did it work?**,
**what ran?**, and **how long did this run take?** Place compiler internals and
full provenance in expandable details. Relative bars can aid comparison after
successful measurements, but each bar needs a textual duration and engine
label. Do not display a fabricated baseline, synthetic loading percentage or
pre-filled performance result before a real run.

## Design direction for the added experiment

Use the page's translation motif to explain the extra interpreter layer. A
compact three-lane diagram can make the difference clear before execution:

```text
Same JavaScript source + inputs
  ├─ QuickJS / ARM → dynarmic.js → WebAssembly
  ├─ QuickJS / WebAssembly
  └─ Browser JavaScript
                          → checked results + measured durations
```

Give the section one obvious Run action, one Stop action, a readable current
state, and three result lanes with consistent labels. Keep the existing simple
ARM demo available: it teaches the embedding contract; QuickJS demonstrates a
substantial real guest. Cross-link the new example source and build instructions
so visitors can reproduce the result outside the page.

Visual polish means controlled hierarchy, aligned numeric columns, legible
units, deliberate whitespace and useful feedback in every state. Do not add
continuous motion that competes with the running experiment. Any execution
animation must follow actual state and stop for reduced-motion preferences.
Use the existing site's own visual vocabulary; these research sources are not
templates to copy.

## Review against the actual rendered result

The pre-change page already had a coherent hero, a real two-architecture C
demo, an interactive architecture explanation and a Node quick start. The
explicit gap was the substantial QuickJS guest and three-path JavaScript
comparison. The acceptance criteria below are requirements for that addition,
not a claim that it has already passed:

- Run all three paths on the same canonical LCG/sieve/pi program and show
  verified outputs plus useful statistics. Test the actual built Wasm and ARM
  artifacts, not only host mocks.
- Inspect empty, loading, running, success, cancelled, mismatch and missing-asset
  states. Stop must terminate execution; retry must not retain stale results.
- Confirm nested GitHub Pages paths, local runtime assets and license/source
  distribution. The landing page must not download the full runtime just to
  display decoration.
- Review full-page screenshots and result details at 320/390 px mobile,
  768 px tablet and 1280/1440 px desktop, in both color schemes. Check focus,
  200% zoom, reduced motion and keyboard-only operation.
- Keep numerical cells readable and disclose very short measurements without
  implying microsecond accuracy. Long logs should not force horizontal page
  scrolling or overwhelm the primary result.
- Evaluate the visual result again after interaction testing: a correct demo
  with crowded results, weak hierarchy or an unclear first action still needs
  improvement. Use local evidence to describe readiness; only the respective
  award bodies can confer an award.

Local test results and screenshots belong in the release evidence directory;
this research document should not substitute for inspecting those artifacts.

## Independent local visual review: initial QuickJS implementation

Reviewed on **2026-10-30** against real Chromium renders of
`examples/browser/index.html` and `styles.css`, served locally under a nested
path using `tests/browser/chromium.mjs`. The review covered 320, 390, 768 and
1440 CSS-pixel viewports, light/dark themes, the supplied initial screenshots
and fresh screenshots after corrections. These are observations of the
reviewed iterations; later changes still require rechecking.

### Findings and verified corrections

| Finding in the first iteration | Correction | Evidence from the follow-up render |
| --- | --- | --- |
| At 320 px the hero's non-wrapping `Your browser.` extended beyond the usable viewport. | Reduced narrow-screen headline sizing. | `documentElement.clientWidth` and `scrollWidth` both measured **305 px**, with no element extending past that edge. Before correction, scroll width was **317 px**. |
| On mobile, the explanation cards pushed Run below the first experiment viewport. | Moved controls and status before the three execution paths. | After navigating to the lab, Run moved from approximately **y=1162 to y=533** at 320 px and **y=1051 to y=509** at 390 px. It remained visible at tablet and desktop sizes. |
| Horizontal flow arrows pointed right while their destination wrapped to the next line. | Used vertical steps on small screens. | The updated narrow-screen layout retains an unambiguous reading sequence instead of a wrapped arrow chain. |
| The introduction described ARM and Wasm as the “same QuickJS build”. | Changed the wording to the same QuickJS **source compiled to WebAssembly**. | The rendered introduction now describes the relationship accurately. |

The overflow finding also exposed a weak test assertion: comparing
`scrollWidth` with `innerWidth` can miss overflow when a classic scrollbar
reduces usable layout width. Compare with `documentElement.clientWidth` and
inspect the actual screenshot. In this test, `innerWidth` was 320 while the
usable width was 305.

A computed-color review of **198 visible text elements** in the corrected
initial state found minimum text contrast ratios of **5.40:1 in light mode**
and **6.56:1 in dark mode**. The check waited for theme transitions to settle,
resolved inherited solid backgrounds, and excluded disabled controls and SVG
text. This is evidence for those initial text colors, not a complete WCAG audit
or proof about results, errors, focus indicators, graphical objects or forced
colors. Human inspection remains necessary.

### Editorial and creative follow-up

The hero has a clear proposition, a visible action and a bounded schematic.
The restrained blue/ivory palette and darker experiment area give useful
hierarchy. The first-pass three-column path lists, however, remain a familiar
information layout. The distinctive material is the nested interpreter and
translator; the visual language should make that relationship easier to see.

The most useful next refinements are:

- Keep a common source/input and common verified-result destination legible
  across the three paths. Reuse consistent path names and A/B/C identifiers in
  the timing charts and details so the explanation and result remain connected.
- Tie running and verified styling to real execution and validation states.
  Retain the full explanation without animation, including reduced-motion mode.
- Direct the skip link to the primary QuickJS experiment. Keep the narrowest
  action layout intentional when Run and Stop wrap onto different rows.
- Prefer a neutral result heading such as “Three paths. Measured here.” over
  “What did the extra layers cost?”, which can imply isolated translation
  overhead despite the more careful methodology text.

At this initial review stage, actual ARM QuickJS execution was still under integration.
Successful runtime results, expanded statistics, cancellation, failure recovery
and the complete rendered result journey therefore remained **unverified**.
The initial-state corrections improve the design but do not close the release
or award-quality review. Inspect real results after the runtime works; do not
use fabricated result screenshots as acceptance evidence.

## Follow-up review: actual Extended results

The integration uncertainties above were revisited on **2026-10-30** after the
Thumb ITSTATE backend fix. This review inspected the actual Extended comparison,
not populated mock results. Local evidence is under
`test-results/quickjs-review/` (generated artifacts, not website assets).

### Runtime and presentation evidence

| Review item | Evidence inspected | Result |
| --- | --- | --- |
| Three real execution paths | `browser-extended.json`, `chromium-quickjs.json`, `firefox-quickjs.log` | ARM QuickJS, direct Wasm QuickJS and browser JavaScript returned the Extended references: LCG **428440788**, sieve **6542**, pi×1e6 **3141587**. The Chromium report includes artifact hashes, inputs, sample order and environment. Firefox's actual comparison smoke also passed. |
| Measured chart values | Raw repeated samples in `browser-extended.json` and `desktop-lab.png` / `mobile-results.png` | Independently recomputed all nine medians and ranges; they match the displayed values. The charts retain a linear scale per kernel and numerical labels even when a fast path's bar is very short. |
| Result details | `dark-expanded-statistics.png` and current renderer source | Outputs, preparation, first pass, repeated samples and memory capacity are separated. Only the ARM path presents dynarmic compile statistics. Direct Wasm and browser JavaScript explicitly mark those statistics not applicable. The current renderer additionally explains the complete-call timing boundary. |
| Lifecycle and export | Eight named checks in `chromium-quickjs.json` | Nested-path lazy loading, all three real runtimes, exported JSON, narrow layouts, cancellation during synchronous ARM execution and startup, missing Worker failure and retry passed. |
| Page journey | `desktop-complete.png`, with focused inspection of hero, both labs, architecture and adoption sections | The proposition, real experiment, architectural explanation and build path form a coherent sequence. Supporting technical detail remains progressively disclosed. |
| Responsive layouts | `visual-accessibility.json`; independent final renders at 320, 390 and 1440 px | Document scroll width equals usable client width. The previously overflowing title and wrapped path arrows are corrected. |
| Accessibility evidence | `visual-accessibility.json`, `independent-result-contrast.log` | Axe reports zero violations in the recorded viewport/theme cases. Its initial reports retain `aria-prohibited-attr` and `color-contrast` items for manual review; these were not silently counted as passes. A separate real Extended run with expanded details checked **308 visible text elements**: minimum **5.40:1 light**, **6.56:1 dark**, with no ratio below 4.5:1. |

The supplementary contrast check resolves computed foreground colors against
their solid ancestor backgrounds after theme transitions settle. It excludes
disabled controls and SVG text, so it supplements rather than replaces manual
and automated accessibility review. The named execution-path and chart
containers now have `role="group"`, making their existing accessible labels
meaningful rather than attaching names to generic containers.

### Final refinements driven by the screenshots

The real result screenshots exposed two additional usability issues that were
less apparent in the empty state:

- **Result typography was too small.** Runtime labels were about 10.9 px and
  ranges about 9.8 px. Labels are now 12 px, ranges 11.2 px and the primary
  duration 13.1 px. The final 320/390 px renders retain aligned labels and
  readable numbers without clipping.
- **Successful mobile runs needed a direct route to the results.** The path
  explanations occupy substantial vertical space. A **View results** anchor
  now appears with the successful status and targets the measured results.
  It stays hidden before success and does not automatically move focus or
  interrupt the visitor's reading position.

The earlier editorial changes are also present: the skip link reaches the
QuickJS lab, the result heading reads **“Three paths. Measured here.”**, and Run
and Stop share a deliberate narrow-screen action row. Duration formatting no
longer adds a third decimal place to sub-millisecond values; very small
nonzero values are distinguished from a measured zero.

The final independent visual run used the current page/stylesheet/controller
over the built local runtime assets, executed Extended again and produced
`independent-final-{small,mobile,desktop,dark}.png`. It verified that the jump
link is hidden initially, shown after a successful run, and navigates to the
result section; both named containers expose their group roles. This is an
explicit source-overlay review, while the final build-artifact gates verify
the copied distribution separately.

The final composition uses its technical subject as the experience: one program,
three understandable execution paths, verified answers and inspectable measured
costs. The restrained schematic, technical typography, contrasting experiment
area and consistent result layout support that story. The concrete visual and
content defects identified in these iterations have been corrected; no further
rendering defect was found in the reviewed final screenshots. This is a local
design assessment against the stated quality criteria, not an award, jury score
or independent accessibility certification.
