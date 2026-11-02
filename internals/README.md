# Maintainer records and experiments

This directory holds engineering handoffs, historical plans, one-off experiment
tools, and the evidence captured with them. Start with the [public
documentation](../README.md) for supported setup, usage, and current limitations.
The ordinary regression tests, reusable benchmark tools, and fuzz corpus remain
in [`tests/`](../tests/).

## Contents

- [Handoff](Handoff.md): chronological engineering notes and local environment
  details. Commands and status claims describe their recorded revision.
- [Historical roadmap](Roadmap.md): the original milestone and experiment log.
  The [public roadmap](../docs/Roadmap.md) describes current direction.
- [IR audit](IrAudit.md): the scope and results of the October 2026 semantic audit.
- [Backend implementation history](WasmBackendHistory.md): the original design
  narrative; the [current backend reference](../docs/WasmBackend.md) documents the
  active implementation.
- [bcore comparison lessons](BcoreComparisonLessons.md): configurations,
  porting pitfalls, performance findings and limits; the experimental harnesses
  are archived for isolated replay and are not part of examples or Pages.
- [`measurements/`](measurements/): dated experiments, analysis scripts, protocol
  tests, raw measurements, and frozen evidence inventories.

These files are retained for reproducibility, not as release guarantees or
portable setup instructions. Local paths, hardware-specific timings, and older
source revisions are expected. This directory is part of the repository; it is
not a place to store credentials or confidential data.

## Running the internal checks

From the repository root, with Node.js 22 or newer, CMake, and Git installed:

```sh
find internals/measurements -name '*.test.mjs' ! -name emit_probe.test.mjs -print0 |
  xargs -0 node --test
```

They can also be registered as a separate CTest group in a native build:

```sh
cmake -S . -B build-native -DDYNARMIC_JS_BUILD_INTERNAL_TESTS=ON
ctest --test-dir build-native -L internal --output-on-failure
```

The internal evidence workflow runs these checks separately from public unit and
browser checks. `emit_probe.test.mjs` is excluded because it needs the native
emitter and guest image from its historical experiment; its
[experiment record](measurements/2026-10-27-hot-edge-static-gate/README.md) documents
the required `EMIT_PROBE` and `EMIT_PROBE_INPUT` environment variables.

## Preserving evidence

The directory was moved from `docs/measurements/` during release preparation.
Runnable documentation uses the new paths. Frozen command transcripts, captured
Git status, JSON provenance, compressed reports, and byte-pinned payloads retain
their original contents, including old paths. Those paths describe how the
evidence was collected; substitute `internals/measurements/` when replaying an
old root-relative command. Hash inventories that name repository-relative paths
have been relocated without changing payload hashes.

Keep historical payloads intact. Capture new runs in a new directory and explain
the source revision, configuration, environment, correctness checks, and limits
of any comparison.
