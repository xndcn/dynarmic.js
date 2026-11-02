// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// A64 context get/set opcodes, system registers / callbacks, and location
// write-back.

#include <array>
#include <vector>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

u32 RegOffset(A64::Reg reg) {
    const size_t index = static_cast<size_t>(reg);
    ASSERT(index < 31);
    return offsetof(A64JitState, reg) + sizeof(u64) * index;
}

u32 VecOffset(A64::Vec vec) {
    return offsetof(A64JitState, vec) + 2 * sizeof(u64) * static_cast<size_t>(vec);
}

const A64::UserConfig& Conf(const EmitContext& ctx) {
    ASSERT(ctx.emit_conf.a64_conf != nullptr);
    return *ctx.emit_conf.a64_conf;
}

void EmitV128LoadAt(EmitContext& ctx, u8 simd_op, u32 offset) {
    ctx.code.LocalGet(kParamState);
    ctx.code.Simd(simd_op);
    ctx.code.ULEB(0);  // align
    ctx.code.ULEB(offset);
}

void EmitV128StoreTop(EmitContext& ctx, u32 offset) {
    ctx.code.Simd(Op::V128Store);
    ctx.code.ULEB(0);  // align
    ctx.code.ULEB(offset);
}

// Self-loop register cache unit holding the register at `offset`, or nullptr.
const EmitContext::RegCache::ExtUnit* CachedUnit(const EmitContext& ctx, u32 offset, u32 size) {
    return ctx.reg_cache.FindExt(offset, size);
}

constexpr std::array<u8, 16> kLow32Mask = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Calls a (state, ectx[, args pushed by `push_args`]) thunk.
template<typename F>
void EmitThunkCall(EmitContext& ctx, const char* name, const FuncType& sig, F&& push_args) {
    const u32 fn = ctx.Import(name, ctx.Type(sig));
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    push_args();
    ctx.code.Call(fn);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Location write-back (used by the shared terminal emitter)
// ---------------------------------------------------------------------------

void EmitA64WriteLocationDescriptor(EmitContext& ctx, IR::LocationDescriptor location) {
    // The FPCR part of the descriptor only changes through SetFPCR, which
    // writes state->fpcr itself; only the PC needs writing back.
    ctx.code.LocalGet(kParamState);
    ctx.code.I64Const(static_cast<s64>(A64::LocationDescriptor{location}.PC()));
    ctx.code.Store(Op::I64Store, offsetof(A64JitState, pc), 3);
}

// (pc & pc_mask) | (fpcr & fpcr_mask) << fpcr_shift   (A64JitState::GetLocationDescriptor)
void EmitA64PushCurrentLocationDescriptor(EmitContext& ctx) {
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I64Load, offsetof(A64JitState, pc), 3);
    ctx.code.I64Const(static_cast<s64>(A64::LocationDescriptor::pc_mask));
    ctx.code.U8(Op::I64And);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A64JitState, fpcr), 2);
    ctx.code.I32Const(static_cast<s32>(A64::LocationDescriptor::fpcr_mask));
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.I64Const(static_cast<s64>(A64::LocationDescriptor::fpcr_shift));
    ctx.code.U8(Op::I64Shl);
    ctx.code.U8(Op::I64Or);
}

