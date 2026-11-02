# WASM Backend Design

This document describes the design of the WebAssembly backend for dynarmic: a
backend that lets dynarmic itself be compiled to WASM (via Emscripten) and then
JIT-compile ARM32 (A32, ARM and Thumb) and AArch64 (A64) guest code into WASM
modules at runtime in the browser. Both frontends share one emitter core; the
frontend-specific parts are described in §13.

Status note (2026-10-22): this file contains implementation history and older
measurements without reproducible artifacts. The active release plan is
[Roadmap.md](Roadmap.md); supported contracts are in [Architecture.md](../docs/Architecture.md).
The historical timings below are not release performance claims.

The design is modeled on [v86](https://github.com/copy/v86)'s runtime WASM JIT
(see `docs/how-it-works.md` in the v86 repo), adapted to dynarmic's IR-driven
architecture.

## 1. The problem

A normal dynarmic backend emits host machine code into RWX memory. In a
browser:

- WASM linear memory is never executable; there is no `mmap(PROT_EXEC)`.
- New executable code enters the system only through
  `WebAssembly.compile/instantiate` (a JS API, callable from the outer module
  via imported JS glue).
- WASM has structured control flow only (no `goto`), no patching of emitted
  code, no cross-function locals.
- Browsers tolerate only a few thousand live modules; each module has fixed
  memory overhead.

v86 solves these constraints with a proven architecture which we adopt.

## 2. Runtime architecture

```
+------------------------------ JS glue (EM_JS / --js-library) ----------------------------+
|  InstantiateBlock(ptr, len) -> table_index                                             |
|    bytes = HEAPU8.slice(ptr, ptr + len)                                                 |
|    mod = new WebAssembly.Module(bytes)                                                  |
|    inst = new WebAssembly.Instance(mod, imports)                                        |
|    return addFunction(inst.exports.f, 'iii')                                            |
+-----------------------------------------------------------------------------------------+
            ^ emitted module bytes                     | funcref
            |                                          v
+-------------------------------- main module (dynarmic, emscripten) ---------------------+
|  A32AddressSpace: translate → optimize → EmitWASM → bytes                               |
|  block cache: robin_map<LocationDescriptor, table_index>                                |
|  dispatcher: run_code loop calls block via function pointer (call_indirect)             |
|  shared: wasmMemory (JitState, guest RAM, page table), wasmTable (block funcrefs)       |
+-----------------------------------------------------------------------------------------+
```

Key mechanisms (all proven in v86):

1. **One outer module + many tiny inner modules.** dynarmic (outer) owns the
   linear memory, the function table, the block cache and the dispatcher. Each
   compiled guest block is a small single-function module (inner).
2. **Shared memory.** Inner modules import the outer module's
   `WebAssembly.Memory`. `A32JitState`, guest RAM and the fastmem page table
   all live at fixed offsets, directly loadable/storable from generated code
   with zero glue.
3. **Shared table.** Inner modules' exported entry funcref is stored into the
   outer module's indirect function table. In Emscripten, a C function pointer
   *is* a table index, so the dispatcher calls JIT blocks through ordinary C
   function pointers (`u32 (*)(uptr state, uptr execution_context)`), which
   lower to a single `call_indirect`.
4. **Host callbacks as imports.** dynarmic exports devirtualized thunks
   (`MemoryRead32`, `AddTicks`, `CallSVC`, interpreter fallback, …) from the
   outer module; inner modules import them by name. WASM→WASM calls, no JS
   trampolines on hot paths.
5. **Synchronous instantiation.** `new WebAssembly.Module/Instance` is used
   in Node and in the browser Worker. Async compilation, cancellation and
   invalidation-during-compilation handling are future work, not implemented
   guarantees.

## 3. Block emission model

### 3.1 One block per module (v1)

Each dynarmic `IR::Block` compiles to one WASM module exporting a single
function:

```wat
(func $f (param $state i32) (param $execution_context i32) (result i32)  ;; -> HaltReason / next action
  ...)
```

The block cache maps `LocationDescriptor::Value()` → table index.
Cross-block transitions v1: the block writes the next `LocationDescriptor`
(`regs[15]` + `upper_location_descriptor`) and returns
`HaltReason::ReturnToDispatch`-equivalent; the C++ dispatcher loop looks up the
next block and calls it — the same dispatcher-mediated model the riscv64
backend uses today.

### 3.2 Locals as SSA registers

WASM provides indexed locals, and V8/JSC perform their own register allocation.
The WASM "register allocator" therefore maps **one local per IR value**:

| IR type | WASM type |
|---------|-----------|
| U1, U8, U16, U32, NZCV, Cond, A32Reg... | i32 |
| U64 | i64 |
| U128 | v128 (SIMD) |

Dynarmic FP IR values are bit-pattern integers (`FPAdd32: U32,U32 -> U32`);
they stay in i32/i64 locals and are reinterpreted at FP-op boundaries.

There is no spilling. The per-backend `RegAlloc` becomes a local-index
assignment pass (with free-list reuse to keep local counts low — browsers
degrade on functions with thousands of locals, cf. v86's
`alloc_local/free_local` pools).

### 3.3 Terminals

- `ReturnToDispatch`: write next location, return 0 to the dispatcher.
- `LinkBlock` / `LinkBlockFast`: **direct block chaining** (implemented).
  Each chainable edge owns a u32 *link slot* in linear memory (calloc'd per
  block by the address space, zero-initialized). Emitted code: write the next
  location descriptor to state, load the slot; if it is nonzero and
  `ectx.halt_reason == 0`, `return_call_indirect` (wasm tail call — no stack
  growth, halt return values propagate) into `table[slot]`; otherwise store
  the slot's address into `ectx.link_slot_addr` and return. The dispatcher
  patches `*slot` with the successor's table index after the call. Inner
  modules import the main module's indirect table as `("e","t")`.
  `LinkBlock` is chained only when cycle counting is off (the dispatcher must
  see blocks to count them); `LinkBlockFast` is always chained.
- `PopRSBHint`: **return stack buffer** (implemented). An 8-entry circular
  buffer lives in the ExecutionContext; `PushRSB` (emitted by the frontend
  for `bl`) records the return descriptor, overwriting the entry only when
  the descriptor changes so a lazily-patched table index survives. On
  `bx lr` the pop path compares `r15 | upper<<32` against the entry, and on a
  hit tail-calls like chaining; an unlinked hit requests a patch via
  `link_slot_addr`; a miss returns to the dispatcher. Cache clears must wipe
  the whole RSB (entries hold table indices of released blocks).
  Pushes take the entry's table index from the fast dispatch table (below)
  when the return target is there, so two call sites of one function that
  alternate in a slot still hit (before this, each overwrite reset the index
  and every return went through the dispatcher).
- `FastDispatchHint` (and RSB misses): a 4096-entry direct-mapped table
  `{descriptor, table index}` owned by the address space, indexed by
  `FastDispatchSlot(descriptor)`. A hit tail-calls; a miss stores the entry's
  address in `ectx.fast_dispatch_entry` and the dispatcher fills it. The
  dispatcher also records every target it links there. Wiped together with
  the RSB on any invalidation.
- `If`: `EmitCond` + `if/else`. The flags come from the packed NZCV (a cached
  local when the register cache is active, §11), or straight from the operands
  of the block's last `cmp`/`subs`-style Add/Sub when that is the last flag
  write (`EmitContext::term_flags`): `cmp r0, r1; bne` becomes one `i32.ne`.
- `CheckHalt`: poll `ectx.halt_reason`. `CheckBit`: the check bit set by
  `SetCheckBit` earlier in the same block lives in an i32 local.
- `Interpret`: call the imported interpreter-fallback thunk.

### 3.4 Regions: several blocks per module (implemented)

As in v86 (which compiles a hot page with an internal `br_table` dispatcher),
`AddressSpace::GenerateRegion` compiles the entry block together with the
blocks reachable from it through direct branches (both arms of `If` /
`CheckBit`, cond-fail edges, and the return addresses of calls) into one
function: at most 64 blocks / 2000 IR
instructions, within 4 KiB of the entry, same FPCR/FPSCR mode bits, no
single-stepping. Blocks are laid out in guest-address order:

```
loop $dispatch
  block $B_{n-1} ... block $B_1 block $B_0
    br_table (local $next) $B_0 .. $B_{n-1}
  end  <block 0>   end  <block 1>  ...
```

A forward edge is a plain `br` to the target's still-open label; a backward
edge sets `$next`, decrements the iteration budget (§11.5) and branches to
`$dispatch`. A block that links back to itself runs in its own nested wasm
`loop` inside its slot (entered before its condition check; the self edge is
the budgeted `br_if`), so nested guest loops stay in one function; a region
of a single self-loop block compiles as the standalone self-loop of §11.
When the region's control flow is reducible (about two thirds of CoreMark's
regions), `emit_wasm_region.cpp` replaces the dispatch loop with a structured
layout after Ramsey's "Beyond Relooper": dominator-tree-ordered nested
`block`s plus a `loop` per loop header, so forward edges are a `br` to an
enclosing block label and back edges a budgeted `br_if` to the header -- no
`$next` local or `br_table`. An edge without an open label there leaves the
region like an outside edge.
A return (`PopRSBHint` / `FastDispatchHint`) first compares the guest location
with the region's call return sites; on a match it pops the RSB entry the call
pushed and branches inside the region. Guest registers and NZCV live in locals for the whole function
(the register cache of §11). Edges leaving the region chain like single
blocks. A32: an in-region edge that changes T/E/IT state stores the target's
`upper_location_descriptor` (`EmitRegionLocationTransition`).

