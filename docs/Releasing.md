# Release procedure

Distributions include the source checkout with pinned Dynarmic submodules,
the browser demonstration, and an experimental npm candidate with a typed
JavaScript API. The npm version is `0.1.0-alpha.0`; a registry release is a
separate maintainer action. Release notes must retain the limitations in
[Roadmap.md](Roadmap.md); packaging does not establish stable CPU/API semantics.

## npm package

This path builds only the package runtime. It needs Emscripten (tested with
6.0.13), CMake, Boost headers, Python 3, GNU patch and Node 22+. It needs no ARM
guest compiler, benchmark downloads or Pages build. From a recursive checkout:

```sh
. /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-package -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR=/path/to/boost-headers \
  -DDYNARMIC_JS_TESTS=OFF -DDYNARMIC_JS_EXAMPLES=OFF \
  -DDYNARMIC_JS_NPM_PACKAGE=ON
cmake --build build-package --target dynarmic_package_runtime -j2
python3 packages/dynarmic/pack.py --build-dir build-package --output build-npm/package
npm pack ./build-npm/package --pack-destination ./build-npm --json
node tests/npm/smoke.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz
npm ci --prefix tests/npm --ignore-scripts --no-audit --no-fund
node tests/npm/verify_browser.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz /path/to/chrome
node tests/npm/verify_firefox.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz /path/to/firefox
node tests/npm/verify_tooling.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz /path/to/chrome
```

Review `npm pack --json` and the source manifest. The 14-file allowlist includes
the wrapper and declarations, paired runtime, complete recursive source archive,
rebuild instructions and all runtime notices. `node build-npm/package/verify.mjs`
checks every payload hash; `prepack` runs the same check. It does not compile
anything. Consumers can install with `--ignore-scripts`. The manifest records
the linked runtime's compiler inputs, toolchain and configuration; packaging
rejects modified binaries, stale compiler inputs and non-production flags.

Extract `source.tar.gz` into an empty directory, follow its accompanying
`REBUILD.md`, then execute the rebuilt runtime with the archived wrapper and
the same consumer cases. Git metadata is not needed to rebuild. Runtime bytes
need not be identical across paths/toolchains; run the behavior checks. Retain
the tarball SHA-256, pack inventory and Node/browser/tooling/source-rebuild
reports. The [package workflow](../.github/workflows/package.yml) performs these
checks and uploads artifacts only; it has no publishing credentials or write
permissions.

Local packaging accepts uncommitted work and labels it as a local snapshot.
For an actual release, first finalize the version, changelog and release commit,
relink from that **clean recursive checkout**, then run the staging command
with `--require-clean`. Verify the staged package using
`node build-npm/package/verify.mjs --require-clean` and repeat the consumer gates
on the resulting tarball. The publication guard rejects a runtime previously
linked from a dirty checkout even if that checkout has since been committed.
No files are automatically committed or tagged by this process.

An additional registry metadata rehearsal is local and does not upload:

```sh
npm publish ./build-npm/dynarmic.js-0.1.0-alpha.0.tgz --dry-run --ignore-scripts --tag next
```

This tarball dry run checks npm's publication metadata only; it does not replace
the explicit clean-source and runtime gates. Do not use `--ignore-scripts` as a
way to bypass release verification. The candidate defaults to `next`, avoiding
an experimental version becoming `latest`.

Actual publication is outside local preparation. Before authorizing it, verify
name ownership and public repository/package settings. Prefer npm trusted
publishing with OIDC and provenance using a separately scoped, protected job,
as described in the [packaging research](research-packaging.md). Configure the
registry trust relationship separately; local checks cannot prove it exists.
Archive the exact tested `.tgz`, complete source and reports for each version.
Never rebuild a different artifact between verification and publication.

## GitHub Releases

