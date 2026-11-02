# Package quick start and browser benchmarks

The Pages site introduces dynarmic.js, then presents **Use the package**,
**C benchmarks**, and one combined **QuickJS benchmarks** section. An
architecture explorer follows the benchmarks. The ARM playground is no longer
part of the public page; its ARM32/AArch64 host checks remain in the test suite.

The header's theme control cycles the color theme between **System**, **Light**
and **Dark**; System follows the OS preference, and an explicit choice is kept
in `localStorage` for later visits. The hero shows a conceptual
ARM-to-WebAssembly pipeline illustration whose motion pauses with a
**Pause motion**/**Play motion** control; that control is hidden and the
animation stops when the user prefers reduced motion.

For a smaller first step without guest cross compilers, start with
[the Node.js example](../node/README.md).

The **Use the package** section explains the experimental npm candidate and its
release tarball installation. It includes complete, copyable ARM32 Node and A64
Vite module Worker examples, TypeScript/BigInt notes, Wasm URL and byte-buffer
loading, cooperative budgets and cleanup. The C++ source-build path remains
available in its own disclosure. Download a package from
[GitHub Releases](https://github.com/xndcn/dynarmic.js/releases), or follow
[the package build procedure](../../docs/Releasing.md#npm-package) when no release
is available. Installing the tarball does not require a registry account.

The page also offers a [QuickJS example](../quickjs/README.md). It executes the
same JavaScript LCG, prime sieve and π workloads in three Workers: ARM32 QuickJS
through dynarmic.js, QuickJS compiled directly to Wasm, and the browser's
JavaScript engine. Each path checks its output and exposes repeated timings.
This is a separate comparison from the C guests above; read its displayed
workload parameters and methodology before interpreting ratios.

**QuickJS benchmarks** defaults to the V8 version 7 benchmark shipped with QuickJS
extras: Richards, DeltaBlue, Crypto (Encrypt and Decrypt), RayTrace, EarleyBoyer
(Earley and Boyer), RegExp, Splay and NavierStokes. All ten cases are available;
select one or **Run all 10**. The **Benchmark suite** selector switches to
the custom LCG / sieve / π comparison in the same area. Each uses a fresh Worker and the upstream harness's
one-second warmup and one-second measurement batches until at least 32 measured
iterations complete. A failed or cancelled run retains every selected case and
its status in the downloadable report.

**C benchmarks** compares five fixed freestanding C kernels — an integer mix,
data-dependent branches, sequential memory, random memory and Float64
arithmetic — through ARM32 on dynarmic.js, x86 on v86, and direct WebAssembly.
All three use the same pinned Zig/Clang compiler and optimization settings
without a guest C library. Choose **Quick** or **Extended** work counts and one
case or all five; each of the three paths runs three fresh-engine rounds of
three measured calls, and the page reports the median and range across rounds
along with v86/ARM and ARM/direct-Wasm ratios. Every call's checksum must
match an independent reference before its timing is accepted, and the raw
samples can be exported as JSON. ISA, emulator behavior and browser Wasm
compilation still differ; this compares the complete execution paths. The
[kernels, build script and host protocol](../c-bench/README.md) are available
as a standalone example.

## Build and run

For a prebuilt site, download `dynarmic.js-<version>-pages.tar.gz` from
[GitHub Releases](https://github.com/xndcn/dynarmic.js/releases), extract it into
an empty directory and run `python3 -m http.server 8000` there. Open
<http://localhost:8000/>. Keep the whole directory together, including matching
source and notices. This path needs no Emscripten or guest compilers.

To build from source:

Follow the [build guide](../../docs/GettingStarted.md) to install Emscripten,
Boost headers, the ARM cross compiler and Python `ziglang`. Both guest toolchains
are needed for the complete page.
The C benchmark builder uses `ziglang==0.17.0` from that Python environment.
The npm tooling requires Node.js 22.12+; CI uses Node 24 LTS.

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-em -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR=/path/to/boost-headers \
  -DDYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON
npm ci
npm run build
npm run pages:preview -- --host 127.0.0.1
```

Open <http://127.0.0.1:4173/dynarmic.js/>. Start with the package examples, then run a
C comparison or select a QuickJS suite and case. **View benchmark code** opens
the actual distributed C kernels, original V8 v7 JavaScript, or custom QuickJS
workloads inline with syntax highlighting. Source loads on demand, can be copied
as plain text or downloaded, and can be retried if its request fails; opening
code does not load an engine. The source viewer and highlighter adapt to the
page's System/Light/Dark theme. If the highlighter cannot load, the original
source remains readable and copyable.

Prism 1.30.0 is served locally and loaded only when a source viewer opens.
The viewer explicitly loads its C or JavaScript grammar, then highlights the
fetched source in the browser. There is no build-time highlighting, language
auto-detection, CDN dependency or extra npm installation. The original download
and clipboard contents remain unchanged.

Use `?cbench=quick` to run the C comparison on load, `?official=all` for the
upstream suite, or `?quickjs=1` for the custom comparison. For a view-only link,
`?quickjs-suite=custom` selects the custom panel without running it. The suite
selector stays locked while its benchmark runs, so Stop and progress remain
visible. Switching suites preserves completed results and exports.

### GitHub Pages preview

The **GitHub Pages** workflow builds this same `browser_page` output with a pinned
Emscripten toolchain, packages the matching recursive source, and checks the page
in Chromium and Firefox. Pushes and pull requests only validate. Deployment
requires a manual workflow run with `deploy` explicitly enabled. Set
`release_tag` to deploy an already published version without compiling or
downloading build toolchains; leave it blank to build the selected ref. See the
[release guide](../../docs/Releasing.md#github-pages).

To reproduce the artifact locally, use the commands above. The source-packaging
step includes the actual working tree for local review and labels dirty snapshots
as unverified. Publication requires a clean build; see [SOURCE.md](SOURCE.md).
The page has no CDN, external fonts, tracking, or runtime npm dependencies. The
Wasm engine downloads only after selecting Run. The architecture explorer and
copy buttons enhance static content without changing guest execution.
The build renders `index.html` with content hashes on its UI script and
stylesheet URLs. This keeps returning visitors from combining a new layout
with cached scripts that still expect controls removed by an earlier version.

The canonical URL, social metadata and 404 recovery paths target
`https://xndcn.github.io/dynarmic.js/`. To test 404 behavior locally, mount the
distribution at `/dynarmic.js/` and serve `404.html` with HTTP 404 for unknown
paths, including nested paths. The Vite preview above mounts this path and serves
the custom 404 with HTTP 404, including its working stylesheet and recovery
link. A root-only Python server previews the main page but does not simulate
Pages' custom 404 routing. Update the metadata, 404 links and Vite base for a fork.

The root `package.json` is private tooling for this review path. `npm run build`
builds `browser_page` in the configured `build-em` directory, packages source,
then checks the finished artifact. Pass `-- --build-dir build-pages` for another
configured build. CMake remains responsible for the Emscripten and benchmark
outputs; Vite serves them unchanged.

To check an existing artifact, run `npm run pages:check -- --dist <directory> --strict`.
It checks real HTML references, manifest icons and scope, UI content hashes,
source/notices integrity and every recorded benchmark artifact's hash and size.
This accepts labeled local snapshots; publication still requires the separate
`--require-clean` source-packaging gate. `npm test` exercises damaged artifacts
and the preview's routing. Set `DYNARMIC_JS_PAGES_DIST` to preview an artifact
from a different directory.

Each benchmark has its own **Stop** control and deadline. Cancellation ends its
Worker; the next run starts with fresh engines. Only one benchmark runs at a
time. The browser test drivers serve `tests/browser/fixtures/arm.html` separately
to retain ARM32/AArch64, memory-mode, cancellation and recovery coverage. This
fixture is not deployed as a Pages asset.

Use a browser with WebAssembly tail calls and SIMD. Chromium and Firefox are
exercised by CI. Serve the entire output directory over HTTP(S), keeping
`arm_jit.js` and `arm_jit.wasm` from the same build together. See
[browser deployment](../../docs/Embedding.md#browser-deployment) for Worker and
Content Security Policy requirements. The generated site needs no runtime npm
dependencies, bundler, CDN, cross-origin isolation headers or external fonts.
The C lab verifies engine hashes before executing their bytes. A custom CSP
must allow its same-origin Workers, WebAssembly compilation and `blob:` script
imports used for those verified engine scripts.

The `browser_page` target builds the host and every page asset. The strict
configure option above reports missing guest tools before building. Leave it
`OFF` when building only `arm_jit` for the Node example.
The first full page build downloads checksum-pinned QuickJS sources, then builds
both interpreter targets. Its verified upstream archive is cached under the
build tree and copied to `dist/` with the QuickJS license and build manifest.
The official suite additionally downloads the checksum-pinned QuickJS extras
archive and retains its original JavaScript, licenses and source. Its separate
`arm_bench_jit` host provides 256 MiB guest RAM with a 192 MiB QuickJS allocation
limit, matching the direct Wasm interpreter's limit. The small example continues
to use its 16 MiB guest host. Engines download only when their lab runs.
The `c_bench_example` target builds the kernels for all three targets and downloads the pinned
v86 runtime into the build tree's `c-bench-cache`. The v86 runtime and license
are served from the same distribution as the page. To rebuild only these
artifacts without the rest of the page, run the
[standalone example's build script](../c-bench/README.md); it writes to its own
output and leaves the page distribution unchanged. Source packaging verifies
every C benchmark artifact and its recorded local source hashes before writing
the `cBenchBuild` section of `source-manifest.json`.

## Files

| File | Role |
| --- | --- |
| `arm_web.cpp` | Demo C API, 16 MiB guest RAM, A32/A64 callbacks and semihosting |
| `worker.js` | Loads the module and guest binary, runs the JIT and reports output |
| `index.html`, `styles.css` | Responsive page, theme toggle, conceptual-pipeline motion control, controls and accessible progress/results |
| `site.webmanifest`, `robots.txt`, `sitemap.xml`, `404.html`, `icon*`, `social-card.png` | Pages metadata, install icon, share card and fallback page |
| `app.js` | Standalone ARM host regression UI used by the test fixture |
| `site.js` | Theme cycling, suite selection, inline source viewers, pipeline motion, architecture explorer and command copying |
| `render_page.cmake` | Adds content hashes to the built page's script and stylesheet URLs |
| `vendor/prism-1.30.0/` | Pinned Prism core, C/JavaScript grammars and MIT license, loaded on demand |
| `quickjs-app.js`, `quickjs-worker.js` | Three-runtime comparison, independent cancellation, checked results and timing reports |
| `official-benchmarks-app.js`, `official-benchmarks-worker.js` | Complete V8 v7 case selection, per-runtime deadlines, partial reports and upstream measurements |
| `c-bench-app.js`, `c-bench-worker.js`, `c-bench-protocol.js` | Three execution paths for the same C kernels, shared validation and report protocol |
| `../c-bench/` | Freestanding kernels, independent references and matching ARM32/x86/Wasm builds |
| `package_source.py` | Recursive source archive, runtime correspondence and license distribution |
| `guest/` | LCG, prime sieve and FP64 kernels plus architecture-specific startup/linker files |
| `CMakeLists.txt` | Builds the host and copies the page, guests and reference output into `dist/` |
| `write_expected.cmake` | Captures the independent native reference output, failing if it cannot run |
| `verify_browser.mjs` | Headless browser smoke driver using an HTTP result callback |
| `fault_test.mjs` | Node regression for host faults and recovery |
| `bench.mjs` | Node-hosted workload measurements and JSON reporting |

The generated directory includes `demo.bin`, `demo_a64.bin`, `expected.txt`,
`arm_jit.js`, `arm_jit.wasm`, `index.html`, `styles.css`, `app.js`, `worker.js`,
site metadata, icons, share card and a build manifest.
The QuickJS comparison adds `quickjs-arm.bin`, `quickjs-wasm.js`,
`quickjs-wasm.wasm`, `quickjs-workloads.js`, `quickjs-manifest.json`,
`quickjs-source.tar.xz`, `quickjs-LICENSE.txt` and `quickjs-runtime-licenses.txt`.
The official section adds `arm_bench_jit.js`, `arm_bench_jit.wasm`,
`arm-bench-build-manifest.json`, `official-benchmarks.json`, `official-v8.js`,
`official-v8-upstream.js`, `official-v8-adapter.js`, `quickjs-extras-source.tar.xz`
and DeltaBlue's GPL 2.0 text in `quickjs-extras-LICENSE.txt`.
The C section adds `c-bench-arm.bin`, `c-bench-x86.elf`, `c-bench.wasm`,
`c-bench-manifest.json`, `c-bench-reference.json`, `c-bench-source.c`,
`c-bench-v86.js`, `c-bench-v86.wasm` and `c-bench-v86-LICENSE.txt`.
If a guest file is missing, inspect the configure warnings, install the missing
compiler and rerun CMake.

## Worker protocol

Send one request at a time. This small one-shot consumer handles Worker failures
and always releases the Worker:

```js
const response = await fetch('demo.bin', {signal: AbortSignal.timeout(30_000)});
if (!response.ok) throw new Error(`guest fetch failed: ${response.status}`);
const bin = await response.arrayBuffer();
const worker = new Worker('worker.js');
let timeout;
try {
  const result = await new Promise((resolve, reject) => {
    timeout = setTimeout(() => reject(new Error('guest timed out')), 120_000);
    worker.onmessage = ({data}) => resolve(data);
    worker.onerror = () => reject(new Error('Worker failed to load or execute'));
    worker.onmessageerror = () => reject(new Error('Worker reply could not be read'));
    worker.postMessage({bin, arch: 0, mode: 2}, [bin]);
  });
  if (!result.ok) throw new Error(result.error);
  console.log(result.text);
} finally {
  clearTimeout(timeout);
  worker.terminate();
}
```

`arch` is `0` for ARM32 and `1` for AArch64. `mode` is `0` for callbacks, `1`
for page-table memory or `2` for a flat window. The worker loads a flat binary
at guest address `0x10000`, creates a fresh JIT and starts execution there.
Each request clears guest RAM and the console; the Emscripten module is reused.
The binary must be a nonempty `ArrayBuffer` and fit between `0x10000` and the
end of the 16 MiB RAM allocation. Invalid input is rejected before copying it.
The transferred buffer is detached in the sender. Keep a copy first if you need
to reuse it. A second request during module loading receives a busy error; wait
for the current reply before sending the next guest. The worker checks required
WebAssembly tail-call and SIMD support before instantiating the module.

Success replies contain `ok: true`, `arch`, console `text`, `haltReason`,
`runMs`, per-kernel `kernelMs`, initial module `loadMs` and compile `stats`.
Failures contain `ok: false` and `error`. A normal semihost exit uses halt reason
`0x01000000`; unhandled guest exceptions/fallback use `0x02000000`.
The page discards a Worker after any failed reply, since a runtime abort can
leave its Wasm module unusable. The next guest or retry creates a fresh Worker.

A run is synchronous within the Worker. It cannot process a cancellation message
while guest code is executing, so cancellation calls `worker.terminate()` on the
page. The example's guests terminate by semihosting; arbitrary programs need
cycle budgets or a host-level Worker termination policy. The demo is a starting
point for a host, not a machine or operating-system emulator.

## Demo C API and semihosting

`arm_init(mode, arch)` creates a fresh JIT. `arm_ram()` and `arm_ram_size()`
expose the guest RAM allocation; `arm_run(pc)` runs from a guest address.
Read output through `arm_console()` and `arm_console_len()`, and clear it with
`arm_console_clear()`. The startup assembly initializes the guest stack.

`arm_init()` returns `-1` for an invalid mode or architecture. `arm_run()` returns
the fault halt reason if no JIT is initialized or the entry PC is outside RAM
or not four-byte aligned. Guest memory accesses must fit entirely within RAM;
invalid accesses halt the guest without wrapping to another address.
`arm_reg(index)` returns the low 32 bits of an A32/A64 register and returns zero
for an invalid index.

| SVC immediate | Demo behavior |
| --- | --- |
| `0` | Exit the guest |
| `1` | Print the low byte of R0/X0 |
| `2` | Print the low 32 bits of R0/X0 as eight hexadecimal digits and a newline |
| `3` | Record a timestamp for per-kernel measurement |
| `4` | Return host time in microseconds, wrapping to 32 bits, in R0/X0 |

Unknown SVC values also exit. Replace these callbacks when embedding a different
guest environment. The general memory and invalidation contracts are documented
in the [embedding guide](../../docs/Embedding.md).

## Verification and measurements

```sh
node --test examples/browser/*.test.mjs
node examples/node/hello.mjs build-em/examples/browser/dist
node examples/browser/fault_test.mjs build-em/examples/browser
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist
node tests/browser/verify_interactions.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist
node tests/browser/verify_quickjs.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist --report /tmp/quickjs-browser-report.json
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist --quickjs
node tests/browser/verify_official.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist --report /tmp/official-browser-report.json
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist --official
node tests/browser/verify_c_bench.mjs /usr/bin/google-chrome \
  build-em/examples/browser/dist --report /tmp/c-bench-browser-report.json
node examples/browser/verify_browser.mjs /usr/bin/firefox \
  build-em/examples/browser/dist --cbench
python3 -m unittest discover -s tests/browser -p '*_test.py'
node examples/browser/bench.mjs build-em/examples/browser 1 --json browser.json
node tests/bench/browser_validate.mjs browser.json
```

The interaction check starts and cleans up its own Chromium instance. On the
page it verifies System/Light/Dark theme cycling and persistence, the pipeline
pause/resume control and its reduced-motion behavior, keyboard focus visibility,
and skip-link/running-Stop hit-testing; on the separate ARM fixture it covers
all memory modes, terminating an infinite guest loop, retry, missing assets,
nested-path hosting, keyboard entry, architecture stages, command copying,
clipboard denial and 320–1440px layouts. Short 640×300/320 viewports verify
that cancellation remains reachable without sticky positioning. Use
`--report output.json` to retain results and the tested asset hashes in a new
file.
The QuickJS check separately exercises the three actual runtimes, report data,
narrow layouts, cancellation, missing-asset failure and a successful retry.
The official check requires all ten cases to complete on all three runtimes,
checks each original measurement window, downloads the full report, and covers
cancellation, complete inventory recovery and narrow layouts with expanded
statistics. The Firefox official smoke runs the same full case list.
The C benchmark check executes all three compiled paths; the source-package
tests separately reject missing artifacts, stale source hashes and missing
license notices.

The page runs in an actual browser Worker. `bench.mjs` runs the browser host's
compiled module under Node and records that engine; its numbers are not Firefox
or Safari measurements. Timing observations include compilation and engine
behavior described in the [performance protocol](../../docs/Performance.md).

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Configure reports missing tools | Install the named compiler or Python module, activate its environment, and rerun the same configure command. |
| A file cannot load / HTTP 404 | Serve `build-em/examples/browser/dist`, not the source directory; rebuild `browser_page` and keep all generated assets together. |
| Worker cannot start | Use HTTP(S) instead of `file://`; check `worker-src` and `script-src` in the site's Content Security Policy. |
| Tail-call or SIMD support is missing | Update the browser and ensure WebAssembly is enabled. Current Chromium and Firefox are exercised by CI. |
| A run times out | Retry on an idle machine; use Stop for a guest that does not exit. The page's limit is a responsiveness guard, not an instruction budget. |
| Output differs from the reference | Record the architecture, memory mode, browser version and exact build revision; include both outputs in a [bug report](https://github.com/xndcn/dynarmic.js/issues). |

When contributing an example, follow the [maintenance guide](../MAINTAINING.md)
for the browser and build checks and the references behind these conventions.

The page's visual rationale and the primary-source award/project research are in
[PagesDesign.md](../../docs/PagesDesign.md). `icon.svg` and `social-card.svg` are
project-owned vector originals; PNG derivatives are rasterized from these files.
The latter uses DejaVu Sans and DejaVu Sans Mono for reproducible text rendering.
