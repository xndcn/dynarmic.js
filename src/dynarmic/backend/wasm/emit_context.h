// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/backend/wasm/wasm_binary.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/value.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Wasm {

struct EmitConfig;

struct EmittedBlockInfo;

// Offsets of the state fields that frontend-independent emitters touch.
struct StateLayout {
    u32 nzcv = 0;            // packed NZCV, ARM order (bits 31..28)
    u32 fpsr = 0;            // cumulative FP exception bits + QC (bit 27)
    u32 crypto_scratch = 0;  // 4 x v128 staging area for the crypto thunk
};

struct EmitContext {
    // The IR block being emitted (changes per block when compiling a region).
    IR::Block* block;
    RegAlloc& reg_alloc;
    const EmitConfig& emit_conf;
    EmittedBlockInfo& ebi;

    ModuleBuilder& mod;
    FuncBody& code;

    // True when the module imports the main indirect function table (direct
    // block chaining / RSB speculation via return_call_indirect).
    bool table_imported = false;

    // Region compilation (EmitWASM with several IR blocks): the blocks are
    // laid out in one function as
    //     loop $dispatch
    //       block $B_{n-1} ... block $B_1  block $B_0
    //         br_table (local next) -> $B_0 .. $B_{n-1}
    //       end  <code of block 0>
    //       end  <code of block 1> ...
    // so a branch to block k is `br $B_k` while $B_k is open (k > current,
    // a forward branch), and `next := k; br $dispatch` otherwise. Guest
    // registers live in locals for the whole function (reg_cache).
    struct Region {
        std::vector<IR::Block*> blocks;
        std::unordered_map<u64, u32> index_of;  // location descriptor -> block index
        u32 entry = 0;           // index of the block the function is entered at
        u32 current = 0;
        u32 next = 0;            // i32 local selecting the br_table target
        u32 dispatch_depth = 0;  // FuncBody::Depth() inside the dispatch loop
        bool self_loop_open = false;  // the current block runs in its own loop (reg_cache.loop_depth)
        // Blocks that are return addresses of calls (PushRSB) made in the
        // region: returns (PopRSBHint / FastDispatchHint) try them first.
        std::vector<u32> return_sites;

        // Structured layout (emit_wasm_region.cpp): no dispatch loop; block k
        // is reached by `br` to its enclosing block label (forward) or loop
        // header (backward), whichever is open at the branch (0 = closed).
        bool structured = false;
        std::vector<u32> open_label_depth;
        std::vector<u32> open_loop_depth;

        bool CanBranchTo(u32 k) const {
            return !structured || open_label_depth[k] != 0 || open_loop_depth[k] != 0;
        }
        std::vector<u32> label_depth;  // FuncBody::Depth() inside $B_k

        bool Multi() const { return blocks.size() > 1; }
    } region{};

    // The blocks register caching covers: the region's, or the current block.
    std::vector<IR::Block*> CachedBlocks() const {
        return region.Multi() ? region.blocks : std::vector<IR::Block*>{block};
    }

    // Next chaining slot index (preorder over the terminals of all emitted
    // blocks, matching CountLinkBlockFastTerminals summed in block order).
    u32 next_link_slot = 0;

    // Filled in by EmitWASM from emit_conf.frontend.
    StateLayout state{};
    // FPCR / FPSCR mode bits the block was translated under (part of the
    // location descriptor, hence a compile-time constant).
    u32 fpcr = 0;

    // Gets (or creates on first use) the function import index for a thunk.
    u32 Import(const char* name, u32 type_index);

    // Gets (or creates) the type index for a signature.
    u32 Type(const FuncType& sig);

    std::unordered_map<std::string, u32> import_indices{};

    // Imports that neither read nor write guest registers nor run user
    // callbacks (pure FP / crypto helpers); calls to them need no register
    // flush/reload when registers are cached in locals.
    std::unordered_set<u32> pure_imports{};
    // Guest memory thunks: no register flush/reload either, only the halt
    // check afterwards (see IsMemoryImport).
    std::unordered_set<u32> memory_imports{};

    // Self-loop register caching (emit_wasm_self_loop.cpp). While `active`,
    // A32 guest registers R0..R14 flagged in `cached` live in the wasm local
    // `local[reg]` instead of the state struct (at gpr_base + 4 * reg), and
    // every `ext` unit lives in its local; `written` ones are stored back to
    // the state at every exit and before every call that can observe guest
    // state, and all cached registers are reloaded after such a call (a
    // callback may have changed them).
    struct RegCache {
        bool active = false;
        u32 loop_depth = 0;  // FuncBody::Depth() once the loop frame is open
        // Local: back-edges left before re-entering via the dispatcher (set to
        // 1 after a host call that requested a halt, so the next back-edge
        // leaves).
        u32 budget = 0;
        u32 gpr_base = 0;
        std::array<u32, 16> local{};
        std::array<bool, 16> cached{};
        std::array<bool, 16> written{};

