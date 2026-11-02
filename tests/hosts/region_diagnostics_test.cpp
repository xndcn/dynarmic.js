#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

#include <emscripten.h>

#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/profiler.h"
#include "dynarmic/backend/wasm/region_diagnostics.h"
#include "dynarmic/frontend/A64/a64_ir_emitter.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;

namespace {

EM_JS(void, check_absent, (), {
    const stats = Module.dynarmicWasmStats;
    const snapshot = stats.getInstance(stats.activeInstance);
    if (snapshot.regionDiagnostics !== undefined || stats.regionDiagnostics !== undefined ||
        snapshot.regionTransferDiagnostics !== undefined || stats.regionTransferDiagnostics !== undefined)
        throw new Error('region diagnostics: present in a non-diagnostic build');
});

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
EM_JS(void, check_serializer, (u64 id, int stage), {
    const stats = Module.dynarmicWasmStats;
    const snapshot = stats.getInstance(String(id));
    const diagnostic = snapshot?.regionDiagnostics;
    if (!diagnostic || diagnostic.schema !== 'dynarmic.js/region-diagnostics-v2' ||
        !Object.isFrozen(diagnostic) || !Object.isFrozen(diagnostic.regions))
        throw new Error('region diagnostics: invalid or mutable serializer leaf');
    if (stats.regionDiagnostics !== undefined)
        throw new Error('region diagnostics: unexpected legacy mirror');
    if (stage === 0) {
        if (diagnostic.regions.length !== 0) throw new Error('region diagnostics: empty history');
        return;
    }
    const region = diagnostic.regions[0];
    const shared = '0x8000000000001000', entry = '0xf123456789abcdef';
    if (region.regionId !== 1 || region.frontend !== 'a64' || region.entryDescriptor !== entry ||
        region.entryPc !== '0xfffffffffffffff0' || region.entryIndex !== 1 ||
        region.moduleBytes !== 1000 || region.entryBodyBytes !== 800 ||
        region.layout !== 'fallback' || region.liveRegionIdsBeforeCompile.length !== 0 ||
        JSON.stringify(region.blocks) !== JSON.stringify([
            {descriptor: shared, emitted: true, entryReachable: true, returnSite: false},
            {descriptor: entry, emitted: true, entryReachable: true, returnSite: true},
        ]) || !Object.isFrozen(region) || !Object.isFrozen(region.blocks) ||
        !Object.isFrozen(region.blocks[0]) || !Object.isFrozen(region.liveRegionIdsBeforeCompile))
        throw new Error('region diagnostics: 64-bit serializer mismatch');
});

EM_JS(void, check_transfer_serializer, (u64 id, int stage), {
    const stats = Module.dynarmicWasmStats;
    const snapshot = stats.getInstance(String(id));
    const diagnostic = snapshot?.regionTransferDiagnostics;
    if (!diagnostic || diagnostic.schema !== 'dynarmic.js/region-transfer-diagnostics-v1' ||
        !Object.isFrozen(diagnostic) || !Object.isFrozen(diagnostic.regions))
        throw new Error('transfer diagnostics: invalid or mutable serializer leaf');
    if (stats.regionTransferDiagnostics !== undefined)
        throw new Error('transfer diagnostics: unexpected legacy mirror');
    if (stage === 0) {
        if (diagnostic.regions.length !== 0) throw new Error('transfer diagnostics: empty history');
        return;
    }
    const region = diagnostic.regions[0];
    if (region.regionId !== 1 || region.frontend !== 'a64' ||
        region.callerEntryDescriptor !== '0xf123456789abcdef' ||
        region.callerEntryPc !== '0xfffffffffffffff0' || region.entryCount !== '0x0000000000000007' ||
        region.exits.length !== 2 || !Object.isFrozen(region) || !Object.isFrozen(region.exits))
        throw new Error('transfer diagnostics: region serializer mismatch');
    const [direct, other] = region.exits;
    if (direct.exitSite !== 0 || direct.exitBlockDescriptor !== '0x8000000000001000' ||
        direct.kind !== 'external-direct-call' || direct.staticTargetDescriptor !== '0x8000000000002000' ||
        direct.count !== '0x0020000000000001' || direct.lastDynamicTargetDescriptor !== '0x8000000000002000' ||
        direct.lastDynamicTargetPc !== '0x0000000000002000' ||
        other.exitSite !== 1 || other.exitBlockDescriptor !== '0xf123456789abcdef' ||
        other.kind !== 'other-tail-transfer' || other.staticTargetDescriptor !== null ||
        other.count !== '0x0000000000000003' || other.lastDynamicTargetDescriptor !== '0xf123456789abcdef' ||
        other.lastDynamicTargetPc !== '0xfffffffffffffff0' ||
        !Object.isFrozen(direct) || !Object.isFrozen(other))
        throw new Error('transfer diagnostics: exact 64-bit exit serializer mismatch');
});

EM_JS(void, hold_regions, (), {
    const stats = Module.dynarmicWasmStats;
    const id = String(stats.activeInstance);
    const value = stats.getInstance(id).regionDiagnostics;
    Module.regionTestFirstId = id;
    Module.regionTestHeld = {id, value, json: JSON.stringify(value)};
});

EM_JS(void, check_regions, (int expected, int same_leaf), {
    const require = (condition, message) => {
        if (!condition) throw new Error('region diagnostics: ' + message);
    };
    const stats = Module.dynarmicWasmStats;
    const id = String(stats.activeInstance);
    const snapshot = stats.getInstance(id);
    const diagnostic = snapshot?.regionDiagnostics;
    require(diagnostic && Object.isFrozen(diagnostic) && Object.isFrozen(diagnostic.regions), 'frozen leaf');
    require(diagnostic.regions.length === expected, 'region count');
    const leaves = Module.regionTestLast || (Module.regionTestLast = {});
    if (same_leaf) require(leaves[id] === diagnostic, 'cache hit replaced history');
    leaves[id] = diagnostic;
    const held = Module.regionTestHeld;
    if (held) require(JSON.stringify(held.value) === held.json, 'historical leaf changed');
    const entries = [0x1000, 0x1100, 0x1000, 0x1000];
    const liveIds = [[], [1], [2], []];
    const callees = ['0x0000000000000800', '0x0000000000002000',
        '0x0000000000000800', '0x0000000000000800'];
    const entryIndexes = [1, 0, 1, 1];
    let bytes = 0;
    for (let i = 0; i < diagnostic.regions.length; ++i) {
        const region = diagnostic.regions[i];
        const entryPc = '0x' + entries[i].toString(16).padStart(16, '0');
        const returnPc = '0x' + (entries[i] + 4).toString(16).padStart(16, '0');
        require(region.regionId === i + 1 && region.frontend === 'a64', 'region identity');
        require(region.entryPc === entryPc && region.entryIndex === entryIndexes[i], 'entry PC or index');
        require(region.entryDescriptor === entryPc, 'entry descriptor');
        const expectedSelected = [entryPc, returnPc, callees[i]];
        for (let pc = entries[i] + 8; expectedSelected.length < 64; pc += 4)
            expectedSelected.push('0x' + pc.toString(16).padStart(16, '0'));
        expectedSelected.sort();
        require(JSON.stringify(region.blocks.map(block => block.descriptor)) === JSON.stringify(expectedSelected),
            'selected descriptors at region ' + i + ': ' + JSON.stringify(region.blocks));
        require(JSON.stringify(region.liveRegionIdsBeforeCompile) === JSON.stringify(liveIds[i]),
            'pre-compile live IDs at region ' + i);
        require(['single', 'structured', 'fallback'].includes(region.layout), 'region layout');
        require(region.blocks.length === expectedSelected.length && region.blocks.length > 0, 'block count');
        for (let j = 0; j < region.blocks.length; ++j) {
            const block = region.blocks[j];
            require(typeof block.emitted === 'boolean' && typeof block.entryReachable === 'boolean' &&
                typeof block.returnSite === 'boolean', 'block flags');
            if (region.layout === 'structured') require(block.emitted === block.entryReachable, 'structured masks');
            if (region.layout === 'fallback') require(block.emitted, 'fallback emission');
            require(Object.isFrozen(block), 'mutable block');
        }
        require(region.blocks[region.entryIndex].descriptor === region.entryDescriptor &&
            region.blocks[region.entryIndex].emitted && region.blocks[region.entryIndex].entryReachable,
            'entry membership/emission/reachability');
        require(Object.isFrozen(region) && Object.isFrozen(region.blocks) &&
            Object.isFrozen(region.liveRegionIdsBeforeCompile), 'mutable region');
        require(Number.isSafeInteger(region.moduleBytes) && region.moduleBytes > 0 &&
            Number.isSafeInteger(region.entryBodyBytes) && region.entryBodyBytes > 0 &&
            region.entryBodyBytes <= region.moduleBytes, 'module/body bytes');
        bytes += region.moduleBytes;
    }
    require(snapshot.compile.blocks === expected && snapshot.compile.bytes === bytes, 'compile totals');
#if defined(DYNARMIC_WASM_PROFILE)
    require(snapshot.profile, 'profile missing in a profile build');
#else
    require(!snapshot.profile, 'unexpected profile in a non-profile build');
#endif
    if (snapshot.profile) {
        require(snapshot.profile.compiledBlocks === expected && snapshot.profile.emittedBytes === bytes,
            'profile totals');
        require(snapshot.profile.compiledRegions === expected && snapshot.profile.regionBlocks === expected * 64,
            'profile region totals');
    }
});

EM_JS(void, check_first_history, (int expected), {
    const stats = Module.dynarmicWasmStats;
    const held = Module.regionTestHeld;
    const current = stats.getInstance(held.id).regionDiagnostics;
    if (current.regions.length !== expected || JSON.stringify(held.value) !== held.json ||
        stats.getInstance(stats.activeInstance).regionDiagnostics.regions.length !== 1)
        throw new Error('region diagnostics: instance histories mixed');
});
#endif

class Callbacks final : public A64::UserCallbacks {
public:
    A64::Jit* jit = nullptr;
    u8 MemoryRead8(u64) override { return 0; }
    u16 MemoryRead16(u64) override { return 0; }
    u32 MemoryRead32(u64 address) override {
        switch (address) {
        case 0x0800:
        case 0x2000: return 0xd65f03c0;  // ret
        case 0x1000: return 0x97fffe00;  // bl 0x0800
        case 0x1100: return 0x940003c0;  // bl 0x2000
        case 0x1004:
        case 0x1104: return 0xd4000001;  // svc #0
        default: return 0xd4000001;
        }
    }
    u64 MemoryRead64(u64) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, u16) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64, size_t) override { jit->HaltExecution(HaltReason::UserDefined1); }
    void CallSVC(u32) override { jit->HaltExecution(HaltReason::UserDefined1); }
    void ExceptionRaised(u64, A64::Exception) override { jit->HaltExecution(HaltReason::UserDefined1); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 10000; }
    u64 GetCNTPCT() override { return 0; }
};

