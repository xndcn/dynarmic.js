# Local npm release acceptance — 2026-10-31

The experimental `dynarmic.js@0.1.0-alpha.0` candidate passed the local package
and consumer gates. No registry upload, tag, push or deployment was performed.
The working tree is intentionally uncommitted; the artifact identifies itself
as a local snapshot, not a clean published release.

The final package is `build-npm/dynarmic.js-0.1.0-alpha.0.tgz`:

- Size: **5,831,756 bytes**, 14 distributed files.
- SHA-256: `634dc62a8ebb1471131c4f78d5979931126dcf5bbfe81af8fc49173a604d9bdf`.
- Complete-source archive SHA-256:
  `39ba8f7d803be1c56a9b641d14c311184eecae12c9d1a77bc2726cd4927d5e7c`.
- Runtime toolchain: Emscripten **6.0.13**, Boost **1.74.0**, CMake **4.0.2**.

The [validation manifest](validation.json) binds these files to the retained
reports. Every final Node/browser/tooling report identifies the same tarball
hash. The source-rebuild report additionally identifies the exact source
archive extracted from that tarball.

| Gate | Result and retained evidence |
| --- | --- |
| Node installed tarball | 18 checks each on Node 22.11.0 and 24.19.0: [Node 22](node22.json), [Node 24](node24.json) |
| Offline consumer | Installation with an empty npm cache, `--offline --ignore-scripts`, then default Wasm initialization: [report](offline.json) |
| Chromium module Worker | 15 checks in Chrome 156, including nested hosting, default/relocated/supplied Wasm and invalid bytes: [report](chromium.json) |
| Firefox module Worker | 16 checks in Firefox 157; phase-specific request records prove overrides do not fall back to the default asset: [report](firefox.json) |
| Package/type/bundler consumers | Strict publint; ATTW ESM-only; strict TS NodeNext and Bundler; CJS dynamic import; unchanged README example; Vite 8.3.5 production Worker: [report](tooling.json) |
| Distributed source rebuild | Extracted into `build-npm/rebuild-verified/source/dynarmic.js` without Git metadata, built independently, packed a private test package and passed all 18 consumer checks: [report and commands](rebuild.json) |
| Staging provenance | 15 tests reject stale/changed/missing compiler inputs, mismatched artifacts, non-production configurations and archive races: [log](packaging.log) |
| Package integrity | Actual npm pack JSON/inventory, modified Wasm, missing source, extra file, symlink and dirty-release rejection: [log](integrity.log) |
| npm publication rehearsal | Valid JSON from [npm pack](pack.json) and [publish dry-run](publish-dry-run.json); dry run used `--ignore-scripts --tag next` and uploaded nothing |
| Existing regressions | 23/23 [native CTest](native-ctest.log), 30/30 [CMake/infrastructure](cmake.log), 440/440 [JavaScript](js-regression.log); no skips in those runs |
| Native sanitizers | Rebuilt Debug backend with ASan/UBSan; 23/23 CTest checks pass with leak detection and halt-on-error: [log](sanitizer-ctest.log) |
| Strict full regression | `tests/regress.sh --strict build-em build-native` passes with no skips; 21 Unicorn differential campaigns have zero mismatches; browser host checks cover 18 halt/fault runs and 216 RAM-boundary runs: [summary](strict-regression.json), [log](strict-regression.log), [invocation](strict-run.txt) |

The type checker excludes the binary `runtime.wasm` subpath from JS/TS module
resolution. Node asset resolution and actual Vite asset emission/fetch exercise
that public export. Direct synchronous CommonJS `require()` is not the package
contract; the checked integration uses dynamic `import()`.

The strict gate retained full commands, guest outputs and fuzz logs under
`test-results/npm-strict-regression/`. CoreMark was not requested in this run;
the result does not qualify a CoreMark or comparative-performance claim.

## Corrections found by the gates

Emscripten 6 defaults omitted the `wasmBinary` incoming option. Early overrides
appeared to work while the generated runtime still loaded its default asset.
The package now explicitly enables `INCOMING_MODULE_JS_API=wasmBinary`. Invalid
bytes must reject, and browser tests prohibit fallback network requests. Node
also tests actual file/HTTP URLs, Buffer, ArrayBuffer and offset Uint8Array input.

The source packager now reads back the produced archive and hashes every
repository compiler input, shipped wrapper/type/metadata file and license. It
rejects inputs absent from the archive, including ignored files, and changes
that are reverted before an end-of-packaging worktree comparison.

The package verifier writes diagnostics to stderr so the `prepack` lifecycle
does not corrupt `npm pack --json` output. An actual pack/inventory test guards
this behavior.

Existing CMake test fixtures needed to represent the current memory metadata
and C benchmark reference file. A previously committed Thumb/BX correctness fix
also had an outdated A32 golden module; its update retained byte equality and
added eight semantic checks. See the [independent corpus review](corpus-review.md).
The backend source was not modified in this packaging work.

## Reproduction and publication boundary

Follow [the release procedure](../../../docs/Releasing.md#npm-package) and
[consumer test commands](../../../tests/npm/README.md). The package workflow
contains the same build, clean-source, integrity, consumer and source-rebuild
gates, with read-only permissions and artifact upload only. This session tested
the commands locally; it did not run the workflow on GitHub.

`node build-npm/package/verify.mjs --require-clean` correctly rejects this local
snapshot. To publish later, first finalize and commit the release, relink from
the clean recursive checkout, stage with `--require-clean`, and rerun gates on
the resulting exact tarball. Registry ownership/OIDC setup and actual publishing
remain separate external actions. The API stays experimental and retains the
documented CPU limitations; these results establish packaging and consumer
readiness, not stable 1.0 semantics or a performance claim.

This evidence directory is explicitly excluded from npm corresponding-source
archives, so retaining this report does not change the tested artifact.
