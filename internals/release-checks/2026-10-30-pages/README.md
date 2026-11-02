# Pages release-candidate review — 2026-10-30

Local-only review of the worktree based on
`f8e098efc3056f94cd197812df7d0a95fdf55f65`, with Dynarmic
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. Nothing was pushed or deployed.
`source-sha256.json` identifies the final implementation reviewed here.
The runtime remains experimental; no stable npm API or performance comparison
is claimed. Existing compiler/backend/guest source is unchanged.

## Requirements and evidence

| Requirement | Evidence |
| --- | --- |
| Research awards and open-source practice | [PagesDesign.md](../../../docs/PagesDesign.md) records primary Awwwards, Webby and FWA criteria plus Vite, Astro and GSAP website/repository research. |
| Improve through self-review | First generic visual pass was rejected: fabricated LIVE label, excessive hero height and weak adoption path. Second pass replaces it with a truthful translation diagram, working run/docs routes, architecture exploration and integration commands. An independent review found an accessible-name mismatch and no-JS dead controls; both were corrected. |
| Desktop and mobile craft | `desktop.png`, `mobile.png`, and `presentation.json`: light/dark, 320/390/768/1280/1440px, 200%-equivalent reflow, no-JS and reduced motion. No horizontal overflow, page exceptions or failed initial requests. The hero action is visible in normal desktop/mobile viewports. |
| Real interaction and execution | `chromium-interactions.json` and `chromium-profile.json`: 17 checks each. Three guest-memory modes, exact native-reference outputs, infinite-loop Stop, retry, missing resources, nested hosting, keyboard entry, on-demand Wasm, architecture stages, copying, denied-clipboard fallback and reduced motion. Reports include the exact tested artifact hashes. |
| Browser compatibility | `firefox.log` and `firefox-profile.log`: real Worker smoke passed for profiling OFF/ON. Chromium 133.0.6943.53; system Firefox 157.0.1; Node 24.19.0. |
| Accessibility | `presentation.json`: axe 4.14.0 found zero violations in light/dark. SVG text/background contrast remains an automated-review item; inspected visually against the explicit light/dark color tokens. Semantic groups, visible focus, skip navigation and no-JS disabled controls were reviewed. This is not a screen-reader certification. |
| Performance and metadata | `lighthouse.json.gz`: local mobile Lighthouse Performance/Accessibility/Best Practices/SEO all 100; FCP/LCP 0.9s, TBT 0ms, CLS 0. No runtime download before Run. These are local lab measurements, not field Core Web Vitals. |
| Recoverability | `presentation.json`: nested custom HTTP 404 serves styles/icons from the project root and returns to the working homepage. Source/license downloads return 200. |
| Focused regressions | `unit.log`: 59 browser/CMake tests pass. `packaging-tests.log`: 10 isolated Git/tar/SDK tests pass. Host fault/bounds checks pass in both builds (18 normal/fault runs and 216 RAM boundary runs each). |
| Rebuildable source distribution | `archive-inspection.json` records the tested snapshot hash and 3,091 extracted files checked byte-for-byte, without Git/build/cache files. `archive-rebuild.log`: independent 221-step Release build from that archive, then ARM32/AArch64 Node hello PASS. Runtime/build source was unchanged by the later notice/page refinements. Final packaging adds matching SDK license texts and rejects missing or mismatched SDK inputs. |
| Safe publication workflow | `actionlint v1.7.7` passes. Push/PR runs validate only; manual deploy defaults false. Browser verification and strict source/runtime correspondence precede artifact upload. Only the explicit deploy job has Pages/OIDC writes. No remote workflow success is claimed. |

## Scope and publication boundary

This is a local, uncommitted release candidate. The generated source manifest
truthfully records `dirty: true` and `correspondenceVerified: false`; it must not
be relabelled as a clean release. After committing the candidate, rebuild and
use `package_source.py --require-clean` before publication. The workflow enforces
that rule automatically.

Anonymous HTTP checks to the project's GitHub repository currently return 404.
The outgoing repository/documentation paths match real files in this checkout,
but public link availability must be checked when the repository is published.
No repository visibility, Pages settings or remote contents were changed. Local
source and license downloads work independently of public repository access.

The unchanged native/compiler baseline has prior strict-regression, native
CTest and sanitizer evidence in the adjacent release-check records. This page
review does not pretend to rerun or replace all compiler-performance gates.

The visual target is the awards' documented design/usability/creativity/content
and overall-experience criteria. Scores and an independent local review support
a high-quality release candidate; they cannot certify an award or predict a jury.

## Reproduce

```sh
cmake --build build-em --target browser_page --parallel 2
python3 examples/browser/package_source.py --dist build-em/examples/browser/dist
node --test examples/browser/*.test.mjs tests/cmake/*.test.mjs
python3 -m unittest discover -s tests/browser -p '*_test.py'
node tests/browser/verify_interactions.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist --report /tmp/pages-interactions.json
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist
```

Repeat browser tests with `build-em-profile`. For lab scores, serve the complete
dist and run Lighthouse 13.6.0 with a local URL, `--output=json` and
`--chrome-flags='--headless --no-sandbox --disable-gpu'`. See
[the example guide](../../../examples/browser/README.md) for 404 path simulation
and [the release guide](../../../docs/Releasing.md) for publication gates.