void Run(Callbacks& callbacks, A64::Jit& jit, u64 pc) {
    jit.ClearHalt(HaltReason::UserDefined1);
    jit.SetPC(pc);
    if (jit.Run() != HaltReason::UserDefined1) std::abort();
}

void TestSerializer() {
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    RegionDiagnostics history;
    const u64 id = AllocateProfileInstanceId();
    PublishStats(nullptr, CompileStats{}, id);
    PublishRegionDiagnostics(history, id);
    PublishRegionTransferDiagnostics(history, id);
    check_serializer(id, 0);
    check_transfer_serializer(id, 0);
    auto transfers = std::make_unique<RegionTransferRecord>();
    transfers->entry_count = 7;
    RegionTransferExit& direct = transfers->AddExit(
        0x8000000000001000ULL, RegionTransferKind::ExternalDirectCall, 0x8000000000002000ULL);
    direct.count = 0x0020000000000001ULL;
    direct.last_dynamic_target_descriptor = 0x8000000000002000ULL;
    direct.last_dynamic_target_pc = 0x2000;
    RegionTransferExit& other = transfers->AddExit(
        0xf123456789abcdefULL, RegionTransferKind::OtherTailTransfer);
    other.count = 3;
    other.last_dynamic_target_descriptor = 0xf123456789abcdefULL;
    other.last_dynamic_target_pc = 0xfffffffffffffff0ULL;
    history.Record(Frontend::A64, 0xf123456789abcdefULL, 0xfffffffffffffff0ULL, 1,
                   1000, 800, RegionDiagnosticLayout::Fallback,
                   {{0x8000000000001000ULL, 0xf123456789abcdefULL}, {1, 1}, {1, 1}, {0, 1}},
                   {}, std::move(transfers));
    PublishRegionDiagnostics(history, id);
    PublishRegionTransferDiagnostics(history, id);
    check_serializer(id, 1);
    check_transfer_serializer(id, 1);
#endif
}

