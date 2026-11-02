# npm and WebAssembly packaging research

Reviewed **2026-10-31**. The official documentation and GitHub files below were
retrieved successfully over HTTPS during this review. GitHub examples link to
the exact revisions inspected. These are packaging decisions and acceptance
criteria; the local release report supplies execution evidence. No registry
publication or GitHub deployment is part of this review.

## Primary sources

| Source | Observed practice | Application here |
| --- | --- | --- |
| [Node.js package entry points](https://nodejs.org/api/packages.html#package-entry-points), [conditional exports](https://nodejs.org/api/packages.html#conditional-exports), [community conditions](https://nodejs.org/api/packages.html#community-conditions-definitions) | `exports` defines public subpaths; explicit module type avoids ambiguous interpretation. Conditions are ordered, `types` belongs first, and a `default` fallback helps environments outside Node. | Publish a small explicit API and asset subpaths. Keep the generated C ABI private. Match the declaration file's module format to its JavaScript implementation. |
| [npm package.json](https://docs.npmjs.com/cli/v11/configuring-npm/package-json/) | `files` is an inclusion list; omitting it includes almost everything. README, license and package metadata have special inclusion rules. `prepare` also runs in development installation scenarios. | Stage a prebuilt package with an explicit file list. Inspect the actual tarball. Installing it must not require Emscripten, a C++ compiler, a source checkout or a download script. |
| [Emscripten modularized output](https://emscripten.org/docs/compiling/Modularized-Output.html) | `MODULARIZE` creates an asynchronous factory and separate instances; `EXPORT_ES6` exports that factory. The single-instance alternative is explicitly experimental. | Use an asynchronous JavaScript factory around the ordinary modularized runtime, with independent CPU state per instance and explicit disposal. |
| [Emscripten Module API](https://emscripten.org/docs/api_reference/module.html#Module.locateFile) | `locateFile` maps runtime assets to their actual locations. Its prefix can be empty. Asset locations can differ from the JavaScript location. | Resolve default assets relative to the module, and offer a deliberate custom Wasm URL or byte-buffer path. Do not infer asset paths from the current page or working directory. |
| [sql.js package manifest](https://github.com/sql-js/sql.js/blob/4f1c22d0cf7d91b78e5feedab62212e8127751f4/package.json), [README](https://github.com/sql-js/sql.js/blob/4f1c22d0cf7d91b78e5feedab62212e8127751f4/README.md) | Distinguishes browser and default entry points, ships the Wasm separately, and documents `locateFile`; Node does not need custom asset configuration. Its README explicitly distinguishes normal, debug, inline and Worker variants. | Make the first Node call work with installed assets. Explain bundler asset handling and Worker use with complete examples. Add variants only when supported by a tested need. |
| [quickjs-emscripten package manifest](https://github.com/justjake/quickjs-emscripten/blob/7b7af98e4e69757c64c27aac46a74e1e07229545/packages/quickjs-emscripten/package.json), [README](https://github.com/justjake/quickjs-emscripten/blob/7b7af98e4e69757c64c27aac46a74e1e07229545/README.md) | Publishes types and explicit entry points, excludes tests/build metadata, documents resource ownership and disposal, and separates runtime variants from the high-level API. | Expose the CPU API with useful types, document lifetime rules, and keep runtime generation separate from package consumption. |
| [ffmpeg.wasm package manifest](https://github.com/ffmpegwasm/ffmpeg.wasm/blob/f876f907c7e9b9bf51d4ed0b913a855a63ae63fc/packages/ffmpeg/package.json), [Worker construction](https://github.com/ffmpegwasm/ffmpeg.wasm/blob/f876f907c7e9b9bf51d4ed0b913a855a63ae63fc/packages/ffmpeg/src/classes.ts) | Declares a Worker subpath and TypeScript declarations. Creates a module Worker with a URL relative to `import.meta.url`, with a configurable URL escape hatch. Its Node entry is deliberately different from the browser implementation. | Test module Workers as actual consumers. State supported environments honestly; successful bundling alone does not prove browser or Node execution. |
| [publint](https://publint.dev/docs/), [Are the types wrong? CLI](https://github.com/arethetypeswrong/arethetypeswrong.github.io/blob/ee85fcadb8c1130627f36d0f48cc5b8b6b12451c/packages/cli/README.md) | Inspect published package metadata and type resolution, including ESM/CJS mismatches and Node/bundler resolution. ATTW has an explicit `esm-only` profile. | Run these tools against the staged package or tarball and add actual TypeScript consumer compilation. Use a profile matching the supported contract; do not disguise unsupported CJS as a working entry point. |
| [npm trusted publishing](https://docs.npmjs.com/trusted-publishers/), [provenance](https://docs.npmjs.com/generating-provenance-statements/) | OIDC avoids long-lived publish tokens. Current prerequisites include npm 11.5.1+ and Node 22.14.0+. GitHub Actions requires `id-token: write`. Automatic provenance requires a public repository and public package. | Prepare an artifact-only local/CI validation path now. If publishing is later authorized, use a separately scoped release job and verify the registry's trusted-publisher configuration. |
| [MIT License](https://opensource.org/license/mit) | Copies or substantial portions of the software must retain the copyright and permission notices. | Retain the project's MIT license and dependency notices with prebuilt runtime distribution. Continue shipping matching complete source and rebuild instructions for reproducibility. |

The mature projects above solve different compatibility requirements. Their
manifests are evidence of real practices, not templates to copy wholesale:
sql.js exposes a broad `dist/*` surface, quickjs-emscripten supports many variants,
and ffmpeg.wasm intentionally does not supply its browser API to Node.
dynarmic.js can keep a smaller, explicitly tested contract.

## API and import decisions

Use an ESM-first package with bundled declarations and a Promise-returning
factory. Separate package loading from runtime initialization: merely importing
the API should not start a guest or require DOM globals. CommonJS consumers can
use `import()`; do not advertise synchronous `require()` support unless its
implementation, declarations and minimum Node version are independently tested.
Supporting ESM does not require duplicating a large runtime in a CJS build.

The facade should own runtime lifetime and validate inputs before entering the
private C ABI. Architecture-specific types should distinguish A32 numeric
registers and A64 `bigint` registers. Guest stops should be explicit results,
including SVC, instruction budget, unsupported instruction and memory fault.
Disposal must be documented and covered by lifecycle tests. These choices keep
ordinary users away from generated `_malloc`/`_free` conventions and raw pointers.

Publish the runtime Wasm through a named asset export so bundlers can address it.
For an unbundled installation, resolve it adjacent to the runtime JavaScript.
For bundlers, show an explicit asset URL import or static copy and the factory's
URL option. Accepting supplied Wasm bytes also serves environments that need
their own fetch/cache policy. These paths need separate tests because a Node
filesystem path, a browser URL and a bundler asset URL are different values.

The browser example should create a module Worker; guest execution is
synchronous. Document how to terminate the Worker for hard cancellation rather
than promising that an asynchronous main-thread message interrupts a synchronous
guest call. Describe WebAssembly features, CSP requirements and missing-feature
errors as part of the environment contract. Do not promise a universal sandbox
or precise per-instruction time limit from a block-based budget.

## Artifact and source integrity

Build into a dedicated staging directory with its own package manifest. Include
only the facade, declarations, runtime JavaScript/Wasm, documentation, notices,
source archive and provenance records required by the distribution. No install
lifecycle step should compile native code or fetch a second runtime. A local
consumer must be able to install the tarball with scripts disabled.

Record the compiler and SDK versions, build flags, source inventory, upstream
revision and hashes of the runtime assets when the runtime is linked. Package
assembly must reject artifacts whose recorded inputs no longer match their
source. Computing new hashes only at packaging time cannot establish which
source produced an old binary. Keep a source snapshot for local uncommitted
validation; a release candidate must additionally satisfy the documented clean
source requirement.

The corresponding-source archive should contain the local backend, host and
facade sources, build/packaging scripts and pinned upstream submodules, together
with applicable licenses. Extract it into an empty directory and rebuild the
runtime and package there. This tests the distributed source rather than
silently relying on files elsewhere in a developer's checkout. Rebuildability
is separate from byte-for-byte reproducibility; do not claim the latter without
comparing independently produced outputs under a specified environment.

## Consumer acceptance matrix

Run acceptance checks on the exact result of `npm pack`, installed into temporary
consumer projects outside the repository. A direct source import, monorepo link
or successful build can conceal missing package files and undeclared tools.

| Consumer or invariant | Required evidence |
| --- | --- |
| Package contents | Inspect `npm pack --json` and the archive inventory; check every declared export and required source/license file exists, and reject accidental development files. |
| Node ESM | Run real ARM32, Thumb and AArch64 programs using only the installed package and default asset resolution. Test the declared minimum Node version. |
| CommonJS integration | A `.cjs` consumer performs dynamic `import()` and runs a guest; support for direct `require()` is a separate claim. |
| TypeScript | Compile representative consumers with `NodeNext` and `Bundler` module resolution, including architecture-specific register values, factory options, disposal and stop-result narrowing. |
| Browser Worker | Load the installed package under a nested local HTTP path and run real guests in Chromium and Firefox. Validate default and custom asset handling and useful missing-asset failures. |
| Bundler | Build and execute a consumer with an actual bundler. Check the emitted Wasm asset, module Worker path and a non-root deployment base. |
| Lifecycle and state | Verify independent instances, disposal, invalid input rejection, guest stop/fault behavior and code-cache invalidation after modifying executable memory. |
| Metadata and types | Run publint and ATTW with declared support profiles, plus consumer compilation. These complement runtime tests rather than replacing them. |
| Source completeness | Extract the shipped source, rebuild without the original checkout and run an installed-tarball smoke test on that rebuild. |
| Local publication rehearsal | Run pack and publish dry-run checks with no credentials. Retain the package name/version, tarball checksum, tool versions and commands alongside the results. |

## Future publication boundary

For this task, validation creates local artifacts only. A future registry
publication requires choosing and confirming ownership of the package name,
selecting a release version/tag, and configuring npm's trusted publisher.
An experimental API should use an explicitly prerelease version and a deliberate
dist-tag rather than accidentally becoming `latest`.

The npm documentation retrieved for this review includes staged publishing:
trusted-publisher configurations created after **2026-09-03** default to allowing
`npm stage publish`, and direct `npm publish` can be enabled separately. A
maintainer can choose a stage-only publisher and approve the package through
npm's documented flow. This is a future external action, not a local dry-run.
Do not assume that an older workflow example captures the registry's current
configuration requirements.

Keep default CI jobs read-only and artifact-producing. Grant OIDC publication
permission only to the intended publication job, tied to the configured workflow
and, where used, the release environment. npm provenance records build origin;
it does not replace package tests, corresponding source, source review or
verification that a binary matches the intended inputs.
