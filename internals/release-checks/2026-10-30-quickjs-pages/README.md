# QuickJS example and Pages release-candidate review

Local review: **2026-10-30**, after backend correction
`6701c8dec3a21adf78b13939b40d1fa421bc9371`. No deployment or remote repository
changes were made. The backend correction and its 24-case/strict-regression
records are in the [separate fix](../2026-10-30-thumb-it/README.md).

## Delivered experience

- A reproducible **QuickJS 2025-09-13** example, compiled from one checksum-pinned
  upstream source archive to bare-metal ARM32 and direct WebAssembly. Both use
  the same embedding source and allocation accounting; complete source and
  runtime notices accompany the distribution.
- One unchanged JavaScript source for **LCG / sieve / pi**, executed through
  dynarmic.js + ARM QuickJS, direct Wasm QuickJS, and the browser JavaScript
  engine. Preview and Extended profiles have independently verified answers.
- First-pass timing, one warmup and three measured samples per kernel, median
  and range charts, raw samples, loading/complete-call boundaries, outputs,
  memory capacity and applicable JIT statistics. Exports include browser context,
  parameters, sample order, both build manifests and runtime hashes.
- A focused three-path visual explanation, responsive controls, live truthful
  states, cancellation and retry, a success-to-results link, and progressive
  statistics. The existing ARM32/AArch64 C playground remains available.

Research and iterative design reviews, including cited award criteria and
GitHub implementations, are in [the research record](../../../docs/research-release-quality.md).
The actual runtime is the distinctive interaction; no fabricated live traces,
benchmark figures, awards or stability claims are used.

## Evidence and scope

| Requirement | Current local evidence |
| --- | --- |
| Same source and known results | `node-light.json`, `node-full.json`; both profiles pass all three paths. `examples/quickjs/reference.py` independently checks integers, primes and Decimal pi. The final build manifest binds the exact shared source to both QuickJS artifacts. |
| Real browser execution | `quickjs-chromium.json`: eight checks, including the three Workers, downloaded JSON, exact build hashes, both loading/executing cancellation, missing Worker and retry. `firefox-quickjs.log`: actual Firefox Worker comparison passes. |
| Preserve C examples | `c-guests-chromium.json`: 17 interaction checks across all memory modes, errors, retry, keyboard entry, copying and reduced motion. `firefox-guests.log` passes both C guests. |
| Fault boundaries | `node-tests.log`: 38 tests, including malformed measurements/build provenance, failed assets, timeout, cancellation, obsolete Worker events and exclusion between the two labs. `package-tests.log`: 12 isolated packaging tests. |
| Visual review | `desktop-results.png`, `mobile-results.png`, `dark-results.png`, `visual-review.log`: real Extended results reviewed at 320/390/1440 px; the complete page and tablet layout were also reviewed. Final fixes improve result typography, narrow action layout, result navigation and named groups. |
| Accessibility | `accessibility.json`: zero axe violations at 320/390/768/1440 px and dark/light, plus expanded statistics. The initial reports retain manual-review items; `contrast-review.log` supplements them with 308 visible text checks (minimum 5.40:1 light, 6.56:1 dark). Named groups were subsequently corrected. Keyboard, focus and reduced-motion checks remain covered by real-browser tests. No full accessibility certification is claimed. |
| Initial-page performance | `lighthouse.json.gz`: local mobile Performance/Accessibility/Best Practices/SEO **100**, FCP/LCP **0.9 s**, TBT **0 ms**, CLS **0**. Heavy engines load only after Run. These are local lab observations, not field Core Web Vitals or guest-performance promises. |
| Source distribution | `source-rebuild.log` / `source-rebuild-result.json`: extract the project source archive, reuse its included upstream QuickJS archive without a download, rebuild both guest targets, and pass the three-runtime Node comparison. Final packaging separately verifies current source and artifact hashes and excludes editor swaps. |
| Maintenance and release flow | CMake builds all assets; CI adds the QuickJS reference, Node and both-browser gates; explicit newlib dependencies and linked runtime notices are included. Workflow validation remains local, with deployment still manual and opt-in. |

The tests ran on Chromium 133, Firefox 157.0.1 and Node 24.19.0 in this
workspace. No remote CI success is claimed. Timing values in screenshots are
snapshots of those devices, not values embedded in the page.

## Publication boundary

The QuickJS/Pages candidate remains uncommitted for review, separate from the
requested backend-fix commit. Its source manifest correctly records a dirty
snapshot and does **not** certify clean source/runtime correspondence. After
committing the candidate, a publication build must rebuild and use
`package_source.py --require-clean`; the Pages workflow enforces this gate.

Public GitHub/documentation URLs target the intended repository and paths.
Their public availability belongs to the later repository publication; the
current local distribution's source and license downloads work independently.

The award references informed design, usability, content and interaction review.
They do not constitute an award or a prediction of a jury decision. The concrete
visual and functional defects identified during this local review were fixed
and the relevant checks repeated.

## Reproduce locally

```sh
cmake --build build-em --target browser_page -j2
python3 examples/quickjs/reference.py
node examples/quickjs/run.mjs build-em/examples/browser/dist light
node examples/quickjs/run.mjs build-em/examples/browser/dist full
node --test examples/browser/*.test.mjs
python3 -m unittest discover -s tests/browser -p '*_test.py'
node tests/browser/verify_interactions.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist
node tests/browser/verify_quickjs.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist --quickjs
python3 examples/browser/package_source.py --dist build-em/examples/browser/dist
python3 -m http.server 8137 --bind 127.0.0.1 \
  --directory build-em/examples/browser/dist
```

The local preview is at `http://127.0.0.1:8137/#quickjs`. `source-sha256.json`
identifies the implementation inspected here. Supporting generated work files
and the complete screenshot collection are in `test-results/quickjs-review/`.