std::vector<IR::Block> MakeTransferByteFixture() {
    const auto entry = A64::LocationDescriptor{0x1000, FP::FPCR{}};
    const auto target = A64::LocationDescriptor{0x3000, FP::FPCR{}};
    std::vector<IR::Block> blocks;
    blocks.emplace_back(IR::LocationDescriptor{entry});
    A64::IREmitter ir{blocks.back(), entry};
    ir.SetX(A64::Reg::R0, ir.Imm64(11));
    blocks.back().SetTerminal(IR::Term::If{
        IR::Cond::EQ, IR::Term::LinkBlockFast{IR::LocationDescriptor{target}}, IR::Term::FastDispatchHint{}});
    return blocks;
}

void TestTransferByteEquivalence() {
    A64::UserConfig user_config;
    EmitConfig config{false, true};
    config.frontend = Frontend::A64;
    config.a64_conf = &user_config;
    config.link_slots_base = 0x10000;
    config.fast_dispatch_base = 0x20000;
    const auto ordinary = EmitWASM(MakeTransferByteFixture(), config);
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    RegionTransferRecord transfers;
    config.transfer_record = &transfers;
    const auto instrumented = EmitWASM(MakeTransferByteFixture(), config);
    config.transfer_record = nullptr;
    const auto control = EmitWASM(MakeTransferByteFixture(), config);
    if (ordinary.module_bytes != control.module_bytes || ordinary.module_bytes == instrumented.module_bytes ||
        transfers.exits.size() != 2)
        throw std::runtime_error("transfer diagnostics: byte-equivalence control failed");
#else
    const auto control = EmitWASM(MakeTransferByteFixture(), config);
    if (ordinary.module_bytes != control.module_bytes)
        throw std::runtime_error("transfer diagnostics: production bytes changed");
#endif
}