Cycle-counted A32 regions can also check an optional `BoundaryState` before
in-region edges. With `boundary_hook_snapshot`, emission omits only the
instruction-hook loads for a static target whose global bits and conservative
slot are both clear; cycle, halt and board-flag guards remain dynamic. This is
an explicit invalidation contract, not a general hook cache: the host must
quiesce execution, invalidate the entire JIT for every hook-state or slot
mutation, and serialize mutation with compilation/publication. Range-only
invalidation is insufficient because callers and chained regions elsewhere
may contain the specialized edge. Leaving the snapshot empty retains the
fully dynamic hook guard.

Blocks are capped at 256 guest instructions (`kMaxBlockInstructions`):
upstream's IR passes recurse along Identity chains, and an unbounded block
(e.g. a page of zeros, `movs r0, r0` in Thumb) overflowed the Emscripten stack.

## 4. Memory access

v1: `ReadMemory{8,16,32,64}`/`WriteMemory*` lower to calls to imported
devirtualized callback thunks (correctness first).

v3 (flat window, implemented): if the host sets `UserConfig::fastmem_pointer`
and the page table maps a leading run of pages contiguously from it, those
accesses compile to `vaddr < limit ? load offset=base(vaddr) : thunk` -- one
compare plus one load, no page-table lookup. The host must keep those pages
mapped for the lifetime of the Jit (16 bytes of slack at the end go through the
thunk). `arm_init(2)` in the browser example uses it.