void EmitA64FallbackEntrySelector(EmitContext& ctx) {
    auto& region = ctx.region;
    const u32 current = ctx.code.AddLocals(1, ValType::I64);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I64Load, offsetof(A64JitState, pc), 3);
    ctx.code.I64Const(static_cast<s64>(A64::LocationDescriptor::pc_mask));
    ctx.code.U8(Op::I64And);
    ctx.code.LocalSet(current);

    ctx.code.I32Const(-1);
    ctx.code.LocalSet(region.next);
    const auto select = [&](u32 index) {
        const u64 raw_pc = region.blocks[index]->Location().Value() & A64::LocationDescriptor::pc_mask;
        ctx.code.LocalGet(current);
        ctx.code.I64Const(static_cast<s64>(raw_pc));
        ctx.code.U8(Op::I64Eq);
        ctx.code.If();
        ctx.code.I32Const(static_cast<s32>(index));
        ctx.code.LocalSet(region.next);
        ctx.code.End();
    };
    select(region.entry);
    for (const u32 site : region.return_sites) {
        if (site != region.entry) {
            select(site);
        }
    }

    // Fail closed before SetupRegisterCache and before any block can observe
    // or modify guest state, the RSB, or link / fast-dispatch state.
    ctx.code.LocalGet(region.next);
    ctx.code.I32Const(-1);
    ctx.code.U8(Op::I32Eq);
    ctx.code.If();
    ctx.code.I32Const(0);
    ctx.code.Return();
    ctx.code.End();
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A64SetCheckBit>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ctx.CheckBitLocal());
}

template<>
void EmitIR<IR::Opcode::A64GetCFlag>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPushNzcv(ctx);
    ctx.code.I32Const(29);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetNZCVRaw>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPushNzcv(ctx);
    ctx.code.LocalSet(dst);
}

// Both the raw u32 and the IR NZCV value are ARM-order packed flags.
static void EmitSetNzcv(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitBeginNzcvStore(ctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    EmitEndNzcvStore(ctx);
}

template<>
void EmitIR<IR::Opcode::A64SetNZCVRaw>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetNzcv(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64SetNZCV>(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.flags.active && inst == ctx.flags.setter) {
        return;  // deferred: materialized when observable (see emit_wasm_self_loop.cpp)
    }
    EmitSetNzcv(ctx, inst);
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A64GetW>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 offset = RegOffset(args[0].value.GetA64RegRef());
    if (const auto* unit = CachedUnit(ctx, offset, 8)) {
        ctx.code.LocalGet(unit->local);
        ctx.code.U8(Op::I32WrapI64);
    } else {
        ctx.code.LocalGet(kParamState);
        ctx.code.Load(Op::I32Load, offset, 2);
    }
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetX>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 offset = RegOffset(args[0].value.GetA64RegRef());
    if (const auto* unit = CachedUnit(ctx, offset, 8)) {
        ctx.code.LocalGet(unit->local);
    } else {
        ctx.code.LocalGet(kParamState);
        ctx.code.Load(Op::I64Load, offset, 3);
    }
    ctx.code.LocalSet(dst);
}

// GetS / GetD zero the bits above the scalar (matches the x64 backend).
// size: 4 (S), 8 (D) or 16 (Q) bytes of the vector register, upper bits zero.
static void EmitGetVec(EmitContext& ctx, IR::Inst* inst, u32 size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 offset = VecOffset(args[0].value.GetA64VecRef());
    const auto* unit = CachedUnit(ctx, offset, 16);
    if (unit && unit->scalar64) {
        // Every use reads lane 0 through the unit's i64 local.
        ctx.scalar_alias[inst] = unit->local;
        return;
    }
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    if (unit) {
        ctx.code.LocalGet(unit->local);
        if (size != 16) {
            VConst(ctx, size == 4 ? kLow32Mask : kLow64Mask);
            ctx.code.Simd(Op::V128And);
        }
    } else {
        EmitV128LoadAt(ctx, size == 4 ? Op::V128Load32Zero : size == 8 ? Op::V128Load64Zero : Op::V128Load, offset);
    }
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetS>(EmitContext& ctx, IR::Inst* inst) {
    EmitGetVec(ctx, inst, 4);
}

template<>
void EmitIR<IR::Opcode::A64GetD>(EmitContext& ctx, IR::Inst* inst) {
    EmitGetVec(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::A64GetQ>(EmitContext& ctx, IR::Inst* inst) {
    EmitGetVec(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::A64GetSP>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    if (const auto* unit = CachedUnit(ctx, offsetof(A64JitState, sp), 8)) {
        ctx.code.LocalGet(unit->local);
    } else {
        ctx.code.LocalGet(kParamState);
        ctx.code.Load(Op::I64Load, offsetof(A64JitState, sp), 3);
    }
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetFPCR>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A64JitState, fpcr), 2);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetFPSR>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A64JitState, fpsr), 2);
    ctx.code.LocalSet(dst);
}

// Writes an i64 GPR / SP value (pushed by `push`) to its cache unit or the state.
template<typename F>
static void EmitSetGpr64(EmitContext& ctx, u32 offset, F&& push) {
    if (const auto* unit = CachedUnit(ctx, offset, 8)) {
        push();
        ctx.code.LocalSet(unit->local);
        return;
    }
    ctx.code.LocalGet(kParamState);
    push();
    ctx.code.Store(Op::I64Store, offset, 3);
}

// Writes to W registers zero the upper half of the X register.
template<>
void EmitIR<IR::Opcode::A64SetW>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitSetGpr64(ctx, RegOffset(args[0].value.GetA64RegRef()), [&] {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.U8(Op::I64ExtendI32U);
    });
}

