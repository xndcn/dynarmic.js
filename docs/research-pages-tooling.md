# Pages tooling research and implementation decision

Reviewed **2026-11-02** against the official documentation and GitHub sources
below. This supplements [PagesDesign.md](PagesDesign.md) and
[research-release-quality.md](research-release-quality.md). It records the
reason for the tooling choices; browser reports remain the authority for
runtime and visual verification.

## Official guidance

| Source inspected | Guidance | Decision for this repository |
| --- | --- | --- |
| [Vite: deploying a static site](https://vite.dev/guide/static-deploy) | Install Vite locally, build a deployable directory, and use `vite preview` to inspect that directory locally. Preview is not a production server. The Pages example uses `npm ci` and the official Pages artifact/deployment actions. | Add a private root npm workspace and a locked Vite dependency for local preview. CMake continues to produce the complete deployable directory. |
| [Vite: `base`](https://vite.dev/config/shared-options#base) and [public base path](https://vite.dev/guide/build#public-base-path) | A project site at `username.github.io/repository/` needs the matching base when Vite generates URLs. `./` is supported for embedded deployments. | Preserve the generated page's relative asset URLs and preview them under `/dynarmic.js/`, matching the actual project path. A root-only preview does not prove subpath support. |
| [GitHub: custom Pages workflows](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages) | Upload a Pages artifact, make deployment depend on the build, use a deployment environment, and give the deploy job `pages: write` and `id-token: write`. Artifact archives must not contain symbolic or hard links. | Keep the existing validated-artifact pipeline and `github-pages` environment. Build/check jobs keep read-only permissions. Deployment remains an explicit manual action. |
| [GitHub: creating a Pages site](https://docs.github.com/en/pages/getting-started-with-github-pages/creating-a-github-pages-site) | An Actions artifact must have an entry file such as `index.html` at its root. Published Pages content is public. | Check the generated web root and ship the intended site artifacts, source and license notices; do not upload the workspace or build cache. |
| [GitHub: secure use of Actions](https://docs.github.com/en/actions/security-for-github-actions/security-guides/security-hardening-for-github-actions) | Use minimum token permissions. A full commit SHA is the immutable way to reference an action. Avoid checking out untrusted pull-request code in privileged workflows. | Keep ordinary PR validation unprivileged. The Pages workflow pins each existing action version to its resolved commit. Dependency updates are reviewed manually; Dependabot updates are disabled. Other workflows have not been converted as part of this change. |
| [npm: `npm ci`](https://docs.npmjs.com/cli/v11/commands/npm-ci) and [`npm pack`](https://docs.npmjs.com/cli/v11/commands/npm-pack) | A lockfile makes clean installs repeatable; packing and dry runs expose the package's actual file set. | Commit the root lockfile and use `npm ci` locally and in Pages CI. Keep the private tooling package distinct from the publishable `packages/dynarmic` package. |
| [npm: provenance](https://docs.npmjs.com/generating-provenance-statements) | Supported CI publishers can attach verifiable source/build provenance to registry releases. | Consider this when registry publishing is enabled. Local tarball checks do not establish registry provenance, and this work does not publish a package. |

The Vite and GitHub Pages guides were read from their official Markdown
endpoints as well as their linked documentation. The referenced GitHub project
files below were fetched at the recorded commits through the GitHub API.

## What mature project sources demonstrate

These are specific practices to adapt, not evidence that dynarmic.js has the
same audience, adoption, budget or independent recognition.

| Inspected source | Observed implementation | Applicable practice |
| --- | --- | --- |
| [Vite `Hero.vue`](https://github.com/vitejs/vite/blob/b3cb0575aff63004a71301a1ec798aeea2aa05d7/docs/.vitepress/theme/landing/Hero.vue) | A short category statement, adjacent Get Started/GitHub actions, executable package-manager commands, and a bounded illustration column. | Put a working adoption route beside the proposition. Keep examples copyable and verified. Bound the conceptual ARM-to-Wasm illustration so it cannot displace the primary action. |
| [Vite landing `Layout.vue`](https://github.com/vitejs/vite/blob/b3cb0575aff63004a71301a1ec798aeea2aa05d7/docs/.vitepress/theme/landing/Layout.vue) | Separate sections for introduction, features, ecosystem, community and the final getting-started action. | Maintain a coherent journey from explanation to execution, interpretation and documentation. Do not borrow its sponsor or customer claims. |
| [Astro docs configuration](https://github.com/withastro/docs/blob/8d8ef22cb7a2c06ae3a103d97270071a969ea013/astro.config.ts) | Explicit site URL, sitemap integration, edit links, sidebar configuration, locales and separate mobile navigation components. | Give metadata and navigation explicit ownership. Keep getting-started, API, support and contribution paths discoverable. A multilingual docs framework is not required for this single-page demo. |
| [Astro docs `package.json`](https://github.com/withastro/docs/blob/8d8ef22cb7a2c06ae3a103d97270071a969ea013/package.json) | A private tooling package with distinct build, preview, check and generated-site link-check commands. | Provide a small, documented contributor command surface and validate the generated site, not just its source HTML. Our Pages check is narrower than Astro's site-wide link checker. |
| [Astro docs search component](https://github.com/withastro/docs/blob/8d8ef22cb7a2c06ae3a103d97270071a969ea013/src/components/starlight/Search.astro) | Search result origins are removed so links work on localhost and preview hosts. Search controls have accessible labels and responsive presentation. | Treat localhost, preview paths and the production host as real usage contexts. Do not make navigation accidentally depend on the production origin. External search is unnecessary for the current landing page. |

## Why CMake builds and Vite previews

This site includes more than frontend source. The existing CMake graph compiles
Emscripten hosts, ARM/AArch64 guests, QuickJS and the C comparison assets. It
records build manifests, packages matching source and licenses, and renders
the page's UI asset content hashes. Replacing that graph with `vite build`
would require deliberately reproducing those responsibilities.

The current integration therefore has three boundaries:

1. **CMake builds the artifact.** `browser_page` remains the producer of the
   generated `examples/browser/dist` directory. The source/package tooling
   retains ownership of provenance and redistribution notices.
2. **npm exposes contributor commands.** `npm run pages:check -- --dist …`
   checks the generated static contract, and `npm ci` installs the locked
   preview tooling. `npm run build` orchestrates the configured CMake build,
   matching-source packaging and strict artifact check. The root package is
   private. No root `type: module` is set: generated Emscripten `.js` files
   retain their CommonJS behavior in Node.
3. **Vite previews the artifact.** `npm run pages:preview` serves the configured
   generated directory on loopback under `/dynarmic.js/`.
   `DYNARMIC_JS_PAGES_DIST` selects a different
   artifact directory. It does not compile, bundle, rewrite or authenticate the
   Wasm and benchmark assets.

This is an npm/Vite tooling integration, **not a migration of the Pages
frontend to a Vite bundle**. The independently tested npm consumer examples
can use Vite to bundle an application importing `dynarmic.js`; that consumer
path is separate from building this demonstration site.

## Routing, caching and verification boundaries

The landing page uses document sections and anchor navigation, with a real
`404.html` recovery page. The Vite preview uses `appType: 'mpa'` and serves that
recovery document with HTTP 404 for missing paths, avoiding an SPA fallback
that would mask broken links. It has no client-side history router, so no SPA
redirect workaround is required. If routing changes, direct nested navigation
and refresh behavior must be tested against static hosting rather than relying
on a development server's fallback.

`render_page.cmake` binds UI JavaScript and CSS references to their actual
content through SHA-256 query keys. Runtime/build manifests cover the generated
engines. Vite preview serves these files unchanged; it does not add production
cache headers, a service worker, CDN cache invalidation or a second asset
fingerprinting scheme. Browser loading remains lazy for explicitly requested
execution rather than preloading the large engines for decoration.

The static checker complements the existing real-browser tests. It cannot
prove benchmark correctness, cancellation, accessible interaction, visual
quality, performance or upstream link availability. Those require the
appropriate browser reports and rendered inspection described in
[PagesDesign.md](PagesDesign.md). Likewise, successful local preview does not
prove that a remote Pages deployment has succeeded.

The current work is local preparation: no Pages deployment, registry publish,
custom-domain change or claim of independently certified accessibility or
design quality follows from these tooling choices.

## Local verification of this integration — 2026-11-02

The current CMake output was rebuilt before testing; old, unreferenced Vite
experiment chunks were retained outside the Pages artifact. The final local
source archive is a labeled dirty snapshot, not a clean published release.

| Check | Observed result |
| --- | --- |
| Tooling installation and build | Locked `npm ci` and `npm run build` passed; CMake, source packaging and strict artifact verification completed in order. |
| Static checker and preview regression | 11 tests passed, including modified/missing artifacts, malformed cache keys, icon paths, traversal/symlinks, CLI errors, unbuilt preview directories and HTTP 404 behavior. |
| Existing host and packaging regression | 72 Node tests and 26 Python tests passed. The Node host executed ARM32 and AArch64 with the expected value 42. |
| Chromium interactions | 33 page checks, 8 custom QuickJS checks, 8 C comparison checks and 6 official V8 checks passed against the current build. C Quick and Extended each completed 45 observations with matching checksums; official V8 completed all 30 case/runtime measurements. |
| Firefox | ARM host, custom QuickJS and C comparison smoke checks passed. |
| Package adoption | The existing local tarball executed the page's unchanged ARM32 Node snippet and AArch64 Vite Worker snippet under a nested base. This was a consumer check, not a newly published package. |
| Actual Vite preview | Homepage controls, lazy engine loading, Wasm MIME type, nested missing-page status, styled 404 and recovery navigation passed. |
| Visual review | Fresh 1440px light/dark, 390px light and 320px dark viewport/full-page renders were inspected; no document overflow or visibly clipped main content was found. |
| Accessibility | axe-core 4.10.3 reported zero violations in those four combinations. Hero background/decoration contrast items required manual review; worst-case dotted-background text contrast was at least 5.20:1. |
| Local performance | Lighthouse 12.8.2 on the Vite preview reported Performance, Accessibility, Best Practices and SEO all 100; FCP 0.8s, LCP 1.4s, TBT 0ms and CLS 0, with no run warnings. |
| Supply chain and review | All 12 Pages action references are pinned, workflow YAML and manual-only deployment conditions were checked, and final code review found no required fixes. Root npm audit (excluding optional packages) reported zero known advisories. |

Raw reports, screenshots and command/version notes are retained locally in
`test-results/pages-refresh/` (ignored build evidence). The browser versions were
Chrome 133.0.6943.53 and Firefox 157.0.1. Lighthouse is one local lab observation,
not field Core Web Vitals. This round did not test Safari or physical mobile
devices, run a remote workflow, publish a package or deploy Pages. Publication
still uses the documented clean-checkout/rebuild gate; automated accessibility
checks and this local manual review do not constitute independent certification.