v2 (fastmem, implemented): the host's page table
(`UserConfig::page_table`, 4 KiB pages) lives in linear memory; generated
code inlines the entry load and, on a null entry (MMIO/unmapped), falls back
to the thunk. Validated on the memory-heavy bench: ~2x over thunk mode, ~5x
combined with chaining.

## 5. Self-modifying code

dynarmic does not detect guest stores to code; the host calls
`InvalidateCacheRange` (e.g. on an icache flush). Implemented:

- Every compiled block records the guest range it covers (`[start_pc, end_pc]`,
  padded by one instruction for folded conditional branches).
- `InvalidateRanges` drops only the overlapping blocks (`ReleaseBlock` recycles
  their table slots) and zeroes every chaining slot that was patched to point at
  a dropped block (`patched_slots`: slot address -> target descriptor, filled by
  the dispatcher). The RSB is wiped. Survivors keep their links.
- `ClearCache` (whole flush) is still available and clears `patched_slots`.
- Verified by `tests/hosts/smc_test.cpp`: patch a hot callee 3 times; only ~2
  blocks recompile per patch and the chained caller picks up the new code.
- `Step()` is supported: single-step blocks never chain, drop the
  single-stepping location flag on exit, and are excluded from branch folding and
  the self-loop transform.

## 6. Flags