template<>
void EmitIR<IR::Opcode::A64SetX>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitSetGpr64(ctx, RegOffset(args[0].value.GetA64RegRef()), [&] {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    });
}

// SetS / SetD store the scalar with the rest of the vector register zeroed.
static void EmitSetVec(EmitContext& ctx, IR::Inst* inst, u32 size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 offset = VecOffset(args[0].value.GetA64VecRef());
    const auto* unit = CachedUnit(ctx, offset, 16);
    if (unit && unit->scalar64) {
        // The value is ZeroExtendLongToQuad(x) (elided): store x.
        const IR::Inst* widen = args[1].value.GetInst();
        EmitPush(ctx.code, ctx.reg_alloc, widen->GetArg(0));
        ctx.code.LocalSet(unit->local);
        ctx.code.I32Const(1);
        ctx.code.LocalSet(unit->dirty);
        return;
    }
    if (!unit) {
        ctx.code.LocalGet(kParamState);
    }
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    if (size != 16) {
        VConst(ctx, size == 4 ? kLow32Mask : kLow64Mask);
        ctx.code.Simd(Op::V128And);
    }
    if (unit) {
        ctx.code.LocalSet(unit->local);
    } else {
        EmitV128StoreTop(ctx, offset);
    }
}

template<>
void EmitIR<IR::Opcode::A64SetS>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetVec(ctx, inst, 4);
}

template<>
void EmitIR<IR::Opcode::A64SetD>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetVec(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::A64SetQ>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetVec(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::A64SetSP>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitSetGpr64(ctx, offsetof(A64JitState, sp), [&] {
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    });
}

template<>
void EmitIR<IR::Opcode::A64SetFPCR>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Store(Op::I32Store, offsetof(A64JitState, fpcr), 2);
}

template<>
void EmitIR<IR::Opcode::A64SetFPSR>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Store(Op::I32Store, offsetof(A64JitState, fpsr), 2);
}

template<>
void EmitIR<IR::Opcode::A64SetPC>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Store(Op::I64Store, offsetof(A64JitState, pc), 3);
}

// ---------------------------------------------------------------------------
// Exceptions, barriers, cache maintenance
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A64CallSupervisor>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitThunkCall(ctx, kImportA64CallSupervisor, {{ValType::I32, ValType::I32, ValType::I32}, {}}, [&] {
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    });
}

template<>
void EmitIR<IR::Opcode::A64ExceptionRaised>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitThunkCall(ctx, kImportA64ExceptionRaised, {{ValType::I32, ValType::I32, ValType::I64, ValType::I64}, {}}, [&] {
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    });
}

template<>
void EmitIR<IR::Opcode::A64DataCacheOperationRaised>(EmitContext& ctx, IR::Inst* inst) {
    // Only reached with hook_data_cache_operations (A64CallbackConfigPass
    // removes or lowers these otherwise). args: location, op, value.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitThunkCall(ctx, kImportA64DataCacheOperation, {{ValType::I32, ValType::I32, ValType::I64, ValType::I64}, {}}, [&] {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    });
}

