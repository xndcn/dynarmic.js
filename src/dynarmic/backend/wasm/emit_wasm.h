// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/frontend.h"
#include "dynarmic/backend/wasm/region_diagnostics.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::IR {
class Block;
class Inst;
enum class Cond;
enum class Opcode;
}  // namespace Dynarmic::IR

namespace Dynarmic::A32 {
class Coprocessor;
}  // namespace Dynarmic::A32

namespace Dynarmic::A64 {
struct UserConfig;
}  // namespace Dynarmic::A64

namespace Dynarmic::Backend::Wasm {

struct BoundaryHookSnapshot {
    u32 instruction_hooks;
    const u8* hook_slots;
};

struct EmittedBlockInfo {
    std::vector<u8> module_bytes;
    size_t num_locals = 0;
    size_t entry_body_bytes = 0;
    // Additional descriptors accepted by the same generated function: selected
    // board-region blocks, or actual A64 fallback return sites when opted in.
    std::vector<u64> fallback_aliases;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    RegionDiagnosticLayout region_layout = RegionDiagnosticLayout::Single;
    RegionDiagnosticBlocks region_blocks;
#endif
};

struct EmitConfig {
    bool enable_cycle_counting;
    bool always_little_endian;

    // Inline fastmem: wasm address of the page table array (u32 entries,
    // indexed by vaddr >> 12), 0 disables the inline path and all accesses go
    // through host thunks. Page entries are wasm addresses of each page (null
    // entry = fall back to the thunk for this access).
    u32 page_table_base = 0;
    // false: access page_table[vaddr >> 12][vaddr & 0xFFF]
    // true:  access page_table[vaddr >> 12][vaddr]
    bool absolute_offset_page_table = false;
    // Masks out the first N bits in page table pointers (attributes packed
    // into the pointer).
    int page_table_pointer_mask_bits = 0;

    // Flat guest window (UserConfig::fastmem_pointer + a page table that maps
    // pages [0, flat_limit) contiguously from it): vaddr < flat_limit is
    // accessed as a single `load offset=flat_base` with no page-table lookup.
    // flat_limit == 0 disables it.
    u32 flat_base = 0;
    u32 flat_limit = 0;

    // Direct block chaining: wasm address of a zero-initialized u32 array with
    // one slot per LinkBlockFast terminal in the block (preorder over the
    // terminal tree; see CountLinkBlockFastTerminals). On a LinkBlockFast edge
    // the block tail-calls the table index stored in its slot, falling back to
    // the dispatcher (which patches the slot) when the slot is 0.
    // 0 disables chaining.
    u32 link_slots_base = 0;

    // Optional A32 cycle-counted region boundaries. Generated code checks the
    // execution budget, halt state, board flags and the next PC's conservative
    // instruction-hook slot before every in-region edge. A slow edge writes a
    // complete resumable location and returns to the native dispatcher.
    const BoundaryState* boundary_state = nullptr;

    // Optional native snapshot used to omit a static edge's hook guard when its
    // target cannot have a code/block hook. boundary_state remains the runtime
    // linear-memory address; this native pointer snapshot is only read during
    // emission. Opting in requires the caller to quiesce execution, invalidate
    // the whole JIT on every hook-state/slot mutation, and serialize mutation
    // with compilation and publication so no stale specialized code can run.
    // Nullopt preserves the fully dynamic guard.
    std::optional<BoundaryHookSnapshot> boundary_hook_snapshot;

    // A32 coprocessors, consulted at emit time for A32Coproc* ops (the
    // Compile* decision is baked into the generated code). Nullptr = every
    // coprocessor access raises UndefinedInstruction.
    const std::array<std::shared_ptr<A32::Coprocessor>, 16>* coprocessors = nullptr;

    Frontend frontend = Frontend::A32;

    // Experiment: allow an A64 irreducible-region module to be entered at its
    // original entry or one of its actual return sites, selected from state.pc.
    bool enable_a64_fallback_multi_entry = false;
    // Reuse every region block through a shared full-descriptor selector.
    bool enable_region_multi_entry = false;

    // Independent emission policies. All are opt-in and fixed per JIT.
    bool enable_shared_exit = false;     // A32/Thumb multi-entry fallback regions
    bool enable_constant_memory = false; // immediate A32 page-table addresses