        // Register units cached in locals, one per (state offset, size).
        // A32: VFP/NEON accesses that overlap no other unit (S/D/Q views
        // alias in the state struct, so aliased ones stay in memory).
        // A64: whole X / SP (8 bytes) and V (16 bytes) registers; W / S / D
        // views are derived from them. size 4 -> i32 local, 8 -> i64,
        // 16 -> v128.
        struct ExtUnit {
            u32 offset = 0;
            u32 size = 0;
            u32 local = 0;
            bool written = false;
            // A64 V register used only as a 64-bit scalar (see
            // CollectA64SelfLoopUnits): `local` is an i64 holding the low
            // half; `dirty` (i32 local) is set by writes, which zero the
            // upper half, so a flush knows to store that zero.
            bool scalar64 = false;
            u32 dirty = 0;
        };
        std::vector<ExtUnit> ext{};

        // Packed NZCV (state.nzcv) in the i32 local `nzcv_local` when some
        // cached block reads or writes it (never together with deferred
        // flags); stored back when `nzcv_written`.
        bool nzcv_cached = false;
        bool nzcv_written = false;
        u32 nzcv_local = 0;

        bool NzcvInLocal() const { return active && nzcv_cached; }

        const ExtUnit* FindExt(u32 offset, u32 size) const {
            if (!active) {
                return nullptr;
            }
            for (const auto& unit : ext) {
                if (unit.offset == offset && unit.size == size) {
                    return &unit;
                }
            }
            return nullptr;
        }
    } reg_cache{};
    // Optional cycle-counted A32 board region budget, committed before every
    // host call and exit so callbacks observe the same accumulated work.
    u32 cycle_local = 0;

    // Scalar64 register units: GetQ/GetD insts that emit nothing and are read
    // through their unit's i64 local by VectorGetElement64 (inst -> local),
    // and ZeroExtendLongToQuad insts elided because their only use stores
    // into such a unit.
    std::unordered_map<IR::Inst*, u32> scalar_alias{};
    std::unordered_set<IR::Inst*> elided_widen{};

    // The i64 local a (possibly Identity-wrapped) value aliases, if any.
    const u32* ScalarAliasOf(IR::Value value) const {
        while (!value.IsImmediate() && value.GetInst()->GetOpcode() == IR::Opcode::Identity) {
            value = value.GetInst()->GetArg(0);
        }
        if (value.IsImmediate() || scalar_alias.empty()) {
            return nullptr;
        }
        const auto it = scalar_alias.find(value.GetInst());
        return it == scalar_alias.end() ? nullptr : &it->second;
    }

    // Deferred NZCV for self-loops whose only flag producer is an Add/Sub
    // (32 or 64-bit) with constant carry-in (cmp/subs/cmn/adds): the loop
    // body just records the operands and result; the branch condition is
    // computed from them and the packed NZCV is written to the state struct
    // only when it can be observed (loop exits, calls into the host).
    struct DeferredFlags {
        bool active = false;
        bool is_sub = false;
        bool is64 = false;
        IR::Inst* producer = nullptr;  // the Add/Sub
        IR::Inst* nzcv = nullptr;      // its GetNZCVFromOp
        IR::Inst* setter = nullptr;    // the A32SetCpsrNZCV / A64SetNZCV consuming it
        u32 a = 0, b = 0, res = 0;     // operand / result locals (i32, or i64 if is64)
        u32 valid = 0;                 // set once the first iteration recorded flags
    } flags{};

    // Terminal-condition shortcut (FindTerminalFlags): the block's last flag
    // write is such an Add/Sub, so the terminal's If conditions are computed
    // from its operands (captured in a / b / res) instead of unpacking NZCV.
    // NZCV itself is still written normally; `active` from the setter on.
    DeferredFlags term_flags{};
    std::array<u32, 3> term_flag_locals32{};  // shared by all blocks
    std::array<u32, 3> term_flag_locals64{};

    // The check bit (SetCheckBit -> CheckBit terminal of the same block)
    // lives in an i32 local instead of ExecutionContext::check_bit.
    u32 check_bit_local = 0;
    u32 CheckBitLocal() {
        if (check_bit_local == 0) {
            check_bit_local = code.AddLocals(1, ValType::I32);
        }
        return check_bit_local;
    }

    // Temporaries: short-lived locals for within-one-instruction use.
    // Allocated via Temp(), recycled in bulk by FreeTemps() (the driver calls
    // it after every instruction; terminal emission may span several Temps).
    u32 Temp(ValType type);
    void FreeTemps();

    std::unordered_map<u32, std::vector<u32>> temp_free{};  // ValType -> free locals
    std::vector<std::pair<u32, ValType>> temp_live{};
};

// Push an IR value (const or local.get) onto the WASM stack.
void EmitPush(FuncBody& code, RegAlloc& ra, IR::Value value);
void EmitPush(FuncBody& code, RegAlloc& ra, const RegAlloc::Argument& arg);
void EmitPushConst(FuncBody& code, IR::Value imm);

// Pushes the linear-memory address of the guest FPSR word. FP reference
// thunks take it as their first argument (instead of the state pointer) and
// OR the exception bits they raise into it, so they work for either frontend.
inline void EmitPushFpsrAddr(EmitContext& ctx) {
    ctx.code.LocalGet(kParamState);
    ctx.code.I32Const(static_cast<s32>(ctx.state.fpsr));
    ctx.code.U8(Op::I32Add);
}

}  // namespace Dynarmic::Backend::Wasm