template<>
void EmitIR<IR::Opcode::A64InstructionCacheOperationRaised>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitThunkCall(ctx, kImportA64InstructionCacheOperation, {{ValType::I32, ValType::I32, ValType::I64, ValType::I64}, {}}, [&] {
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    });
}

template<>
void EmitIR<IR::Opcode::A64DataSynchronizationBarrier>(EmitContext&, IR::Inst*) {
    // Single-threaded WASM: no-op.
}

template<>
void EmitIR<IR::Opcode::A64DataMemoryBarrier>(EmitContext&, IR::Inst*) {
    // Single-threaded WASM: no-op.
}

template<>
void EmitIR<IR::Opcode::A64InstructionSynchronizationBarrier>(EmitContext& ctx, IR::Inst*) {
    if (!Conf(ctx).hook_isb) {
        return;
    }
    EmitThunkCall(ctx, kImportA64InstructionSynchronizationBarrier, {{ValType::I32, ValType::I32}, {}}, [] {});
}

// ---------------------------------------------------------------------------
// System registers
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A64GetCNTFRQ>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.I32Const(static_cast<s32>(Conf(ctx).cntfrq_el0));
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetCNTPCT>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitThunkCall(ctx, kImportA64GetCNTPCT, {{ValType::I32, ValType::I32}, {ValType::I64}}, [] {});
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetCTR>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.I32Const(static_cast<s32>(Conf(ctx).ctr_el0));
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetDCZID>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.I32Const(static_cast<s32>(Conf(ctx).dczid_el0));
    ctx.code.LocalSet(dst);
}

// TPIDR_EL0 / TPIDRRO_EL0 live in host memory (UserConfig pointers, i.e.
// linear-memory addresses under wasm32); a null pointer reads as 0.
static void EmitLoadHostU64(EmitContext& ctx, IR::Inst* inst, const void* ptr) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    if (ptr) {
        ctx.code.I32Const(static_cast<s32>(reinterpret_cast<uptr>(ptr)));
        ctx.code.Load(Op::I64Load, 0, 3);
    } else {
        ctx.code.I64Const(0);
    }
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A64GetTPIDR>(EmitContext& ctx, IR::Inst* inst) {
    EmitLoadHostU64(ctx, inst, Conf(ctx).tpidr_el0);
}

template<>
void EmitIR<IR::Opcode::A64GetTPIDRRO>(EmitContext& ctx, IR::Inst* inst) {
    EmitLoadHostU64(ctx, inst, Conf(ctx).tpidrro_el0);
}

template<>
void EmitIR<IR::Opcode::A64SetTPIDR>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (const u64* ptr = Conf(ctx).tpidr_el0) {
        ctx.code.I32Const(static_cast<s32>(reinterpret_cast<uptr>(ptr)));
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.Store(Op::I64Store, 0, 3);
    }
}

// ---------------------------------------------------------------------------
// Self-loop register caching (see emit_wasm_self_loop.cpp)
// ---------------------------------------------------------------------------

