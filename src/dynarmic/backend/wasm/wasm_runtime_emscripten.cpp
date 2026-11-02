// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Emscripten implementation of the WASM backend runtime: block modules are
// compiled and instantiated in JS against the main module's linear memory,
// and their entry functions are registered in the indirect function table.
// The table index is returned as a C function pointer (v86-style).

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

#include "dynarmic/backend/wasm/compile_stats.h"
#include "dynarmic/backend/wasm/wasm_runtime.h"

namespace Dynarmic::Backend::Wasm {

// Use Emscripten's shared slot allocator and mirror, but do not build its
// deduplication map by scanning the whole main-module table. Every emitted
// block defines its own entry function in a fresh instance. If the embedding
// later uses addFunction, its lazy scan sees these entries; subsequent inserts
// and releases keep that map consistent too.
EM_JS_DEPS(dynarmic_block_table, "$getEmptyTableSlot,$setWasmTableEntry,$getWasmTableEntry,$functionsInTableMap,$freeTableIndexes");

// Imports shared by every block module, built once. Thunks are the main
// module's own wasm exports, so calls from block code are direct wasm->wasm
// calls (no JS trampoline, no BigInt boxing of i64 arguments).
EM_JS(void, js_init_block_imports, (), {
    const e = {m : wasmMemory, t : wasmTable};
    const thunks = {
        read8 : _dynarmic_wasm_read8, read16 : _dynarmic_wasm_read16, read32 : _dynarmic_wasm_read32, read64 : _dynarmic_wasm_read64,
        write8 : _dynarmic_wasm_write8, write16 : _dynarmic_wasm_write16, write32 : _dynarmic_wasm_write32, write64 : _dynarmic_wasm_write64,
        xread8 : _dynarmic_wasm_xread8, xread16 : _dynarmic_wasm_xread16, xread32 : _dynarmic_wasm_xread32, xread64 : _dynarmic_wasm_xread64,
        xwrite8 : _dynarmic_wasm_xwrite8, xwrite16 : _dynarmic_wasm_xwrite16, xwrite32 : _dynarmic_wasm_xwrite32, xwrite64 : _dynarmic_wasm_xwrite64,
        xclear : _dynarmic_wasm_xclear, svc : _dynarmic_wasm_svc, raise : _dynarmic_wasm_raise, interp : _dynarmic_wasm_interp,
        dsb : _dynarmic_wasm_dsb, icache : _dynarmic_wasm_icache, coproc : _dynarmic_wasm_coproc_cb,
        fp_h2s : _dynarmic_wasm_fp_h2s, fp_s2h : _dynarmic_wasm_fp_s2h, fp_h2d : _dynarmic_wasm_fp_h2d, fp_d2h : _dynarmic_wasm_fp_d2h,
        recip_est : _dynarmic_wasm_recip_est, rsqrt_est : _dynarmic_wasm_rsqrt_est, crypto : _dynarmic_wasm_crypto,
        fp_vrecpe32 : _dynarmic_wasm_fp_vrecpe32, fp_vrsqrte32 : _dynarmic_wasm_fp_vrsqrte32,
        fp_vrecps32 : _dynarmic_wasm_fp_vrecps32, fp_vrsqrts32 : _dynarmic_wasm_fp_vrsqrts32,
        fp_ref : _dynarmic_wasm_fp_ref, fp_fma64 : _dynarmic_wasm_fp_fma64,
        a64_read8 : _dynarmic_wasm_a64_read8, a64_read16 : _dynarmic_wasm_a64_read16, a64_read32 : _dynarmic_wasm_a64_read32,
        a64_read64 : _dynarmic_wasm_a64_read64, a64_read128 : _dynarmic_wasm_a64_read128,
        a64_write8 : _dynarmic_wasm_a64_write8, a64_write16 : _dynarmic_wasm_a64_write16, a64_write32 : _dynarmic_wasm_a64_write32,
        a64_write64 : _dynarmic_wasm_a64_write64, a64_write128 : _dynarmic_wasm_a64_write128,
        a64_xread8 : _dynarmic_wasm_a64_xread8, a64_xread16 : _dynarmic_wasm_a64_xread16, a64_xread32 : _dynarmic_wasm_a64_xread32,
        a64_xread64 : _dynarmic_wasm_a64_xread64, a64_xread128 : _dynarmic_wasm_a64_xread128,
        a64_xwrite8 : _dynarmic_wasm_a64_xwrite8, a64_xwrite16 : _dynarmic_wasm_a64_xwrite16, a64_xwrite32 : _dynarmic_wasm_a64_xwrite32,
        a64_xwrite64 : _dynarmic_wasm_a64_xwrite64, a64_xwrite128 : _dynarmic_wasm_a64_xwrite128,
        a64_xclear : _dynarmic_wasm_a64_xclear, a64_svc : _dynarmic_wasm_a64_svc, a64_raise : _dynarmic_wasm_a64_raise,
        a64_interp : _dynarmic_wasm_a64_interp, a64_dc : _dynarmic_wasm_a64_dc, a64_ic : _dynarmic_wasm_a64_ic,
        a64_isb : _dynarmic_wasm_a64_isb, a64_cntpct : _dynarmic_wasm_a64_cntpct,
    };
    Object.assign(e, thunks);
    Module.dynarmicBlockImports = {e : e};
});

EM_JS(uptr, js_instantiate_block, (const u8* bytes, u32 size, u8* stats_ptr), {
    const code = HEAPU8.slice(bytes, bytes + size);
    const imports = Module.dynarmicBlockImports;
    // Debug aid (Node only): DYN_DUMP_DIR=<dir> writes every emitted block module.
    if (typeof process !== 'undefined' && process.env && process.env.DYN_DUMP_DIR) {
        Module.dynarmicDumpCount = (Module.dynarmicDumpCount || 0) + 1;
        require('fs').writeFileSync(process.env.DYN_DUMP_DIR + '/block_' + Module.dynarmicDumpCount + '.wasm', code);
    }
    const t0 = performance.now();
    const mod = new WebAssembly.Module(code);
    const t1 = performance.now();
    const inst = new WebAssembly.Instance(mod, imports);
    const t2 = performance.now();
    const entry = inst.exports.f;
    const fn = getEmptyTableSlot();
    setWasmTableEntry(fn, entry);
    if (functionsInTableMap) functionsInTableMap.set(entry, fn);
    const t3 = performance.now();
    // Compile accounting is written to the owning Jit's POD snapshot. The
    // snapshot is published after Run/Step, so another live Jit cannot mix
    // its counters into this one.
    if (stats_ptr) {
        const stats = stats_ptr >> 3;
        HEAPF64[stats + 0] += 1;
        HEAPF64[stats + 1] += code.length;
        HEAPF64[stats + 5] += t1 - t0;
        HEAPF64[stats + 6] += t2 - t1;
        HEAPF64[stats + 7] += t3 - t2;
    }
    return fn;
});

EM_JS(void, js_release_block, (uptr fn), {
    if (functionsInTableMap) functionsInTableMap.delete(getWasmTableEntry(fn));
    setWasmTableEntry(fn, null);
    freeTableIndexes.push(fn);
});

BlockFunc InstantiateBlock(const u8* bytes, size_t size, CompileStats* stats) {
    static const bool imports_ready = (js_init_block_imports(), true);
    (void)imports_ready;
    const uptr index = js_instantiate_block(bytes, static_cast<u32>(size), reinterpret_cast<u8*>(stats));
    return reinterpret_cast<BlockFunc>(index);
}

void ReleaseBlock(BlockFunc fn) {
    js_release_block(reinterpret_cast<uptr>(fn));
}

}  // namespace Dynarmic::Backend::Wasm

#endif  // __EMSCRIPTEN__
