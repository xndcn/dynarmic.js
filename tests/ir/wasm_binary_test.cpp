// Standalone validation of the WASM binary emitter (wasm_binary.h): writes
// basic.wasm, imports.wasm and numeric.wasm into <out_dir> (default cwd) for
// wasm_binary_test.mjs to instantiate and check.
#include <cstdio>
#include <fstream>

#include "dynarmic/backend/wasm/wasm_binary.h"

using namespace Dynarmic::Backend::Wasm;

static void WriteFile(const char* path, const std::vector<u8>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Module 1: exported memory + add(i32,i32)->i32 + store_load(i32)->i32
static std::vector<u8> BuildBasic() {
    ModuleBuilder m;
    const u32 add_ty = m.AddType({{ValType::I32, ValType::I32}, {ValType::I32}});
    const u32 sl_ty = m.AddType({{ValType::I32}, {ValType::I32}});

    const u32 add_fn = m.AddFunction(add_ty);
    {
        FuncBody b;
        b.LocalGet(0);
        b.LocalGet(1);
        b.U8(Op::I32Add);
        m.SetCode(add_fn, b);
    }
    const u32 sl_fn = m.AddFunction(sl_ty);
    {
        FuncBody b;
        b.I32Const(0);
        b.LocalGet(0);
        b.Store(Op::I32Store, 0);
        b.I32Const(0);
        b.Load(Op::I32Load, 0);
        m.SetCode(sl_fn, b);
    }
    m.AddMemory({1, 1});
    m.AddExport("mem", ExternalKind::Memory, 0);
    m.AddExport("add", ExternalKind::Function, add_fn);
    m.AddExport("store_load", ExternalKind::Function, sl_fn);
    return m.Finalize();
}

// Module 2: imports memory/table/function from env; uses call_indirect
static std::vector<u8> BuildImports() {
    ModuleBuilder m;
    const u32 void_ty = m.AddType({{}, {}});
    const u32 ii_ty = m.AddType({{ValType::I32}, {ValType::I32}});
    const u32 iii_ty = m.AddType({{ValType::I32, ValType::I32}, {ValType::I32}});

    const u32 host_fn = m.ImportFunc("env", "host_double", ii_ty);
    m.ImportMemory("env", "memory", {1, std::nullopt});
    m.ImportTable("env", "table", ValType::FuncRef, {1, 16});

    // local mul2(x) = x * 2, placed into imported table at index 0
    const u32 mul2_fn = m.AddFunction(ii_ty);
    {
        FuncBody b;
        b.LocalGet(0);
        b.I32Const(2);
        b.U8(Op::I32Mul);
        m.SetCode(mul2_fn, b);
    }
    // dispatch(x) = table[0](x) via call_indirect on ii_ty
    const u32 dispatch_fn = m.AddFunction(ii_ty);
    {
        FuncBody b;
        b.LocalGet(0);
        b.I32Const(0);
        b.CallIndirect(ii_ty, 0);
        m.SetCode(dispatch_fn, b);
    }
    // use_host(x) = host_double(x) + imported memory load at 0
    const u32 use_host_fn = m.AddFunction(ii_ty);
    {
        FuncBody b;
        b.LocalGet(0);
        b.Call(host_fn);
        b.I32Const(0);
        b.Load(Op::I32Load, 0);
        b.U8(Op::I32Add);
        m.SetCode(use_host_fn, b);
    }
    m.AddActiveElem(0, 0, {mul2_fn});
    m.AddExport("dispatch", ExternalKind::Function, dispatch_fn);
    m.AddExport("use_host", ExternalKind::Function, use_host_fn);
    return m.Finalize();
}

// Module 3: SLEB edge cases + i64/f64 ops + SIMD smoke
static std::vector<u8> BuildNumeric() {
    ModuleBuilder m;
    const u32 ty = m.AddType({{}, {ValType::I32}});
    const u32 fn = m.AddFunction(ty);
    FuncBody b;
    b.AddLocals(1, ValType::I64);
    // consts: -1, -64, 64, 0x7FFFFFFF, INT32_MIN (SLEB edge cases)
    b.I32Const(-1);
    b.I32Const(-64);
    b.U8(Op::I32Add);
    b.I32Const(64);
    b.U8(Op::I32Add);
    b.I32Const(0x7FFFFFFF);
    b.I32Const(-2147483647 - 1);
    b.U8(Op::I32Add);
    b.U8(Op::I32Add);
    // f64 roundtrip: f64(1.5) trunc -> 1
    b.F64Const(0x3FF8000000000000ULL);
    b.U8(Op::I32TruncF64S);
    b.U8(Op::I32Add);
    // SIMD: v128.const splat 1 as i32x4, extract lane 0
    b.Simd(Op::V128Const);
    for (int i = 0; i < 4; i++) {
        b.Raw32(1);
    }
    b.Simd(Op::I32x4ExtractLane);
    b.U8(0);
    b.U8(Op::I32Add);
    m.SetCode(fn, b);
    m.AddExport("run", ExternalKind::Function, fn);
    return m.Finalize();
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : ".";
    char path[512];
    std::snprintf(path, sizeof(path), "%s/basic.wasm", out);
    WriteFile(path, BuildBasic());
    std::snprintf(path, sizeof(path), "%s/imports.wasm", out);
    WriteFile(path, BuildImports());
    std::snprintf(path, sizeof(path), "%s/numeric.wasm", out);
    WriteFile(path, BuildNumeric());
    std::printf("wrote 3 modules to %s\n", out);
    return 0;
}