bool CollectA64RegisterUnits(EmitContext& ctx, bool region) {
    struct Unit {
        u32 offset, size;
        bool written;
        bool scalar64 = true;  // V registers: every access has the 64-bit scalar shape
    };
    std::vector<Unit> units;
    auto note = [&](u32 offset, u32 size, bool written) -> Unit& {
        for (auto& u : units) {
            if (u.offset == offset) {
                u.written |= written;
                return u;
            }
        }
        return units.emplace_back(Unit{offset, size, written});
    };
    // Scalar shape: reads (GetQ / GetD) used only by VectorGetElement64(_, 0)
    // (through Identity chains); writes (SetQ / SetD) of a
    // ZeroExtendLongToQuad used only by that write.
    std::vector<std::pair<u32, IR::Inst*>> widens;  // (register offset, widening)
    auto reads_as_scalar64 = [&](IR::Block& block, IR::Inst& get) {
        std::vector<IR::Inst*> values{&get};
        for (size_t i = 0; i < values.size(); i++) {
            size_t uses = 0;
            for (auto& user : block) {
                for (size_t a = 0; a < user.NumArgs(); a++) {
                    const IR::Value arg = user.GetArg(a);
                    if (arg.IsImmediate() || arg.GetInst() != values[i]) {
                        continue;
                    }
                    uses++;
                    if (user.GetOpcode() == IR::Opcode::Identity) {
                        values.push_back(&user);
                    } else if (user.GetOpcode() != IR::Opcode::VectorGetElement64 || a != 0 || !user.GetArg(1).IsImmediate() || user.GetArg(1).GetU8() != 0) {
                        return false;
                    }
                }
            }
            if (uses != values[i]->UseCount()) {
                return false;
            }
        }
        return true;
    };
    auto writes_scalar64 = [&](IR::Inst& set) {
        const IR::Value value = set.GetArg(1);
        if (value.IsImmediate() || value.GetInst()->GetOpcode() != IR::Opcode::ZeroExtendLongToQuad || value.GetInst()->UseCount() != 1) {
            return false;
        }
        widens.emplace_back(VecOffset(set.GetArg(0).GetA64VecRef()), value.GetInst());
        return true;
    };
    for (IR::Block* block : ctx.CachedBlocks()) {
    for (auto& inst : *block) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64GetW:
        case IR::Opcode::A64GetX:
            note(RegOffset(inst.GetArg(0).GetA64RegRef()), 8, false);
            break;
        case IR::Opcode::A64SetW:
        case IR::Opcode::A64SetX:
            note(RegOffset(inst.GetArg(0).GetA64RegRef()), 8, true);
            break;
        case IR::Opcode::A64GetSP:
            note(offsetof(A64JitState, sp), 8, false);
            break;
        case IR::Opcode::A64SetSP:
            note(offsetof(A64JitState, sp), 8, true);
            break;
        case IR::Opcode::A64GetS:
        case IR::Opcode::A64GetD:
        case IR::Opcode::A64GetQ: {
            Unit& u = note(VecOffset(inst.GetArg(0).GetA64VecRef()), 16, false);
            u.scalar64 &= inst.GetOpcode() != IR::Opcode::A64GetS && reads_as_scalar64(*block, inst);
            break;
        }
        case IR::Opcode::A64SetS:
        case IR::Opcode::A64SetD:
        case IR::Opcode::A64SetQ: {
            Unit& u = note(VecOffset(inst.GetArg(0).GetA64VecRef()), 16, true);
            u.scalar64 &= inst.GetOpcode() != IR::Opcode::A64SetS && writes_scalar64(inst);
            break;
        }
        case IR::Opcode::A64SetPC:
            // The PC is rewritten mid-body (SVC, exceptions): not a plain loop.
            // (A region block like that just exits through its terminal.)
            if (!region) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    }
    auto& rc = ctx.reg_cache;
    for (const auto& u : units) {
        const bool scalar64 = u.size == 16 && u.scalar64;
        EmitContext::RegCache::ExtUnit unit{u.offset, u.size, ctx.code.AddLocals(1, u.size == 16 && !scalar64 ? ValType::V128 : ValType::I64), u.written};
        unit.scalar64 = scalar64;
        if (scalar64 && u.written) {
            unit.dirty = ctx.code.AddLocals(1, ValType::I32);
            ctx.code.I32Const(0);
            ctx.code.LocalSet(unit.dirty);
        }
        rc.ext.push_back(unit);
    }
    // Widenings feeding scalar64 writes are elided (the write stores the
    // 64-bit operand directly).
    for (const auto& [offset, widen] : widens) {
        for (const auto& unit : rc.ext) {
            if (unit.offset == offset && unit.scalar64) {
                ctx.elided_widen.insert(widen);
            }
        }
    }
    return true;
}

}  // namespace Dynarmic::Backend::Wasm