dynarmic's IR already does the heavy lifting (`A32GetSetElimination`,
NZCV-as-value, pseudo-ops like `GetCarryFromOp` fused with their producer).
The WASM emitter adds v86-style producer/consumer fusion: `Add32` +
`GetCarryFromOp` emits `(u64(a)+u64(b))>>32` etc. without materializing
intermediate flag state.

## 7. Loop safety / interrupts

Generated blocks decrement `cycles_remaining` in memory per block (dynarmic
cycle counting), and the dispatcher enforces a budget before returning to JS
for timers/IRQs — mirroring v86's `LOOP_COUNTER` loop-safety exit, which
prevents an infinite guest loop from wedging IRQ delivery.

## 8. Instrumentation

From day one, port v86's profiler mindset: counters for cache hit/miss,
dispatch fallbacks, slow-path memory causes (esp. SMC frequency), per-terminal
distribution. Behind a compile flag.
The concrete `profile-v1` fields and benchmark artifact contract are documented
in [Architecture.md](../docs/Architecture.md) and [Performance.md](../docs/Performance.md).

## 9. Build integration

The backend lives out of tree in dynarmic.js; upstream dynarmic is an
unmodified submodule (`externals/dynarmic`). The top-level `CMakeLists.txt`:

- configures upstream with `ARCHITECTURE=wasm`, `DYNARMIC_FRONTENDS=A32 A64`
  and bundled externals, then `add_subdirectory`s it;
- injects the backend sources into the upstream `dynarmic` target
  (`src/CMakeLists.txt`) and adds `src/` to its include path, so backend
  headers keep their `dynarmic/backend/wasm/...` paths;
- removes upstream's host exception handler sources — there are no POSIX
  signals for fastmem faults (SMC uses explicit invalidation instead of page
  protection) and the backend never uses `ExceptionHandler`;
- removes `common/llvm_disassemble.cpp` (only the x64/arm64 backends call it;
  it assumes 64-bit pointers);
- applies `patches/*.patch` to *copies* of the affected upstream translation
  units in the build tree (`cmake/PatchedSources.cmake`) and compiles those
  copies instead — all wasm32 (`size_t` = 32 bits) fixes and candidates for
  upstreaming: `IREmitter::CallHostFunction` casts the host function pointer
  via `uptr` (0001), `FPRecipExponent` and the A64 FNEG mask compute in `u64`
  instead of `size_t` (0002, 0003); plus one memory fix: `IR::Block` starts
  its instruction pool with 256 slots instead of 4096 (0004; every block of a
  region is alive while it is emitted, which exhausted a 64 MiB heap);