GitHub Releases is the default download channel for prebuilt packages and the
complete example site. Public release assets can be downloaded without registry
authentication. [GitHub Packages' npm registry](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-npm-registry)
requires scoped package names and authentication, including to install packages;
switching to it would change the current `dynarmic.js` import and add setup for
users. npmjs.com trusted publishing remains an optional future registry channel.

The [release workflow](../.github/workflows/release.yml) starts on a pushed
`v*` tag. The tag must exactly equal `v` plus the version in
`packages/dynarmic/package.json`. Finalize the version, page examples and changelog
on a clean commit, run the release gates below, then push that version tag.
No tag or release is created by the local packaging commands.

The workflow reuses the npm and Pages acceptance workflows at the tagged commit,
including Node 22/24, Chromium, Firefox, Vite/TypeScript and source rebuild checks.
It preserves the exact tested tarball and site, then creates a **draft release**
with these assets:

| Asset | Use |
| --- | --- |
| `dynarmic.js-<version>.tgz` | Install with `npm install ./dynarmic.js-<version>.tgz`; includes runtime, types, matching recursive source and notices. |
| `dynarmic.js-<version>-pages.tar.gz` | Extract into an empty directory and serve with `python3 -m http.server 8000`; includes all demo engines and their source/licenses. |
| `release.json` | Records version, tag, commit, validation run and asset SHA-256 hashes/sizes. |
| `SHA256SUMS` | Verify all downloaded assets with `sha256sum -c SHA256SUMS` in the download directory. |

The assembler rejects changed binaries, dirty builds, mismatched versions or
different recursive source revisions. Prerelease versions produce prerelease
drafts. Review the assets, release notes and linked reports, then publish the
draft in GitHub. Until that happens, the downloads are not publicly available.
Existing releases are never overwritten; if a draft upload failed, inspect and
remove that incomplete draft before rerunning the job. Reports remain in the
linked Actions run under its artifact retention policy; download them if longer
retention is needed.

Use the Releases list or an exact version URL, especially for alpha versions:
GitHub's `releases/latest` does not select prereleases. The automatic GitHub
source ZIP/tar files omit submodules; use the corresponding source bundled in
our assets for rebuilding. Browser engines are served from the extracted Pages
directory, so running examples does not rely on cross-origin Release downloads.

## Prepare and validate

Use a clean recursive checkout of the candidate commit. Record the commit and
submodule revision, compiler, Emscripten, Node and browser versions, and build
options alongside the results. Do not change or rebuild binaries while tests
or performance collectors are using them.

1. Follow [GettingStarted.md](GettingStarted.md) to build native and Emscripten
   Release trees. Run `ctest --test-dir build-native --output-on-failure` and
   `tests/install/smoke.sh build-native /tmp/dynarmic-js-install`.
2. Run `tests/regress.sh --strict build-em build-native`. This requires the
   guest toolchains, Unicorn/Capstone and browser host artifacts. It executes
   the browser host under Node; real browsers are a separate gate below. Set
   `DYNARMIC_JS_TEST_ARTIFACTS` to a new directory to retain commands, reports,
   guest output and fuzz reproducers from the release run.
3. Run the native ASan/UBSan build described in [CONTRIBUTING.md](../CONTRIBUTING.md).
4. Run `node --test examples/browser/*.test.mjs`, the browser host fault tests,
   and Chromium and Firefox Worker checks from the Browser Worker workflow.
   Test both `DYNARMIC_JS_PROFILE=OFF` and `ON` builds, enabling
   `DYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON` so missing demo inputs fail configure.
   Run `node examples/node/hello.mjs build-em/examples/browser/dist` and verify
   both architecture results. Exercise all three browser memory modes, Stop and
   retry, and nested-path hosting using the browser workflow's interaction check.
   Review the page at desktop and narrow widths; see the
   [example acceptance criteria](../examples/MAINTAINING.md).
   Run the QuickJS comparison in both browser engines using
   `node tests/browser/verify_quickjs.mjs /path/to/chrome build-em/examples/browser/dist`
   and `node examples/browser/verify_browser.mjs /path/to/firefox build-em/examples/browser/dist --quickjs`.
   All three runtimes must agree on the LCG, sieve and π outputs; retain the
   exported timing samples and environment with the local review evidence.
   Run the complete official JavaScript suite using `tests/browser/verify_official.mjs`
   in Chromium and `examples/browser/verify_browser.mjs --official` in Firefox
   (same executable and dist arguments as above). All ten cases must complete
   on all three runtimes; partial reports do not satisfy this gate.
5. Verify the external CMake embedding from the parent project scope:

   ```sh
   emcmake cmake -S tests/embedding -B build-embedding \
     -DDYNARMIC_JS_SOURCE_DIR="$PWD" -DCMAKE_BUILD_TYPE=Release \
     -DBoost_INCLUDE_DIR=/path/to/boost-headers
   cmake --build build-embedding -j2
   node build-embedding/embedding_smoke.js
   ```
6. Build and run the [native Dynarmic comparison](Performance.md#native-dynarmic-comparison)
   with at least five fresh process pairs on an idle host. Retain the validated
   raw report, commands and environment with the release evidence. Also collect
   the fixed, cache and browser benchmark reports described in the performance
   protocol. CI smoke observations verify the runner but do not qualify a
   performance claim.

The public CI workflows run unit, sanitizer, strict regression, embedding and
browser gates, plus a native Dynarmic comparison correctness job. The separate
Internal evidence workflow verifies historical experiments. Its results do not
replace current-source tests.

## GitHub Pages

The Pages workflow builds and validates `browser_page` for pull requests and
changes on `main`. These runs only upload an artifact; they do not deploy or
require Pages to be enabled. A manual workflow run also defaults to
`deploy: false`. Deployment runs only with `deploy: true`.

For a released version, set `release_tag` (for example `v0.1.0-alpha.0`). The
workflow downloads that published release's site and `release.json`, checks the
repository, tag, resolved commit and archive checksum, then deploys those exact
files. It accepts published prereleases too. This path installs no compiler and
does not rebuild or rerun benchmarks. A different published `release_tag` rolls
back the site using the same verification. Tag pushes create a release draft
only; they do not deploy Pages.

Leave `release_tag` blank to build the selected ref, package its complete source,
and run Chromium and Firefox checks before deployment. Only the deployment job
uses Pages write and OIDC permissions; build jobs retain read-only permissions.

Before publishing, review the generated page locally at desktop and narrow
widths and check the manifest, icons, share card and 404 page. The default
canonical URL and 404 asset paths target `https://xndcn.github.io/dynarmic.js/`;
update those when using a fork or custom domain. For the first deployment,
select **GitHub Actions** as the Pages source in repository settings. The
deployment job uses the `github-pages` environment; it does not change
repository visibility.

After `browser_page` and source packaging complete, use the private npm
workspace (Node 22.12+ or Node 24 LTS) to check and preview the same artifact as CI:

```sh
npm ci
npm run pages:check -- --dist build-em/examples/browser/dist --strict
npm run pages:preview -- --host 127.0.0.1
```

Vite serves the generated directory without rebuilding or mutating its Wasm,
benchmark or source archive at <http://127.0.0.1:4173/dynarmic.js/>, with custom
404 routing. The check fails on missing local references, stale content hashes,
invalid manifest icons/scope or changed source, notices and benchmark artifacts.
It accepts local dirty snapshots; the publication cleanliness gate below still
applies. `npm run build` combines the CMake build, local source packaging and
strict artifact check after initial CMake configuration.
`python3 -m http.server` remains a dependency-free option for a basic preview.

### Matching source in the Pages artifact

Prebuilt JavaScript and Wasm are distributed together with their corresponding
source and notices in the same `dist/` directory. After building `browser_page`,
prepare the local review bundle with:

```sh
python3 examples/browser/package_source.py --dist build-em/examples/browser/dist
```

The bundle contains `source.tar.gz`, `source-manifest.json`, `LICENSE.txt`,
`THIRD_PARTY_NOTICES.md` and `runtime-licenses.txt`. The archive includes the project's build scripts and
documentation plus the pinned upstream source and recursive submodules, with
their own license notices. It omits Git metadata and generated build directories.
Historical reports in `internals/measurements/` and `internals/release-checks/`
are excluded by the same rule used for npm; they are not build inputs.
The manifest records those exclusions, source revisions, whether the working tree is dirty,
and hashes that bind the archive to the browser build artifacts. A local review
may package uncommitted work; its manifest identifies that state and does not
claim verified correspondence to a clean runtime build. The archive extracts
under `dynarmic.js/`.

The complete page also ships `quickjs-source.tar.xz`, `quickjs-LICENSE.txt`,
`quickjs-runtime-licenses.txt` and `quickjs-manifest.json`. Keep these with both QuickJS runtime builds. Source
packaging checks the QuickJS runtime files, upstream archive and local build
inputs against their recorded hashes and records them in `source-manifest.json`.
When rebuilding an extracted release bundle, copy the supplied
`quickjs-source.tar.xz` into `build-em/examples/browser/quickjs-cache/` before
building. The builder rechecks its checksum and uses it without a download.
The generated source manifest includes these commands.
Also retain `quickjs-extras-source.tar.xz`, the complete upstream source for the
official V8 suite, and `quickjs-extras-LICENSE.txt`, the complete GPL 2.0 text
for its GPL-2.0-or-later DeltaBlue component. The packager verifies the source
against the canonical GPL 2.0 SHA-256 and requires the distributed copy to match
`examples/quickjs/licenses/GPL-2.0.txt`, checks the distributed original
JavaScript against the archive, verifies that all ten upstream cases are in the
inventory, and binds the separate large-memory host through
`arm-bench-build-manifest.json`.

The workflow calls the same packager with `--require-clean` before uploading a
Pages artifact. This rejects uncommitted source and mismatched build provenance.
It also passes the exact Emscripten SDK selected by the workflow so the runtime
license bundle is tied to the SDK that produced the Wasm.
For publication, inspect the generated source manifest, extract the archive to
an empty directory and rebuild it using the included instructions. Keep the
archive, manifest, notices and matching JavaScript/Wasm together; a moving
`main` link or GitHub's automatic source archive, which omits submodules, is not
a replacement for the corresponding source bundle.

The Pages artifact gate runs Chromium interaction checks against the exact
built files, covering all memory modes, Stop and retry, nested-path hosting,
keyboard entry and a narrow layout. It also runs the page in Firefox. The
Chromium report retains the tested asset hashes as a separate workflow
artifact. These checks upload no website and change no public state; selecting
`deploy: true` is the explicit publication action.

The build uploads an intermediate `pages-build` artifact for three independent
Node, Chromium and Firefox validation jobs. All three must pass before
`publish-artifact` promotes that same build to the Pages or release artifact.
Reports are retained by suite and run attempt, including each Firefox case's
result and the runner CPU. A failed-job retry reuses the existing build and
successful validation jobs; the intermediate artifact alone is not publishable.

## Package and publish

- Update [CHANGELOG.md](../CHANGELOG.md), supported toolchain versions and known
  limitations. Link the exact workflow results for the candidate commit.
- Keep [LICENSE](../LICENSE), [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)
  and all bundled dependency licenses with the source. A plain GitHub source
  archive omits submodule contents; either document a recursive clone of the
  release tag or supply a source archive containing the pinned submodule.
- Rebuild any downloadable source archive after extracting it to an empty
  directory. A source archive without Git metadata cannot establish benchmark
  revision provenance; collect comparison reports from a Git checkout.
- If distributing a prebuilt browser example, include the matching complete
  source, build instructions, notices and recorded build configuration. Host
  JavaScript and Wasm must come from the same build.
- Publish performance claims only with compatible raw reports and the
  methodology in [Performance.md](Performance.md). CoreMark requires its own
  correctness and duration qualification; synthetic kernels do not establish
  application performance.
- Create the tag and release only after the checks for that exact commit pass.
  Publishing is a maintainer action; the local preparation steps do not create
  a tag, upload binaries, or change repository visibility.