    // A64 page table geometry (A64::UserConfig): the table has
    // 2^(page_table_address_space_bits - 12) entries; with silently_mirror the
    // upper vaddr bits are ignored, otherwise an out-of-range vaddr takes the
    // thunk path.
    u32 page_table_address_space_bits = 32;
    bool silently_mirror_page_table = false;

    // A64 system registers / hooks (CNTFRQ, CTR, DCZID, TPIDR pointers,
    // hook_isb), consulted at emit time. Required for Frontend::A64.
    const A64::UserConfig* a64_conf = nullptr;

    // Linear-memory address of the address space's fast dispatch table
    // (kFastDispatchEntries FastDispatchEntry); 0 = indirect branches always
    // return to the dispatcher.
    u32 fast_dispatch_base = 0;

    // Debug aid: name each module's function after the guest location
    // ("a64_<pc>" / "a32_<pc>") so engine profilers attribute time to guest code.
    bool function_names = false;

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    // Optional stable shared-memory counters. Null emits exactly the ordinary
    // production module; non-null instruments only taken external tail calls.
    RegionTransferRecord* transfer_record = nullptr;
#endif
};

struct EmitContext;

// Compiles one IR block into a self-contained WASM module (see abi.h for the
// block ABI).
EmittedBlockInfo EmitWASM(IR::Block block, const EmitConfig& emit_conf);
// Compiles a region -- several IR blocks, entered at blocks[0] -- into one
// module function: branches between the blocks stay inside the function and
// guest registers stay in wasm locals (see EmitContext::Region). Chaining
// slots are numbered over the blocks' terminals in order.
EmittedBlockInfo EmitWASM(std::vector<IR::Block> blocks, const EmitConfig& emit_conf, size_t entry_index = 0);

// Number of chainable link terminals in a terminal tree (preorder, matching
// emission order): every LinkBlockFast, plus every LinkBlock when
// count_link_block is set (ordinary cycle counting still returns to the
// dispatcher; boundary-aware A32 repeats its guard before chaining). The caller must allocate that many u32 link slots
// and pass their base address as EmitConfig::link_slots_base.
u32 CountLinkBlockFastTerminals(const IR::Terminal& terminal, bool count_link_block);

// Terminal feature detection for function-table imports. Boundary-aware RSB
// stays on the native dispatcher; FastDispatchHint has its own dynamic guard.
bool TerminalHasPopRSB(const IR::Terminal& terminal);
bool TerminalHasFastDispatch(const IR::Terminal& terminal);

template<IR::Opcode op>
void EmitIR(EmitContext& ctx, IR::Inst* inst);

// Emits code that evaluates an ARM condition onto the WASM stack (i32 0/1),
// reading the packed NZCV flags (both frontends keep them in ARM order, N..V
// at bits 31..28).
void EmitCond(EmitContext& ctx, IR::Cond cond);

// Packed NZCV access, from the register cache's local when NZCV is cached
// (EmitContext::RegCache::nzcv_cached) or else the state struct. A store is
// EmitBeginNzcvStore(); <push the new value>; EmitEndNzcvStore().
void EmitPushNzcv(EmitContext& ctx);
void EmitBeginNzcvStore(EmitContext& ctx);
void EmitEndNzcvStore(EmitContext& ctx);

// Emits the block-entry condition check: blocks whose entry_cond is not AL
// divert to their cond_fail location when the condition does not hold.
void EmitCondPrelude(EmitContext& ctx);

// Return helpers consume the ordinary block result / tail-call operands.
// Eligible regions route them through the optional shared coherence barrier.
void EmitReturn(EmitContext& ctx);
void EmitReturnCallIndirect(EmitContext& ctx, u32 type, u32 table = 0);

// Emits the block terminal (shared by both frontends).
void EmitTerminal(EmitContext& ctx);

// Frontend-specific pieces of terminal emission.
// Writes `location` back as the guest's next location (A32: R15 + upper
// descriptor; A64: PC).
void EmitWriteLocationDescriptor(EmitContext& ctx, IR::LocationDescriptor location);
// Pushes (i64) the location descriptor value of the current guest state, as
// used for RSB / fast-dispatch lookups.
void EmitPushCurrentLocationDescriptor(EmitContext& ctx);
bool IsSingleStepping(const EmitContext& ctx, IR::LocationDescriptor location);
IR::LocationDescriptor WithoutSingleStepping(const EmitContext& ctx, IR::LocationDescriptor location);
// Before an in-region branch from block `from` to block `to`: update the
// location state the target block relies on (everything but the PC).
void EmitRegionLocationTransition(EmitContext& ctx, const IR::Block& from, const IR::Block& to);
// Per-frontend implementations of the above (emit_wasm_a32.cpp / emit_wasm_a64.cpp).
void EmitA32WriteLocationDescriptor(EmitContext& ctx, IR::LocationDescriptor location);
void EmitA32PushCurrentLocationDescriptor(EmitContext& ctx);
void EmitA32RegionLocationTransition(EmitContext& ctx, IR::LocationDescriptor from, IR::LocationDescriptor to);
void EmitA64WriteLocationDescriptor(EmitContext& ctx, IR::LocationDescriptor location);
void EmitA64PushCurrentLocationDescriptor(EmitContext& ctx);
// Selects region.next from masked state.pc for an opt-in fallback region.
// Unknown PCs return halt bits 0 without entering the region.
void EmitA64FallbackEntrySelector(EmitContext& ctx);

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
// Diagnostic-only counters in the main module's shared linear memory. These
// helpers emit nothing when EmitConfig::transfer_record is null.
void EmitRegionTransferEntry(EmitContext& ctx);
void EmitRegionTransfer(EmitContext& ctx, RegionTransferExit& exit, u32 target_descriptor);
#endif

// FPMulAdd64 / FPMulSub64: addend +- op1 * op2 (IR operand order: addend,
// op1, op2), correctly rounded through the fp_fma64 thunk.
void EmitFPMulAdd64Fused(EmitContext& ctx, IR::Inst* inst, bool is_sub);

// Pushes the packed NZCV (bits 31..28) of the self-loop's deferred flags
// (EmitContext::DeferredFlags).
void EmitDeferredPackedNzcv(EmitContext& ctx);

// Terminal-condition shortcut (EmitContext::term_flags): analysis at block
// start, and the hook run after each instruction of the block is emitted.
void FindTerminalFlags(EmitContext& ctx);
void EmitCaptureTerminalFlags(EmitContext& ctx, IR::Inst* inst);

// Self-loop register caching: when the block's terminal links back to the
// block itself, wraps the body in a wasm `loop` with guest registers cached in
// locals (flushed/reloaded around anything that can observe guest state, see
// EmitContext::RegCache). Returns true if a loop was opened, in which case
// EmitEndSelfLoop must be called after the terminal has been emitted.
bool EmitBeginSelfLoop(EmitContext& ctx);
void EmitEndSelfLoop(EmitContext& ctx);
// Region compilation scaffolding (emit_wasm_self_loop.cpp): open the
// dispatch structure, start block `index`'s code, close it all.
void EmitBeginRegion(EmitContext& ctx);
void EmitRegionBlockStart(EmitContext& ctx, u32 index);
void EmitEndRegion(EmitContext& ctx);
// Structured region layout (emit_wasm_region.cpp): emits the whole region,
// calling emit_block(k) for each block, or returns false (nothing emitted)
// if its control flow is irreducible. Begin / End set up and tear down the
// register cache around it.
bool EmitStructuredRegion(EmitContext& ctx, const std::function<void(u32)>& emit_block,
                          std::vector<u8>* entry_reachable = nullptr);
void EmitCommitCycles(EmitContext& ctx);
void EmitChargeCycles(EmitContext& ctx, u64 cycles);
void EmitReloadCycles(EmitContext& ctx);
void EmitBeginStructuredRegion(EmitContext& ctx);
void EmitEndStructuredRegion(EmitContext& ctx);
// Within a region: opens a wasm loop around ctx.block if it links back to
// itself (returns whether it did); its self edges then branch to that loop.
bool EmitBeginRegionSelfLoop(EmitContext& ctx);
void EmitEndRegionSelfLoop(EmitContext& ctx);
// Writes deferred NZCV (if an iteration recorded it) into the NZCV local:
// on every edge out of a region self-loop block that defers its flags.
void EmitMaterializeDeferredFlags(EmitContext& ctx);
// Per-frontend part: fill ctx.reg_cache with the registers to cache
// (allocating their locals), scanning ctx.block (self-loop) or every block
// of ctx.region; false = the block cannot be a self-loop.
bool CollectA32RegisterUnits(EmitContext& ctx, bool region);
bool CollectA64RegisterUnits(EmitContext& ctx, bool region);

}  // namespace Dynarmic::Backend::Wasm