void TestCompiledRegions() {
    Callbacks callbacks;
    A64::UserConfig conf;
    conf.enable_cycle_counting = false;
    conf.callbacks = &callbacks;
    A64::Jit jit{conf};
    callbacks.jit = &jit;
    Run(callbacks, jit, 0x1000);
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    check_regions(1, 0);
    Run(callbacks, jit, 0x1100);
    check_regions(2, 0);
    hold_regions();
    Run(callbacks, jit, 0x1000);
    check_regions(2, 1);  // warm cache hit: no new compilation
    jit.InvalidateCacheRange(0x1000, 8);
    Run(callbacks, jit, 0x1000);
    check_regions(3, 0);
    jit.ClearCache();
    Run(callbacks, jit, 0x1000);
    check_regions(4, 0);

    Callbacks other_callbacks;
    conf.callbacks = &other_callbacks;
    A64::Jit other{conf};
    other_callbacks.jit = &other;
    Run(other_callbacks, other, 0x1000);
    check_regions(1, 0);
    Run(other_callbacks, other, 0x1000);
    check_regions(1, 1);
    Run(callbacks, jit, 0x1000);
    check_regions(4, 1);
    Run(other_callbacks, other, 0x1000);
    check_regions(1, 1);
    check_first_history(4);
#else
    check_absent();
#endif
}

}  // namespace

int main() {
    TestSerializer();
    TestTransferByteEquivalence();
    TestCompiledRegions();
    std::puts("PASS region diagnostics publication and compiled regions");
}