- for Emscripten, builds fmt with `FMT_CONSTEVAL=` (works around a clang
  consteval bug in fmt's own sources) and defines
  `BOOST_NO_CXX98_FUNCTION_BASE` for Boost 1.74 headers.

`cmake/EmscriptenHost.cmake` provides `dynarmic_js_host()`, which links a host
executable and exports the thunks from `thunks.cpp` / `thunks_a64.cpp` that
generated block modules import.

- `unsupported` opcodes are handled by the existing `PolyfillPass`
  (`PolyfillOptions`) plus `Term::Interpret` fallback to the frontend
  interpreter loop (not a full interpreter — single-instruction step via
  callbacks).

## 10. Roadmap

| Phase | Content |
|-------|---------|
| 0 | WASM binary emitter (`wasm_binary.{h,cpp}`) — **done, validated against V8** |
| 1 | Skeleton: CMake, JitState, address space, JS glue, dispatcher; core data-processing IR — **done** |
| 2 | Memory ops via thunks; real A32 programs in Node — **done**: bare-metal `arm-none-eabi-gcc -O2` binaries (ARM/Thumb interworking, libgcc Thumb, IT blocks, `ldrd/strd`, `umull/mla`, div) checksum-match native execution (`tests/guest` + `tests/hosts/jit_runbin.cpp`) |
| 3 | Full A32 scalar coverage: FP (VFP), saturation, packed, barriers, exceptions, coprocessor — **done** (no stubs remain; the A64-only scalar FP ops landed with phase 6). Included: 31 saturation/MinMax ops + 34 packed media ops (`emit_wasm_saturation.cpp`, `emit_wasm_packed.cpp`), validated by `tests/guest/media.c` guest vs `media_ref.c` host reference (1472-entry mix log identical, checksum `ce5d6512`, both memory modes); 8 CRC32 ops (`emit_wasm_crc32.cpp`, bitwise-unrolled, checksum `d2a5eb40` via `tests/guest/crc.c`); 4 half-precision conversions (`FPHalfToSingle`/`FPSingleToHalf`/`FPHalfToDouble`/`FPDoubleToHalf`) via `FP::FPConvert` thunks (`fp_h2s`/`fp_s2h`/`fp_h2d`/`fp_d2h`), checksum `06843954` via `tests/guest/fp16.c` vs `fp16_ref.cpp` |

### Phase 3 pitfalls worth remembering

- **32-bit signed saturating add/sub**: clamping the wrapped i32 sum can never
  detect overflow — use sign analysis: `ovf = ((a^r)&(b^r))>>31` (add) /
  `((a^b)&(a^r))>>31` (sub), `boundary = 0x7FFFFFFF ^ (a>>s31)`,
  `result = r ^ ((r^boundary) & (ovf>>s31))`. 8/16-bit lanes have headroom in
  i32 and can use plain clamp.
- **PackedHalvingAddS16**: the whole-register `(a&b)+(((a^b)>>1)&M7)|(t&M8)`
  formula carries across lanes once sign bits are folded in (x64's `paddw`
  discards them). Compute the two 16-bit lanes independently. The U16 variant
  `(a&b)+(((a^b)>>1)&M7)` provably cannot carry; S8/U8 with the XOR trick are
  also safe.
- **UnsignedSaturation operand is signed** (frontend sign-extends): clamp low
  (`max(x,0)` signed) *before* clamping high.
- Packed saturating ops (QADD8/QADD16/UQADD8/...) do **not** set CPSR.Q in
  dynarmic (frontend never calls OrQFlag for them) — matches x64.
| 4 | NEON via v128 SIMD — **done** (the A64-only vector ops landed with phase 6; 64→32 saturating narrows vqmovn/vqmovun via per-lane i64 clamps, `tests/guest/neon6.c`). ARMv8 crypto (AES enc/dec/mc/imc, SHA256 h/h2/su0/su1) is done via one `crypto` thunk staging operands in `state->crypto_scratch` (`emit_wasm_crypto.cpp`), validated by `tests/guest/crypto.c` against FIPS-197 C.1 and SHA-256("abc")). Tranche 1/2: shuffle/zip/rev/ext, qadd/qsub 8/16, abd, qmovn, clz, vshl reg, vtbl/vtbx (`emit_wasm_vector.cpp`, `emit_wasm_vector2.cpp`, shared helpers in `emit_wasm_vector_util.h`). Tranche 3 (`emit_wasm_vector3.cpp`): qadd/qsub 32 via overflow-mask bitselect, qabs/qneg, vqshl reg/imm + vqshlu (barrel shifter + round-trip sat detect), vrshl, vqdmulh/vqrdmulh (ExtMul + truncate-shuffle), vmul.p8/vmull.p8 (8 masked-shift XOR terms), vrecpe/vrsqrte.u32 via `recip_est`/`rsqrt_est` thunks — validated by `tests/guest/neon3.c` vs `neon3_ref.c` (1472-entry mix log identical, checksum `c3cb8657`, both modes). Tranche 4: all 28 FPVector32 ops (`emit_wasm_vector_fp.cpp`) — arithmetic/compares map to direct f32x4 ops, MinNumeric/MaxNumeric via bitselect NaN chain, fused MulAdd32 via f64x2 promote/mul/add/demote, fixed-point vcvt via vectorized round-imm + `i32x4.trunc_sat_f32x4_s/u` (saturation matches ARM), vpadd.f32 via even/odd shuffles, vrecpe/vrsqrte/vrecps/vrsqrts.f32 + vcvt.f16 via per-lane `FP::` thunks folding exception bits into `state->fpsr` — validated by `tests/guest/neon4.c`/`neon5.c` (ARMv7 + ARMv8 forms incl. vfma/vfms, vrint*, vminnm/vmaxnm scalar+vector, vcvtn/a/p/m) vs C++ references (mix logs identical, both modes) |
| 6 | AArch64 (§13) — **done**: every A64 IR opcode emitted, AArch64 guests (`tests/guest64`) and CoreMark match native, differential fuzzer against Unicorn |
| 7 | Regions, fast dispatch, NZCV/check-bit locals, branch conditions from the last compare (§3.3, §3.4) — **done** |
| 5 | Performance — **mostly done**: fastmem page-table inline, LinkBlock/LinkBlockFast tail-call chaining, 8-entry RSB. Memory bench 104→548 Minstr/s (5.2x), call bench 109→250 Minstr/s. **Self-loop register caching + conditional-branch folding** (below): memory-loop bench ~450→1100–1500 Minstr/s (another ~3x). Done since: NZCV + ext-reg caching in loops, loop iteration budget (tier-up), flat memory window, per-range invalidation, Step(), FPSCR.QC. Remaining: async compile and release-grade profiling/performance coverage; multi-block regions and initial counters are implemented |

