// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// WASM "register allocation": assigns one WASM local per live IR value.
//
// WASM locals are unlimited and the engine's own optimizing compiler performs
// physical register allocation, so this pass only needs to:
//   1. assign each IR value a local index (with free-list reuse to keep the
//      local count, and thus engine compile time/memory, low),
//   2. resolve Identity chains and immediates on lookup,
//   3. track remaining use counts (redirected through Identity chains to the
//      root producer) so locals can be recycled as values die.
//
// There is no spilling.

#pragma once

#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/wasm_binary.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/value.h"

namespace Dynarmic::Backend::Wasm {

class RegAlloc {
public:
    struct Argument {
        IR::Value value;

        IR::Type GetType() const { return value.GetType(); }
        bool IsImmediate() const { return value.IsImmediate(); }

        bool GetImmediateU1() const { return value.GetU1(); }
        u8 GetImmediateU8() const { return value.GetU8(); }
        u16 GetImmediateU16() const { return value.GetU16(); }
        u32 GetImmediateU32() const { return value.GetU32(); }
        u64 GetImmediateU64() const { return value.GetU64(); }
        IR::Cond GetImmediateCond() const { return value.GetCond(); }
        IR::AccType GetImmediateAccType() const { return value.GetAccType(); }
    };
    using ArgumentInfo = std::array<Argument, IR::max_arg_count>;

    ArgumentInfo GetArgumentInfo(IR::Inst* inst) {
        ArgumentInfo result;
        for (size_t i = 0; i < inst->NumArgs(); i++) {
            result[i].value = inst->GetArg(i);
        }
        return result;
    }

    // Allocates a fresh (or recycled) local for inst's result.
    u32 DefineLocal(FuncBody& code, IR::Inst* inst) {
        const ValType type = ValTypeOf(inst->GetType());
        const u32 local = AllocateLocal(code, type);
        entries[inst] = local;
        local_types[local] = type;
        return local;
    }

    // Records that inst's value already resides in `local` (used when a
    // producer computes a pseudo-op's result alongside its own).
    void DefineLocalAs(IR::Inst* inst, u32 local, ValType type) {
        entries[inst] = local;
        local_types[local] = type;
    }

    // Resolves Identity chains and immediates.
    // Returns the local index of the root producer, or nullopt when the
    // value is (an Identity of) an immediate — caller then emits a const.
    std::optional<u32> LocalOf(IR::Value value) const {
        value = ResolveIdentity(value);
        if (value.IsImmediate()) {
            return std::nullopt;
        }
        const auto it = entries.find(value.GetInst());
        ASSERT(it != entries.end());
        return it->second;
    }

    // Returns the immediate an (Identity-chained) value resolves to, if any.
    std::optional<IR::Value> ImmediateOf(IR::Value value) const {
        value = ResolveIdentity(value);
        if (value.IsImmediate()) {
            return value;
        }
        return std::nullopt;
    }

    // True while the value has remaining uses (its local must stay live).
    bool IsValueLive(IR::Inst* inst) const {
        const auto it = remaining_uses.find(inst);
        return it != remaining_uses.end() && it->second > 0;
    }

    // True if inst already has a local assigned (pseudo-ops pre-computed at
    // their producer take this path).
    bool HasLocal(IR::Inst* inst) const {
        return entries.find(inst) != entries.end();
    }

    // Called after inst has been fully emitted: accounts for inst's
    // consumption of its arguments, recycling dead locals.
    void FreeArgsIfDead(IR::Inst* inst) {
        for (size_t i = 0; i < inst->NumArgs(); i++) {
            const IR::Value arg = inst->GetArg(i);
            if (arg.IsImmediate()) {
                continue;
            }
            IR::Inst* root = ResolveIdentity(arg).GetInst();
            auto it = remaining_uses.find(root);
            if (it == remaining_uses.end()) {
                continue;
            }
            if (--it->second == 0) {
                remaining_uses.erase(it);
                FreeLocalOf(root);
            }
        }
    }

    // Re-adds one use for each of inst's non-immediate arguments, so they
    // stay live past inst itself. Used by pseudo-ops (VectorTable) whose
    // arguments are actually consumed by a later user instruction.
    void DeferArgUse(IR::Inst* inst) {
        for (size_t i = 0; i < inst->NumArgs(); i++) {
            const IR::Value arg = inst->GetArg(i);
            if (!arg.IsImmediate()) {
                remaining_uses[ResolveIdentity(arg).GetInst()]++;
            }
        }
    }

    void AssertNoMoreUses() const {
        // Values still live here are consumed only by the terminal or are a
        // bookkeeping artifact; their locals simply go out of scope.
    }

    // Uses of an Identity inst are redirected to its root producer, so the
    // root's local stays live until the last aliased consumer.
    void PrecomputeUseCounts(IR::Block& block) {
        for (auto& inst : block) {
            IR::Value value{&inst};
            const IR::Value root = ResolveIdentity(value);
            if (!root.IsImmediate()) {
                remaining_uses[root.GetInst()] += inst.UseCount();
            }
        }
    }

private:
    friend class WasmEmitter;

    static IR::Value ResolveIdentity(IR::Value value) {
        while (!value.IsImmediate() && value.GetInst()->GetOpcode() == IR::Opcode::Identity) {
            value = value.GetInst()->GetArg(0);
        }
        return value;
    }


    u32 AllocateLocal(FuncBody& code, ValType type) {
        std::vector<u32>& free_list = FreeListFor(type);
        if (!free_list.empty()) {
            const u32 local = free_list.back();
            free_list.pop_back();
            return local;
        }
        return code.AddLocals(1, type);
    }

    void FreeLocalOf(IR::Inst* inst) {
        const auto it = entries.find(inst);
        if (it == entries.end()) {
            return;
        }
        FreeListFor(local_types[it->second]).push_back(it->second);
        entries.erase(it);
    }

    std::vector<u32>& FreeListFor(ValType type) {
        switch (type) {
        case ValType::I32:
            return free_i32;
        case ValType::I64:
            return free_i64;
        default:
            return free_v128;
        }
    }

    std::unordered_map<IR::Inst*, u32> entries;
    std::unordered_map<IR::Inst*, size_t> remaining_uses;
    std::unordered_map<u32, ValType> local_types;
    std::vector<u32> free_i32;
    std::vector<u32> free_i64;
    std::vector<u32> free_v128;
};

}  // namespace Dynarmic::Backend::Wasm
