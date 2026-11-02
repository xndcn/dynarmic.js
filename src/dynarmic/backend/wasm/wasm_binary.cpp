// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/wasm_binary.h"

#include <algorithm>
#include <cstring>

#include <mcl/assert.hpp>

namespace Dynarmic::Backend::Wasm {

void EncodeULEB(std::vector<u8>& out, u64 value) {
    do {
        u8 byte = value & 0x7F;
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        out.push_back(byte);
    } while (value != 0);
}

void EncodeSLEB(std::vector<u8>& out, s64 value) {
    bool more = true;
    while (more) {
        u8 byte = value & 0x7F;
        value >>= 7;
        const bool sign_bit = (byte & 0x40) != 0;
        if ((value == 0 && !sign_bit) || (value == -1 && sign_bit)) {
            more = false;
        } else {
            byte |= 0x80;
        }
        out.push_back(byte);
    }
}

namespace {

void EncodeSection(std::vector<u8>& out, u8 id, const std::vector<u8>& payload) {
    out.push_back(id);
    EncodeULEB(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

void EncodeName(std::vector<u8>& out, const std::string& name) {
    EncodeULEB(out, name.size());
    out.insert(out.end(), name.begin(), name.end());
}

void EncodeValTypes(std::vector<u8>& out, const std::vector<ValType>& types) {
    EncodeULEB(out, types.size());
    for (const ValType t : types) {
        out.push_back(static_cast<u8>(t));
    }
}

void EncodeLimits(std::vector<u8>& out, const Limits& limits) {
    u8 flags = 0;
    if (limits.max.has_value()) {
        flags |= 0x01;
    }
    if (limits.shared) {
        flags |= 0x02;
    }
    if (limits.memory64) {
        flags |= 0x04;
    }
    out.push_back(flags);
    EncodeULEB(out, limits.min);
    if (limits.max.has_value()) {
        EncodeULEB(out, *limits.max);
    }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// FuncBody
// ---------------------------------------------------------------------------

u32 FuncBody::AddLocals(u32 count, ValType type) {
    u32 total = param_count;
    for (const auto& [c, t] : local_runs) {
        total += c;
    }
    if (!local_runs.empty() && local_runs.back().second == type) {
        local_runs.back().first += count;
    } else {
        local_runs.emplace_back(count, type);
    }
    return total;
}

void FuncBody::Bytes(const void* data, size_t size) {
    const u8* p = static_cast<const u8*>(data);
    body.insert(body.end(), p, p + size);
}

void FuncBody::Raw32(u32 v) {
    for (int i = 0; i < 4; i++) {
        body.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

void FuncBody::Raw64(u64 v) {
    for (int i = 0; i < 8; i++) {
        body.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

void FuncBody::BrTable(const std::vector<u32>& targets, u32 default_target) {
    U8(Op::BrTable);
    ULEB(targets.size());
    for (const u32 t : targets) {
        ULEB(t);
    }
    ULEB(default_target);
}

void FuncBody::EmitBlockType(const BlockType& bt) {
    if (bt.results.empty()) {
        U8(0x40);
    } else {
        ASSERT_MSG(bt.results.size() == 1, "multi-value block types not yet supported");
        U8(static_cast<u8>(bt.results[0]));
    }
}

std::vector<u8> FuncBody::Serialize() const {
    std::vector<u8> out;
    EncodeULEB(out, local_runs.size());
    for (const auto& [count, type] : local_runs) {
        EncodeULEB(out, count);
        out.push_back(static_cast<u8>(type));
    }
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(Op::End);
    return out;
}

// ---------------------------------------------------------------------------
// ModuleBuilder
// ---------------------------------------------------------------------------

u32 ModuleBuilder::AddType(const FuncType& type) {
    const auto it = std::find(types.begin(), types.end(), type);
    if (it != types.end()) {
        return static_cast<u32>(it - types.begin());
    }
    types.push_back(type);
    return static_cast<u32>(types.size() - 1);
}

u32 ModuleBuilder::ImportFunc(std::string module, std::string name, u32 type_index) {
    func_imports.push_back({std::move(module), std::move(name), type_index});
    return num_imported_funcs++;
}

u32 ModuleBuilder::ImportMemory(std::string module, std::string name, const Limits& limits) {
    mem_imports.push_back({std::move(module), std::move(name), limits});
    return static_cast<u32>(mem_imports.size() - 1);
}

u32 ModuleBuilder::ImportTable(std::string module, std::string name, ValType elem_type, const Limits& limits) {
    table_imports.push_back({std::move(module), std::move(name), elem_type, limits});
    return static_cast<u32>(table_imports.size() - 1);
}

u32 ModuleBuilder::ImportGlobal(std::string module, std::string name, ValType type, bool mutable_) {
    global_imports.push_back({std::move(module), std::move(name), type, mutable_});
    return static_cast<u32>(global_imports.size() - 1);
}

u32 ModuleBuilder::AddFunction(u32 type_index) {
    funcs.push_back(type_index);
    codes.emplace_back();
    return num_imported_funcs + static_cast<u32>(funcs.size() - 1);
}

u32 ModuleBuilder::AddMemory(const Limits& limits) {
    memories.push_back(limits);
    return static_cast<u32>(mem_imports.size() + memories.size() - 1);
}

u32 ModuleBuilder::AddTable(ValType elem_type, const Limits& limits) {
    tables.emplace_back(elem_type, limits);
    return static_cast<u32>(table_imports.size() + tables.size() - 1);
}

u32 ModuleBuilder::AddGlobal(ValType type, bool mutable_, const std::vector<u8>& init_expr) {
    globals.push_back({type, mutable_, init_expr});
    return static_cast<u32>(global_imports.size() + globals.size() - 1);
}

void ModuleBuilder::AddExport(std::string name, ExternalKind kind, u32 index) {
    exports.push_back({std::move(name), kind, index});
}

void ModuleBuilder::DeclareFuncElem(u32 func_index) {
    declarative_elems.push_back(func_index);
}

void ModuleBuilder::AddActiveElem(u32 table_index, u32 offset, const std::vector<u32>& func_indices) {
    active_elems.push_back({table_index, offset, func_indices});
}

size_t ModuleBuilder::SetCode(u32 func_index, const FuncBody& body) {
    ASSERT(func_index >= num_imported_funcs);
    codes[func_index - num_imported_funcs].bytes = body.Serialize();
    return codes[func_index - num_imported_funcs].bytes.size();
}

std::vector<u8> ModuleBuilder::Finalize() const {
    ASSERT_MSG(mem_imports.size() + memories.size() <= 1, "WASM allows at most one memory");
    ASSERT_MSG(funcs.size() == codes.size(), "function/code count mismatch");

    std::vector<u8> out = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    // Type section (1)
    if (!types.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, types.size());
        for (const FuncType& type : types) {
            payload.push_back(0x60);
            EncodeValTypes(payload, type.params);
            EncodeValTypes(payload, type.results);
        }
        EncodeSection(out, 1, payload);
    }

    // Import section (2)
    if (!func_imports.empty() || !mem_imports.empty() || !table_imports.empty() || !global_imports.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, func_imports.size() + mem_imports.size() + table_imports.size() + global_imports.size());
        for (const FuncImport& imp : func_imports) {
            EncodeName(payload, imp.module);
            EncodeName(payload, imp.name);
            payload.push_back(static_cast<u8>(ExternalKind::Function));
            EncodeULEB(payload, imp.type);
        }
        for (const TableImport& imp : table_imports) {
            EncodeName(payload, imp.module);
            EncodeName(payload, imp.name);
            payload.push_back(static_cast<u8>(ExternalKind::Table));
            payload.push_back(static_cast<u8>(imp.elem));
            EncodeLimits(payload, imp.limits);
        }
        for (const MemImport& imp : mem_imports) {
            EncodeName(payload, imp.module);
            EncodeName(payload, imp.name);
            payload.push_back(static_cast<u8>(ExternalKind::Memory));
            EncodeLimits(payload, imp.limits);
        }
        for (const GlobalImport& imp : global_imports) {
            EncodeName(payload, imp.module);
            EncodeName(payload, imp.name);
            payload.push_back(static_cast<u8>(ExternalKind::Global));
            payload.push_back(static_cast<u8>(imp.type));
            payload.push_back(imp.mut ? 1 : 0);
        }
        EncodeSection(out, 2, payload);
    }

    // Function section (3)
    if (!funcs.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, funcs.size());
        for (const u32 type : funcs) {
            EncodeULEB(payload, type);
        }
        EncodeSection(out, 3, payload);
    }

    // Table section (4)
    if (!tables.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, tables.size());
        for (const auto& [elem, limits] : tables) {
            payload.push_back(static_cast<u8>(elem));
            EncodeLimits(payload, limits);
        }
        EncodeSection(out, 4, payload);
    }

    // Memory section (5)
    if (!memories.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, memories.size());
        for (const Limits& limits : memories) {
            EncodeLimits(payload, limits);
        }
        EncodeSection(out, 5, payload);
    }

    // Global section (6)
    if (!globals.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, globals.size());
        for (const GlobalDef& g : globals) {
            payload.push_back(static_cast<u8>(g.type));
            payload.push_back(g.mut ? 1 : 0);
            payload.insert(payload.end(), g.init.begin(), g.init.end());
        }
        EncodeSection(out, 6, payload);
    }

    // Export section (7)
    if (!exports.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, exports.size());
        for (const Export& e : exports) {
            EncodeName(payload, e.name);
            payload.push_back(static_cast<u8>(e.kind));
            EncodeULEB(payload, e.index);
        }
        EncodeSection(out, 7, payload);
    }

    // Element section (9)
    if (!declarative_elems.empty() || !active_elems.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, (declarative_elems.empty() ? 0 : 1) + active_elems.size());
        if (!declarative_elems.empty()) {
            // Segment kind 3: declarative, funcref, vec(funcidx)
            payload.push_back(0x03);
            payload.push_back(0x00);  // elemkind = funcref
            EncodeULEB(payload, declarative_elems.size());
            for (const u32 f : declarative_elems) {
                EncodeULEB(payload, f);
            }
        }
        for (const ActiveElem& elem : active_elems) {
            // Segment kind 0: active, table 0, offset expr, vec(funcidx)
            ASSERT(elem.table == 0);
            payload.push_back(0x00);
            payload.push_back(Op::I32Const);
            EncodeSLEB(payload, elem.offset);
            payload.push_back(Op::End);
            EncodeULEB(payload, elem.funcs.size());
            for (const u32 f : elem.funcs) {
                EncodeULEB(payload, f);
            }
        }
        EncodeSection(out, 9, payload);
    }