### Phase 4 pitfalls worth remembering

- **wasm `select` yields its FIRST-pushed operand when the condition is true**
  (V8-verified). `V128Bitselect(v1, v2, mask)` = v1 where mask is all-ones.
- **V8 f32x4 NaN rules on x86** (SSE lowering): arithmetic propagates the NaN
  of src2 when both are NaN (payload preserved, sNaN quieted) — the C++
  reference must resolve NaNs explicitly since gcc may swap commutative
  operands; `f32x4.min/max` CANONICALIZE any NaN to `0xFFC00000`/`0x7FC00000`,
  so plain vmin/vmax tests must use NaN-free pairs. `f32x4.nearest` etc. pass
  quiet NaNs through with payload intact (SSE4.1 `roundps`).
- **gas silently aliases `vcvtr.s32.f32 q, q` to the truncating `vcvt`** —
  VCVTR is VFP-scalar-only. ARMv8's `vcvtn/a/p/m` are the real vector
  rounding-mode conversions.
- Vector `vdiv.f32`/`vsqrt.f32` are A64-only; A32 NEON has neither.
- **FPSCR.QC** (bit 27 of `state->fpsr`) is set by every saturating NEON op (`EmitQcCheck` in `emit_wasm_vector_qc.cpp`): add/sub/abs/neg compare the saturated result against the wrapping result, doubling multiplies test INT_MIN×INT_MIN, narrows sign/zero-extend the narrowed half back and compare to the source, shifts reuse their saturation masks (vqshlu also flags negative inputs). Validated by `tests/guest/qc.c` vs `qc_ref.cpp` (117040-entry log incl. the QC bit, identical, all three memory modes). The 64-bit-lane vqadd/vqsub emitters are unreachable from the A32 frontend (it rejects VQADD.64) and are covered by an IR-level test instead (`tests/ir/sat64_ir.cpp`).
- `FPHalfToFixed{S,U}{16,32,64}` is implemented (fp_h2s thunk, then the f32 fixed path) but only A64 produces a U16 operand there, so no A32 guest test can reach it.

## 11. Loop optimizations (phase 5)

Two cooperating transforms make hot ARM loops run as a single wasm `loop`:

1. **Conditional-branch folding** (`A32AddressSpace::FoldConditionalBranch`).
   dynarmic's A32 frontend splits `subs; bne loop` into two blocks: the ALU
   body, and an *empty* block with entry condition NE whose terminal is the
   link to the loop head. When a block's terminal links to such a block, its
   terminal becomes `If(cond, taken, LinkBlock(fall-through))`. Every
   `cmp; bcc` pair across all guest code is now one block with both exits
   chained directly. Skipped when cycle counting is on (the two paths' cycle
   counts differ).
