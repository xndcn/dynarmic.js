# Region corpus review during npm release preparation

Reviewed locally on 2026-10-31 against source revision
`2bd45b378e205baeffe797a3d5bc320769d53167`. The native emitter was rebuilt before
recording the candidate. No emitter or runtime code changed during this review.

The native `region_corpus` gate exposed a stale golden: the A32 conditional
backward-branch module grew from 396 to 398 bytes after the already committed
ARM/Thumb correctness fix
`6701c8dec3a21adf78b13939b40d1fa421bc9371`. The previous baseline identified
`e33352f959ac745f18ec9c9854efeda4e8a88131`, which predates that fix.

## Attribution and instruction comparison

`git diff e33352f..2bd45b3 -- src/` changes only
`src/dynarmic/backend/wasm/emit_wasm_a32.cpp`; its changes are exactly the Thumb
ITSTATE/dynamic-exit correction in `6701c8d`. The corpus's synthetic guest
instructions and region-selection inputs are unchanged. The recorder's later
source-archive provenance support does not affect emission.

Binaryen `wasm-dis` version `133 (version_133-96-ga631c1445)` disassembled both
A32 modules. The only instruction differences are inside `BX LR`:

- The old target mask is always `-2`. The new mask selects `-2` for Thumb and
  `-4` for ARM, based on bit 0 of the target.
- The old upper descriptor loads the current state's upper word and clears
  T. The new code uses the completed block's `EndLocation` upper descriptor
  (constant zero for this ARM-only source block), then inserts the target's T
  bit. This is the same completed-descriptor rule that fixes stale Thumb ITSTATE.

The pinned upstream native implementation independently confirms both rules:
`A32EmitX64::EmitA32BXWritePC` in
`externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.cpp`, upstream revision
`a41c380246d3d9f9874f0f792d234dc0cc17c180`.

The A32 module hashes are:

| Corpus | Bytes | SHA-256 |
| --- | ---: | --- |
| Previous golden | 396 | `43cb66bbedffa347736900dd239cf1398f3dd562b8055ea3f94afea1272e80c7` |
| Reviewed current emission | 398 | `411c7e98fa828a2e67defe90aac0a7b1b65be86bf3eb484695da8511a7f49b88` |

All four A64 files are byte-identical to the previous golden. Case names,
ordering, descriptors, entry indices and emission settings are unchanged.

## Independent execution evidence

The existing 20 guest-semantic executions passed on **both** the previous
golden and current emission. They cover both A32 condition paths, RSB and fast
dispatch hits/misses, A64 BL/RET, BRK and the 64-block boundary.

Additional direct execution of the real A32 modules exposed the intended
behavior change rather than merely accepting a new byte count:

| LR | Expected mode / PC | Previous golden PC | Current PC |
| --- | --- | --- | --- |
| `0x5000` | ARM / `0x5000` | `0x5000` | `0x5000` |
| `0x5001` | Thumb / `0x5000` | `0x5000` | `0x5000` |
| `0x5002` | ARM / `0x5000` | **`0x5002` (incorrect)** | `0x5000` |
| `0x5003` | Thumb / `0x5002` | `0x5002` | `0x5002` |

These four targets now run on both condition paths as eight permanent semantic
checks in `tests/tools/region_corpus_semantics.mjs`. They verify target alignment,
the completed descriptor/T bit, preserved NZCV and return to dispatch. The old
golden, recovered directly with `git show HEAD:...`, fails the independent
`BX LR=5002 alignment` assertion. The reviewed golden and freshly recorded
candidate each pass all **28** semantic executions.

`node build-em/tests/thumb_it_return_test.js` additionally passed its **24**
existing focused runtime configurations (12 repeated-call stack/register cases
and 12 dynamic-exit/ARM-alignment cases).

## Accepted update and gate

Only `a32_conditional_backward.wasm` was replaced. Its manifest byte count and
review description were updated, and the manifest/comparator/fixture revision
pins now identify the actual reviewed source revision `2bd45b3...`. The raw-byte
comparison, descriptor/order checks, mutation rejection tests and other four
goldens remain intact; semantic coverage increased from 20 to 28 executions.

```sh
ctest --test-dir build-native -R '^region_corpus$' --output-on-failure
node build-em/tests/thumb_it_return_test.js
```

Both commands passed. The focused CTest output is retained locally in
`test-results/npm-corpus-review.log`. This is a correction to stale test data
following a verified correctness fix, with no new backend behavior change.
