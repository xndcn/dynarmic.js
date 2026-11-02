# qemu-wasm CoreMark comparison (2026-10-28)

## Outcome

**No valid performance comparison was produced.** The exact A64 CoreMark ELF ran under qemu-wasm and reported 364.169999 guest seconds, but its final CRC was `0xf2d2` instead of the required `0x4983`. The reported `549.194059 CoreMark/s` is retained only as an observation from incorrect execution, not as a valid benchmark result or speedup denominator.

For diagnosis only, dividing the existing partial dynarmic throughput references by this invalid qemu-wasm observation gives 6.03x (baseline) and 6.09x (candidate). These are **not formal speedups**: qemu-wasm failed workload integrity, and the dynarmic references are eight-pair loaded-host diagnostics rather than a controlled cross-implementation cohort.

| Implementation | Guest kernel time | Observed CoreMark/s | Final CRC | Use |
|---|---:|---:|---|---|
| dynarmic baseline, partial median | 60.415299 s | 3310.419766 | `0x4983` | loaded-host diagnostic reference |
| dynarmic candidate, partial median | 59.844082 s | 3342.018013 | `0x4983` | loaded-host diagnostic reference |
| qemu-wasm, one run | 364.169999 s | 549.194059 | **`0xf2d2`** | invalid execution; diagnostic observation only |
| native system QEMU, adapter smoke | 31.370008 s | 6375.516385 | `0x4983` | correctness oracle only, not a Wasm comparison |

CoreMark's own `Correct operation validated` line is not sufficient here: this workload checks the individual list/matrix/state CRCs but does not reject the mismatched accumulated final CRC. The harness therefore checks `crcfinal == 0x4983` explicitly.

## Identity and scope

- Workload: original unmodified A64 CoreMark ELF, 200,000 iterations, memory mode 0.
- Guest SHA-256: `ba36aff1dd9b9364a2b3d4c812761389ab483ed01f7a1bb56e48f24394f86853`.
- Adapter SHA-256 for this run: `66c385ee536bfe6e5537813f7a4e8ea3e9bdb508b5624a6a39008b80e50e13d4`.
- qemu-wasm source: [ktock/qemu-wasm](https://github.com/ktock/qemu-wasm), revision `0ef7b4e2814b231705d8371dd7997f5b72e70baf`.
- Runtime: official prebuilt `raspi3ap` runtime from `ktock/qemu-wasm-demo-images`, revision `b7c549b5e6f4c376f76483a03e983214421434ad`; QEMU emulator version 8.2.0.
- Runtime SHA-256: loader `88997f526b8ddd53a8c5da9b3ecab052851175e532f89ecb5425aa00e494d7a6`, Wasm `b37148882e0b7e6d3ca93072439f5680069c303cbb0ee5b6901b9beb395cde42`, worker `0fe5449bd103bcac7ba9a5adc6a8eb5357d2d260522529dbe90ba95bdca910c2`.
- Node: v24.19.0. Both the qemu-wasm run and the referenced dynarmic cohort used host CPU affinity `0,2,4,6`.
- Timing: CoreMark's guest timer surrounds `iterate`. The adapter implements the existing `svc #0`, `svc #1`, and `svc #4` ABI and converts the ARM generic timer to microseconds. No Linux guest or separately compiled CoreMark binary is involved.
- System mismatch: qemu-wasm runs the `raspi3ap` system machine with four required SMP CPUs; CoreMark itself is single-threaded. dynarmic directly executes the guest. This is not an instruction-for-instruction controlled host experiment.

The qemu-wasm process emitted the complete CoreMark report but did not exit; the runner preserved the output and terminated it at the preregistered 900-second timeout. The guest-reported 364.169999-second interval is distinct from that 900.101-second process lifetime.

## Evidence

- `evidence/qemu-wasm-baremetal-1.json`: raw runner record, arguments, artifact hashes, complete serial output, and timeout.
- `evidence/qemu-wasm-baremetal-1.stdout` / `.stderr`: unmodified process streams.
- `evidence/comparison-analysis.json`: parsed values and explicitly invalid comparison classification.
- `evidence/native-baremetal-adapter-5.stderr`: native system QEMU correctness smoke using the same guest and adapter generation.
- `evidence/host-before-baremetal.txt`: host timestamp, load, and leading processes before the qemu-wasm run.
- `scripts/run-baremetal-v1.mjs`, `scripts/guest-adapter-v1.S`, and `scripts/guest-adapter.ld`: exact harness/adapter source generation used for the measurement.
- `scripts/analyze.mjs`: deterministic analyzer; it refuses to overwrite evidence.

The reviewed adapter was also rebuilt with explicit SVC exception-class validation, overflow-safe clock conversion, and an explicit guest-exit marker. A subsequent native system QEMU smoke produced `0x4983` and the exit marker. That revised adapter was not retroactively substituted into the preserved qemu-wasm observation.

## Reproduction and analysis

The original measurement workspace was `/tmp/dynarmic-qemu-wasm-comparison`, with the pinned runtime under `downloads/raspi3ap`, the guest at `/tmp/dynarmic-coremark-long/a64.elf`, and the adapter at the workspace root. The exact run was:

```sh
env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node /tmp/dynarmic-qemu-wasm-comparison/scripts/run-baremetal.mjs \
  /tmp/dynarmic-qemu-wasm-comparison/evidence/qemu-wasm-baremetal-1.json
```

Reanalyze the preserved records without rerunning either implementation:

```sh
node scripts/analyze.mjs \
  evidence/qemu-wasm-baremetal-1.json \
  /tmp/dynarmic-focused-region-performance-current-load/partial-analysis.json \
  /tmp/qemu-wasm-comparison-analysis.json
```

## Follow-up

The individual algorithm CRCs are correct while the long-run accumulated final CRC is wrong. This is consistent with a defect in qemu-wasm's hot-TB/Wasm execution path, but that mechanism is not yet proven. A future comparison must first obtain `0x4983` under qemu-wasm, then collect fresh interleaved dynarmic/qemu-wasm observations under one declared host environment. The invalid observation must not be replaced, discarded, or presented as a passing CoreMark run.