2. **Self-loop register caching** (`EmitBeginSelfLoop` / `EmitEndSelfLoop`,
   `emit_wasm_self_loop.cpp`; the same cache serves regions, §3.4).
   If a block's terminal tree links back to the block's own location (cond=AL,
   no cycle counting, no R15/BX writes in the body), the body+terminal is
   wrapped in a wasm `loop`; the back-edge becomes `br_if` on
   `halt_reason == 0` (a pending halt exits through the dispatcher at the loop
   head). Guest registers R0..R14 touched by the body live in wasm locals
   (`EmitContext::reg_cache`), loaded once before the loop. To keep the state
   struct coherent wherever guest state can be observed, `FuncBody` has
   `before_call` / `after_call` / `before_exit` hooks: written registers are
   flushed before every import call (except "pure" FP/crypto thunks) and before
   every `return` / `return_call_indirect`; all cached registers are reloaded
   after the call (an SVC callback may have changed them). Because the hooks
   live in the byte emitter, no individual opcode emitter can forget the flush.
   Guest memory thunks (plain and exclusive) are the exception: they skip the
   flush and reload and keep only the halt check, so memory callbacks see guest
   registers as of the last flush and must not change them -- the same
   guarantee upstream's GetSetElimination gives. The reload code on those cold
   slow paths had tripled region size and cost ~25% of CoreMark.
   NZCV is one more cache unit (an i32 local) whenever a cached block reads or
   writes it and it is not deferred (item 3); all accesses go through
   `EmitPushNzcv` / `EmitBeginNzcvStore` / `EmitEndNzcvStore`.

3. **Deferred NZCV in self-loops.** When the loop's flags come from a lone
   `Add32`/`Sub32` flag producer (`subs`/`adds`/`cmp`), the emitter records the
   operands and result in locals instead of computing/storing packed NZCV. The
   branch condition is computed straight from them (a `cmp r, r; bne` becomes a
   single wasm compare for integer conditions). Packed NZCV is materialized
   (guarded by a `valid` local) only on exits and before host calls, via the
   same flush hook as the register cache.

4. **Extended-register caching.** VFP/NEON accesses (`A32Get/SetExtendedRegister32/64`,
   `A32Get/SetVector`) are collected per distinct (state offset, size). A unit that
   overlaps no other access (S/D/Q views alias in the state struct) lives in an
   i32 / i64 / v128 local and goes through the same flush/reload hooks as the
   GPRs; aliased units stay in memory. The pi (VFP double) demo loop compiles to
   pure locals: `f64.convert`, `f64.div`, xor-negate, `f64.add`, one compare.

Not cached: FPSCR / FPSR, anything touched by an aliased S/D/Q view in the same
loop. A64 caches whole X/SP (i64) and V (v128) registers; a V register used only
through 64-bit scalar views is cached as an i64 with a dirty flag for the
zeroed upper half.

