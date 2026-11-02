# Maintaining the examples

Examples are executable integration documentation. A new user should be able to
run a documented command, recognize success, and find the small piece of code
that implements the behavior they want to reuse. Keep the source readable and
the first run independent of npm packages, a bundler, or third-party CDNs.

## Acceptance criteria

- **Reproducible first run.** State prerequisites, commands from the repository
  root, generated artifact paths and expected output. Rebuild from the documented
  inputs. Both browser guests must match the reference compiled from the same C
  source; a timing or a successful module load alone is not a passing result.
- **Useful progression.** Keep the smallest integration easy to locate alongside
  the complete browser demonstration. Explain the guest memory layout, entry
  point, semihosting contract and cleanup where they are used. Examples expose
  the project's current API without implying a stable JavaScript package.
- **Visible lifecycle.** Show loading, running, completion and errors. Release
  busy controls on every exit path. An interrupted run must stop the Worker and
  its pending request; the next run must start cleanly. Distinguish cancelled or
  timed-out runs from guest faults and output mismatches.
- **Actionable deployment failures.** Explain HTTP serving, required Wasm
  features, missing files and the relationship between the generated JavaScript
  and Wasm. Keep all runtime assets local and document the complete output
  directory. Test nested-path hosting as well as a site root.
- **Accessible presentation.** Use native controls, visible keyboard focus,
  text labels for success and failure, a polite status region and a layout that
  works on narrow screens. Keep large logs and internal compiler counters out of
  the primary reading path. Respect reduced-motion preferences if adding motion.
- **Honest measurements.** Identify the engine and what each timing includes.
  Keep native-reference correctness separate from performance claims. Follow
  [the performance protocol](../docs/Performance.md) for comparisons.
- **Continuous verification.** Test input rejection, startup failures,
  cancellation, retry and resource cleanup in focused host tests. Execute the
  actual generated host and both guest architectures in real Chromium and
  Firefox Workers. Keep the profiling-on and profiling-off configurations in
  the browser CI matrix. An in-memory JavaScript mock cannot establish Wasm or
  browser compatibility.

The [browser guide](browser/README.md) contains runnable checks; the
[release procedure](../docs/Releasing.md) defines the broader release gates.
When a runtime asset is added, update CMake's copied files and the browser smoke
driver's artifact/MIME lists together. Keep failure messages and documented
recovery steps aligned.

## Research and design references

The examples refresh was informed by inspecting the following public sources
on 2026-10-30. GitHub links pin the inspected revisions so the observations can
be checked later. These are specific practices adopted from the sources, not
claims that every aspect of those projects is a template for this one.

| Source | Observed practice | Application here |
| --- | --- | --- |
| [v86 examples](https://github.com/copy/v86/tree/cc37f35c43bf14865d74ad76879ecf9410f69eb1/examples) | Separate `basic`, `worker`, `destroy`, and `nodejs` examples each isolate one host concern. | Offer a small runnable integration beside the complete Worker demo; make teardown explicit. |
| [sql.js usage](https://github.com/sql-js/sql.js/blob/4f1c22d0cf7d91b78e5feedab62212e8127751f4/README.md#usage) | A short initialization example explains asynchronous loading and the separate Wasm asset; resource cleanup appears next to API use. | Document generated assets and startup before the detailed host protocol. |
| [ffmpeg.wasm usage](https://github.com/ffmpegwasm/ffmpeg.wasm/blob/f876f907c7e9b9bf51d4ed0b913a855a63ae63fc/apps/website/docs/getting-started/usage.md) and [Worker lifecycle](https://github.com/ffmpegwasm/ffmpeg.wasm/blob/f876f907c7e9b9bf51d4ed0b913a855a63ae63fc/packages/ffmpeg/src/classes.ts) | Examples distinguish loading from execution and show execution timeouts. `terminate()` rejects outstanding requests and destroys the Worker. | Keep long execution off the page, provide cancellation and a bounded wait, and make retry start from a usable state. |
| [Ruffle self-hosting](https://github.com/ruffle-rs/ruffle/blob/36bd357f3f9f7066706200ab6a5fd705c32081ac/web/packages/selfhosted/README.md) | The non-bundler path explains hosting the whole distribution and configuring Wasm MIME types. | Keep the demo deployable as static files with clear HTTP and asset requirements. |
| [Emscripten module API](https://emscripten.org/docs/api_reference/module.html#Module.locateFile) | `locateFile` resolves assets; startup configuration precedes initialization; `onAbort` is terminal for that module. | Document asset placement and reset failed module/Worker state instead of reusing an aborted runtime. |
| [MDN status role](https://developer.mozilla.org/en-US/docs/Web/Accessibility/ARIA/Reference/Roles/status_role) | `role="status"` announces advisory updates politely without moving focus. | Announce progress and results without stealing keyboard focus or relying only on color. |

The Pages package examples are also executable documentation. With a locally
packed candidate, validate the exact HTML snippets in Node and a Vite production
Worker (including the non-root base path):

```sh
npm ci --prefix tests/npm --ignore-scripts --no-audit --no-fund
node tests/npm/verify_pages_examples.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz \
  /path/to/chrome --html build-em/examples/browser/dist/index.html
```

Keep the displayed version and installation instructions aligned with the
package manifest. Distinguish local candidate installation from a registry
release. Preserve the full snippets, expected outputs, cleanup and asset-loading
configuration when editing presentation. Presentation edits must keep the
header's System/Light/Dark theme cycle (including `localStorage` persistence
and the System default) and the hero's conceptual-pipeline motion control
(Pause/Play, hidden and stopped under reduced motion); the browser interaction
check verifies both. Keep the skip link above the sticky header and preserve
reachable cancellation controls in short as well as narrow viewports; sticky
benchmark toolbars must yield to normal document flow when height is limited.

Benchmark code viewers follow [Prism's manual highlighting API](https://prismjs.com/docs/Prism.html)
and its [selective language loading guidance](https://prismjs.com/#basic-usage).
Fetch the actual source when the viewer opens, show it immediately, then load
only the local core and required C/JavaScript grammar. Do not auto-detect a
language or depend on a runtime CDN. Let Prism escape source markup, preserve
exact copy/download contents, and keep plain source when highlighting fails.
Verify narrow layouts and light, dark and forced-color presentation. Keep the
vendored grammar version and license pinned.

The implementation remains project-specific: execution is synchronous inside
the Worker, so interrupting a running guest requires terminating that Worker.
Cancelling only a JavaScript promise would leave guest execution running.
