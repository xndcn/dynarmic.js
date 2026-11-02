# bcore comparison archive

bcore integration was removed from the supported examples, browser UI and Pages
workflow on 2026-11-02. Research findings and measurements remain under
`internals/`. Start with [the lessons learned](../../BcoreComparisonLessons.md).

This archive retains the C/Wasm, C/native and QuickJS/Wasm harnesses, the
historical browser integration and its checks. It is a source snapshot for
replaying experiments, not a component of the current example or Pages build.

- `source-snapshot.tar.gz`: 64 source/documentation files at their original
  repository-relative paths, including the shared files changed for the
  experiment and the internal attribution tools.
- `snapshot-manifest.json`: base revision, per-file size/SHA-256/mode, the
  reviewed tracked edits restored, and the added files removed from examples
  and tests. The snapshot contains no symlinks or generated binaries.
- `integration.patch`: the historical tracked diff, for review. The snapshot
  already includes its resulting files; do not also apply this patch.
- [OP1-CONFIG.md](OP1-CONFIG.md): the original webpage configuration audit.

## Replay in a separate checkout

Run from the current repository root. The new directory must not already exist:

```sh
git worktree add --detach build-bcore-replay 28bdfe42dee86457ede3f89916a338427aeb5618
git -C build-bcore-replay submodule update --init --recursive
tar -xzf internals/experiments/bcore-comparison/source-snapshot.tar.gz -C build-bcore-replay
cd build-bcore-replay
```

Then use the restored instructions:

| Experiment | Instructions inside the separate checkout |
| --- | --- |
| C through Blackfin/bcore Wasm, including toolchain setup | `examples/c-bench/README.md`, Blackfin section |
| C through native Dynarmic/bcore | `examples/c-bench/native/README.md` |
| Existing ten QuickJS JavaScript cases through bcore Wasm | `examples/quickjs/bcore/README.md` |
| Wasm or native attribution controls | `internals/measurements/2026-10-31-bcore-attribution/`, `internals/measurements/2026-11-01-native-instruction-analysis/` |

The base revision provides the unchanged shared benchmark and browser helper
files. Toolchains, upstream bcore and generated distributions are built using
the restored instructions; they are not supplied by this source archive. The
original result payloads remain in the main checkout's dated measurement
directories. Historical commands naming `examples/.../bcore` or
`examples/c-bench/native` refer to this restored tree.

The older LLVM-only C report describes a different configuration. Its own
frozen evidence remains authoritative for those original inputs; the snapshot
here retains the final CP/web-default integration, not every earlier candidate.

The original local build caches remain available. In this workspace, removed
example sources and the old public-dist bcore assets were additionally retained
under `build-bcore-archive/`; that ignored directory is not required to replay
the source snapshot and is not part of Pages.