5. **Iteration budget (tier-up re-entry).** wasm has no OSR: V8 starts a block
   in baseline (Liftoff) code and a loop entered once keeps running that code
   even after the optimizing compile finishes in the background. Each self-loop
   therefore carries an i32 budget local (65536 back-edges); when spent, the
   block leaves through the dispatcher at the loop head and is called again,
   picking up the TurboFan code. Measured in cold Node runs: VFP double loop
   64 -> 27 ms, LCG 210 -> 138 ms; whole demo in Chrome 133 347 -> 204 ms
   (native C: ~190 ms). (A `compilationHints` section requesting eager TurboFan is
   also emitted for loop blocks; V8 only honors it behind
   `--experimental-wasm-compilation-hints`, so don't rely on it.)

Debug aids (Node): `DYN_DUMP_DIR=<dir>` writes every emitted block module;
`Module.dynarmicWasmStats` accumulates per-phase compile times
(`translateMs/optMs/emitMs` + engine `compileMs/instMs/tableMs`). Cold start:
the A32 decoder-table init costs a one-time ~39 ms on the first `Translate`;
steady-state per-block compile cost is ~130 µs (opt+emit+engine).

## 12. Browser example

`examples/browser/` is a complete browser deployment: `arm_web.cpp` (C API:
`arm_init(mode, arch)/arm_run/arm_ram/arm_console`, arch 0 = ARM32, 1 =
AArch64), `worker.js` (the emulator runs in a Web Worker to avoid main-thread blocking
and engine-specific synchronous compilation size limits), `index.html`,
a demo guest (LCG loop, prime sieve, VFP double kernel) built for both
architectures (the same C source builds natively to produce `expected.txt`),
all built
by the Emscripten CMake build into `examples/browser/dist`. `verify_chrome.js` drives headless Chrome over the DevTools
protocol and checks that the page reports `DONE PASS`.

Verified in Chrome 133 (headless, Linux), both guests matching the native
reference: ARM32 216 ms guest run time (31 blocks, 42 KB of wasm, 3.9 ms engine
compile+instantiate), AArch64 233 ms (50 blocks, 40 KB). The LCG and pi kernels
run at about native speed; CoreMark under Node is ~6 000 it/s on both
architectures vs ~37 000 natively. The whole JIT + host is a 2.2 MB `.wasm`.

Callbacks are resolved through each execution context's configuration;
multiple live JIT instances are supported. Profile publication still uses a
Module-global snapshot, which must not be interpreted as an aggregate of all
instances.


## 13. AArch64

The A64 frontend shares the emitter core (`EmitConfig::frontend` selects the
state layout and location-descriptor handling). A64-specific pieces:

- `a64_jitstate.h`, `a64_address_space.cpp` (translation, block cap via a
  `b #4` fed after 256 instructions, `ScalarLanePeephole`), `a64_interface.cpp`;
  `core.h` is the dispatcher template shared with A32.
- `emit_wasm_a64.cpp` (context: registers, PSTATE, system registers, check bit),
  `emit_wasm_a64_memory.cpp` (i64 virtual addresses; thunks, page table with
  mirroring, flat window; 128-bit accesses staged through `mem_scratch`),
  `thunks_a64.cpp`.
- `emit_wasm_vector_a64.cpp`, `emit_wasm_fp_a64.cpp`: the A64-only vector and
  FP ops. Rare FP ops go through one generic `fp_ref` thunk running upstream's
  `FP::` soft-float code; fused f64 multiply-add uses `fp_fma64`.
- FPCR mode bits are part of the location descriptor, so they are compile-time
  constants (`EmitContext::fpcr`).

Known deviations (both frontends): FPSR cumulative exception flags are not
produced by inline wasm float ops; NaN payloads may differ (V8 follows SSE
rules); FPDoubleToSingle with ties-away rounds ties-to-even. Inherited from
upstream's A32 frontend: VRECPS/VRSQRTS are emitted as the fused (AArch64)
step operations, while ARMv8's AArch32 versions are unfused (1 ulp off in
rare cases).

## 14. Testing

`tests/regress.sh` is the gate. Besides the guest programs (§12, `tests/guest`,
and the AArch64 builds of the same C in `tests/guest64`, built with `zig cc`),
it runs two differential fuzzers against [Unicorn](https://www.unicorn-engine.org/)
when `python3 -m pip install unicorn capstone` is available:

- `tests/a64/fuzz.py` with `tests/hosts/a64_fuzz.cpp`
- `tests/a32/fuzz.py` with `tests/hosts/a32_fuzz.cpp` (ARM or `--thumb`, IT
  blocks, VFP/NEON)

Each generates random instruction sequences and register/memory state, runs
them in both, and compares registers, flags, FP/SIMD registers and the data
page. `--loop N --branches P` wraps the sequence in a counted loop with
forward branches, which exercises regions, the register cache and the
terminal-condition shortcut. `--word` tests one encoding. NaN payloads and
cumulative FP exception bits are tolerated unless `--strict-nan` /
`--strict-fpsr` (`--strict-fpscr` for A32). Both harnesses were checked by
mutation (an injected emitter bug is caught immediately). The A32 fuzzer found
the ARMv7 NEON flush-to-zero behaviour (standard FPSCR), D-register NEON
writes clobbering the next register, the FPSCR NZCV location, and the
block-length stack overflow above.

Benchmarks: `tests/bench/coremark/run.sh <coremark checkout>` builds CoreMark
for ARM32, AArch64 and the host and runs the guests through the browser
example's `arm_jit.js` under Node; `examples/browser/bench.mjs` times the demo
kernels.

