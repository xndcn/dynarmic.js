# GitHub Pages design research and acceptance criteria

Research date: **2026-10-30**, design and local verification iteration updated
**2026-11-02**. Scope: the public dynarmic.js landing page and
browser demonstration, reviewed locally before publication. This document
records primary-source research, the deficiencies found in the first visual
pass, and the criteria for subsequent reviews. It is not an award certification
or a claim that an award jury would select the work.

## What the awards actually evaluate

The following pages were retrieved directly over HTTPS. The observations below
refer to the website categories, not unrelated film, advertising or app awards.

| Source | Published criteria | Consequence for this project |
| --- | --- | --- |
| [Awwwards evaluation system](https://www.awwwards.com/about-evaluation/) | Design **40%**, usability **30%**, creativity **20%**, content **10%**. | A technically correct demo is insufficient. The composition must be exceptional, the first useful action easy to find, and the central visual idea specific to runtime translation. |
| [Webby Awards: Websites and Mobile Sites, 2026/2027](https://www.webbyawards.com/judging-criteria/) | Content; structure and navigation; visual design; functionality; interactivity; innovation; overall experience. No category weights are published on this page. | Quality applies to the complete journey: understand the project, run it, interpret the result, find documentation, and know its limitations. |
| [FWA About](https://thefwa.com/about/about-fwa/) and [Live Judging](https://thefwa.com/live-judging/) | The public site describes its purpose as cutting-edge innovation in digital design and development, pushing technology's boundaries and showing what is possible. Its public judging text describes a score out of 100, at least 20 votes and a minimum score of 70 for FOTD eligibility, with the best qualifying score selected. | The remarkable part must be experienced. Running ARM guests in a browser is the project's distinctive material; an unrelated decorative animation would weaken that story. These public descriptions are not a published four-category weighting system. |

The Webby criteria are especially useful here:

- **Visual design** should be appropriate to the audience and message and
  improve inclusion, including for low-vision and cognitive disabilities.
- **Functionality** includes quick loading, working links, cross-platform
  behavior, bandwidth limitations and special access needs.
- **Interactivity** requires meaningful action and response, beyond a hover
  effect. The site's actual guest execution can satisfy this more convincingly
  than decorative motion.
- **Structure and navigation** should make the organization understandable and
  provide quick access to the breadth and depth of the content.

FWA is a client-rendered site. The retrieved HTML includes the public judging
and submission descriptions in its embedded configuration. This research does
not infer criteria from private jury instructions or invent numerical weights.

## Public-project references: observe both the site and its source

These projects are references for specific design decisions, not layouts to
copy. Their popularity, scale and testimonials cannot be borrowed as evidence
for dynarmic.js. Vite and Astro are open-source references. GSAP is a public
source/developer-tool design reference with its own standard license; it should
not be described here as an OSI-licensed project.

### Vite: make the next step unmistakable

- Website: [vite.dev](https://vite.dev/).
- Repository and onboarding: [vitejs/vite README](https://github.com/vitejs/vite/blob/main/README.md).
- Actual implementation: [Hero.vue](https://github.com/vitejs/vite/blob/main/docs/.vitepress/theme/landing/Hero.vue)
  and [Layout.vue](https://github.com/vitejs/vite/blob/main/docs/.vitepress/theme/landing/Layout.vue).

The hero states the category, gives a concise explanation, then places **Get
Started** and **View on GitHub** next to each other. The source confirms a
separate, bounded illustration column and a command group with actual package
manager commands. The rest of the page develops the proposition through
features, ecosystem and community, then returns to a clear start action.

Apply: put **Use the package** and **Explore the benchmarks** beside the project
proposition. Preserve a direct GitHub route. Keep artwork bounded so it cannot
enlarge the headline column or push the only useful action off screen. Publish
only build commands that this repository actually supports. The npm candidate
supports local tarball installation; until a registry release exists, use the
[local package instructions](JavaScript.md) instead of a registry install claim.

### Astro: connect identity, technical concept and adoption

- Website: [astro.build](https://astro.build/).
- Repository: [withastro/astro README](https://github.com/withastro/astro/blob/main/README.md).
- Website source: [index.astro](https://github.com/withastro/astro.build/blob/main/src/pages/index.astro)
  and [Hero.astro](https://github.com/withastro/astro.build/blob/main/src/pages/_components/landing-page/Hero.astro).

The hero names the audience and category, links to the docs and offers a real
starter command. The page then explains a distinctive technical idea (islands)
and ties performance claims to named datasets. Its repository starts with
installation and provides separate documentation, support and contribution
paths. The website source is divided by narrative sections, so visual ambition
does not require an unmaintainable page.

Apply: explain the actual transformation **ARM instructions → Dynarmic IR →
WebAssembly → browser execution**, then let the user verify the outputs. Give
an adoption route, a contribution route and an honest support-status route.
Keep measured timings attached to their workload, environment and caveats;
do not turn the demo timings into an unsupported performance claim.

### GSAP: the product should be the demonstration

- Website: [gsap.com](https://gsap.com/).
- Repository and usage examples: [greensock/GSAP README](https://github.com/greensock/GSAP/blob/master/README.md).
- Licensing context: [GSAP standard license](https://gsap.com/standard-license).

The site puts animation capability into its visual language, gives direct
Learn/Docs/Demos navigation and makes getting the library prominent. The README
provides actual installation/import examples and directs users to documentation,
demos, support and the license. The strong lesson is alignment between what the
tool does and what the visitor experiences, not adding GSAP as a dependency.

Apply: the compiler/processor imagery should explain translation and execution.
Use real run states when showing activity, and retain an informative still state
for reduced-motion users. A decorative code panel must be labelled as an
illustration, never as live telemetry.

## Strict review of the first visual pass

The first pass increased contrast and polish but **did not meet the stated
benchmark**. A gradient background, oversized type, rounded panels and hover
elevation are commonplace. They do not establish a memorable identity or a
complete open-source adoption experience. This is a review of that first-pass
snapshot, not a statement that later corrections are still missing.

| Finding | Why it fails the standard | Required correction | Verification |
| --- | --- | --- | --- |
| Static `guest_trace.log` panel labelled **LIVE** | It presents invented activity as runtime evidence. The code rows are also not a trace of the actual guest run. | Replace it with an explicitly illustrative translation diagram, or connect every displayed state to actual execution. | Initial, running, stopped and failed states must never imply fabricated telemetry. Inspect both DOM and runtime behavior. |
| At 1440 px, headline wraps to four lines and the Run control falls below the first screen | Typography overwhelms the task. A capable browser can reach the page without seeing its central action. | Bound title size/line length; add a prominent hero action; shorten unnecessary hero height. | Screenshot at 1440×900 and 1280×800: proposition and a working route to Run visible without scrolling. |
| The technical preview is arbitrary terminal decoration | It does not teach the system and could be pasted into a different developer tool's site. | Create a project-specific visual model of translation/execution, with concise labels and truthful states. | A new visitor can explain what goes in, what changes and where it runs after reading the hero. |
| Header offers Examples and GitHub but no obvious documentation route | A demo visitor cannot readily find build, integration or support information. | Add explicit documentation and getting-started destinations; provide build/embed/contribute paths after the demo. | Every journey reaches the correct guide with one or two clearly labelled actions. |
| Requirements and experimental status live mainly in the footer | The user's first action may fail without prior context; API/support expectations are unclear. | Make browser requirements and experimental scope discoverable beside the demo and adoption path. | Unsupported browser shows an actionable explanation; no unsupported stability or coverage claim. |
| Similar rounded cards dominate the page | Repeated boxes flatten hierarchy and make the composition generic. | Establish a recognisable typographic/grid system and vary section density for explanation, experiment and documentation. | Full-page review, not just a cropped hero screenshot; sections have clear priorities without excess ornament. |
| Large append-only CSS overrides | Multiple definitions obscure which rules govern layout and invite regressions. | Consolidate tokens, components and breakpoints once the design is accepted. | One coherent rule set; no contradictory first-pass dimensions left to override accidentally. |

## Concrete design direction

Treat the page as a small **translation instrument**: rigorous enough for an
engineer, approachable enough for someone seeing a browser JIT for the first
time. Give it an identifiable visual system based on instructions, blocks,
memory lanes and execution, not a generic dashboard aesthetic.

The current content sequence is:

1. **Understand:** project name, one precise proposition, supported guest
   families and a concise explanation. Two obvious routes: adopt the package and
   explore the benchmarks. The compact translation diagram belongs to this
   introduction. Its ARM → optimized IR → simplified Wasm trace is explicitly
   conceptual, not live telemetry.
2. **Use the package:** installation and complete Node / Worker examples are the
   first section after the introduction.
3. **Compare C workloads:** fixed work, three execution paths, checked outputs
   and inline access to the actual shared C source.
4. **Compare QuickJS workloads:** one shared explanation of the nested engine,
   with a suite selector for upstream V8 v7 cases and the custom LCG / sieve / π
   benchmark. Each suite exposes its JavaScript source inline. Keep active runs,
   cancellation and results visible when switching between suites.
5. **Understand the architecture:** the stage explorer, embedding guide, support
   limitations and license follow the runnable examples. The former ARM
   playground lives only in a separate host regression fixture.

Prioritize a single excellent demonstration over multiplying decorative effects.
Motion should explain a state change, indicate real work or acknowledge an
action. Decorative movement must stop for reduced-motion users, and essential
meaning must not depend on animation, hover or color alone.

The presentation controls follow the same restraint:

- The header cycles **System → Light → Dark**. System is the default and removes
  the saved preference; an explicit choice is the only presentation state stored
  in `localStorage`. The page still works when storage is unavailable.
- The conceptual pipeline can be paused and resumed. It also stops offscreen and
  in hidden tabs. Reduced-motion users receive the complete static illustration
  and no irrelevant motion toggle.
- Active-section navigation uses `aria-current="location"`; it supplements, but
  never replaces, ordinary anchor navigation.
- During long C and QuickJS runs, their native control rows remain sticky below
  the header when the viewport has enough height, so Stop stays reachable. Short
  viewports use ordinary document flow instead of trapping a tall toolbar below
  the header. The rows return to normal flow when idle.
- The keyboard skip link stacks above the sticky header. Automated browser
  checks hit-test both it and running Stop controls, rather than checking only
  their focused or enabled DOM state.
- Green is reserved for correctness. Runtime comparison bars use foreground and
  neutral colors so a faster or preferred path is not confused with a passing
  checksum.

## Local acceptance evidence

These are project acceptance gates derived from the research. They are not
numerical substitutes for the awards' jury decisions.

| Area | Evidence required before calling the page release-ready |
| --- | --- |
| Visual craft | Actual rendered screenshots at 320/390 px mobile, 768 px tablet, 1280×800 and 1440×900 desktop; dark/light; full-page composition and focused interaction states. No clipping, accidental title wraps, overlapping labels or broken visual hierarchy. |
| First action | A visible, keyboard-reachable route to the working demo and to documentation in the initial desktop viewport. Mobile text and controls remain readable without horizontal page scrolling. |
| Truthful presentation | No fabricated live states, benchmark figures, compatibility claims, star counts, testimonials, award badges or package-install instructions. Claims trace to source or reproducible execution. |
| Interaction | Successful runs in all three memory modes, Stop while fetching/executing, rerun after cancellation, guest mismatch/failure, missing assets and unsupported features. Output, badge and page status must agree. |
| Accessibility | Semantic heading/landmark structure, descriptive link labels, keyboard-only operation, clearly visible focus, text alternatives, adequate contrast, 200% zoom and reduced-motion behavior. Automated checks supplement manual inspection. |
| Responsiveness | Use actual browser device emulation or confirmed viewport dimensions. Chrome command-line window screenshots alone may enforce a larger minimum layout width and can misrepresent a 390 px test. |
| Performance | Measure the initial landing page separately from the explicitly requested Wasm execution. Do not preload the Wasm runtime merely for decoration. Record local Lighthouse/lab context rather than claiming field Core Web Vitals from a local test. |
| Resource quality | Links resolve to intended guides, static assets work under the repository subpath, metadata/share artwork/favicon match the design, and 404 recovery offers useful navigation. |
| Maintainability | The static page remains buildable and auditable; source assets have provenance; CSS/JS describe the accepted implementation rather than accumulating abandoned visual experiments. |

Relevant technical references: [WCAG 2.2 quick reference](https://www.w3.org/WAI/WCAG22/quickref/)
and [web.dev Core Web Vitals](https://web.dev/articles/vitals). Core Web Vitals
uses field thresholds at the 75th percentile; a local Lighthouse score cannot
prove those field thresholds. Accessibility also requires human review beyond
an automated pass.

## Verified local release candidate — 2026-11-02

The following evidence comes from the local source/build and real browser
execution. It does not include a deployment, remote device lab, field telemetry
or an independent design/accessibility audit.

| Check | Local result |
| --- | --- |
| Presentation and guest interactions | **33 Chromium checks passed**, including real ARM execution/cancellation, theme persistence, blocked storage, source fallback, focused skip-link hit-testing, 320–1440px layouts, and running Stop geometry at 640×900, 640×320 and 640×300. |
| C benchmarks | **8 Chromium checks passed**. Both Quick and Full profiles completed 45 observations each across five kernels and three execution paths; every checksum matched. Firefox C smoke passed. |
| Custom QuickJS | **8 Chromium checks passed**, covering all three actual runtimes, checked statistics, export, cancellation and recovery. Firefox smoke passed. |
| Official QuickJS V8 v7 | **6 Chromium checks passed**; all 30 case/runtime measurements completed with the original timing windows. The instrumented Firefox rerun also completed all 30 successfully. An earlier Firefox smoke reported one measurement failure but retained no per-case report; that failure was not reproduced in the instrumented rerun. |
| Package adoption path | Every unchanged published npm/Node/Vite snippet passed against the local tarball. Node executed ARM32, Chromium executed the AArch64 Worker from a nested base, and Vite loaded the package Wasm asset. |
| Unit and host regression | 56 Node tests and 24 Python tests passed. Node ARM32/AArch64 smoke and browser-host normal/fault, RAM-boundary and API-validation checks passed. |
| Automated accessibility | axe-core reported **zero violations** in 1440px Light, 1440px Dark, 390px Light and 320px Dark configurations. This does not replace manual review. |
| Manual contrast review | Representative text/control combinations calculated from the actual tokens exceeded 4.5:1: Light muted/background 5.77:1, Light accent/background 6.14:1, white/accent button 6.67:1, Dark muted/background 9.48:1, Dark accent/background 9.07:1, and code muted/code background 8.27:1. |
| Local Lighthouse lab | Final run: Performance **1.0**, Accessibility **1.0**, Best Practices **1.0**, SEO **1.0**; FCP **1.1s**, LCP **1.1s**, TBT **0ms**, CLS **0**, maximum potential FID **20ms**. The forced-reflow insight passed after active navigation moved to `IntersectionObserver`. |
| Visual/responsive review | Fresh desktop Light/Dark and 390px mobile screenshots were inspected, including the full mobile composition. No document-level horizontal overflow or clipped main content remained at the tested widths. |
| Code review | The initial review found a skip-link/header stacking defect and a short-viewport Stop defect. Both were fixed with failing-first regression coverage; scoped re-review found no remaining CRITICAL, HIGH, MEDIUM or LOW issues. |

These Lighthouse values are a single local lab run, not field Core Web Vitals or
a guarantee for visitors' devices. Firefox and Chromium were exercised locally;
this round did not test Safari or physical mobile devices. No award recognition
is asserted.

## Completion language

The defensible result is a locally verified, publication-ready experience with
documented design rationale and no known release-blocking defects. “Awwwards,
Webby and FWA quality” is a demanding design target. It does not grant permission
to label the project award-winning or to claim independent jury validation.
Keep the design review open whenever the actual page still fails the visual,
content or interaction criteria above, even when implementation tests are green.