    // Compilation hints (custom section; must precede the code section).
    if (compilation_hint && !codes.empty()) {
        std::vector<u8> payload;
        EncodeName(payload, "compilationHints");
        EncodeULEB(payload, codes.size());
        for (size_t i = 0; i < codes.size(); i++) {
            payload.push_back(*compilation_hint);
        }
        EncodeSection(out, 0, payload);
    }

    // Code section (10)
    if (!codes.empty()) {
        std::vector<u8> payload;
        EncodeULEB(payload, codes.size());
        for (const CodeEntry& code : codes) {
            EncodeULEB(payload, code.bytes.size());
            payload.insert(payload.end(), code.bytes.begin(), code.bytes.end());
        }
        EncodeSection(out, 10, payload);
    }

    // Name section (custom, after the code section): function names subsection.
    if (!function_names.empty()) {
        std::vector<u8> sub;
        EncodeULEB(sub, function_names.size());
        for (const auto& [index, name] : function_names) {
            EncodeULEB(sub, index);
            EncodeName(sub, name);
        }
        std::vector<u8> payload;
        EncodeName(payload, "name");
        payload.push_back(1);  // function names
        EncodeULEB(payload, sub.size());
        payload.insert(payload.end(), sub.begin(), sub.end());
        EncodeSection(out, 0, payload);
    }

    return out;
}

}  // namespace Dynarmic::Backend::Wasm
