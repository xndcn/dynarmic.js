// Exercise generated-entry ownership alongside Emscripten's ordinary function
// registration. Checks remain enabled in Release and profiling builds.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

#include <emscripten.h>

#include "dynarmic/backend/wasm/wasm_runtime.h"

using namespace Dynarmic::Backend::Wasm;

EM_JS(void, check_no_scan, (), {
    if (functionsInTableMap) throw new Error('fresh block registration scanned the main table');
});
EM_JS(void, hold_entry, (uptr slot), {
    Module.tableTestHeld = getWasmTableEntry(slot);
});
EM_JS(void, check_released, (uptr slot), {
    if (wasmTable.get(slot) !== null || getWasmTableEntry(slot) !== null)
        throw new Error('released entry or mirror remains live');
    if (functionsInTableMap?.has(Module.tableTestHeld))
        throw new Error('released function remains in deduplication map');
});
EM_JS(uptr, add_host_entry, (uptr native, uptr generated), {
    if (getWasmTableEntry(native)(3, 4) !== 24)
        throw new Error('main-module function was clobbered');
    if (addFunction(getWasmTableEntry(native), 'iii') !== native)
        throw new Error('native function deduplication changed');
    if (addFunction(getWasmTableEntry(generated), 'iii') !== generated)
        throw new Error('lazy scan missed generated entry');
    return addFunction((a, b) => a + b + 100, 'iii');
});
EM_JS(void, check_registered, (uptr slot), {
    const fn = getWasmTableEntry(slot);
    if (!fn || fn !== wasmTable.get(slot) || functionsInTableMap.get(fn) !== slot)
        throw new Error('generated entry/map/mirror disagree');
});
EM_JS(void, remove_host_entry, (uptr slot), { removeFunction(slot); });
EM_JS(unsigned, table_size, (), { return wasmTable.length; });

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL runtime table: %s\n", message);
        std::abort();
    }
}
u32 NativeEntry(uptr a, uptr b) { return static_cast<u32>(a + b + 17); }
// Independent Wasm fixture: f(i32, i32) returns (arg0 XOR arg1) + a tag.
// Defined in each instance, not a re-export of an imported/shared function.
constexpr std::array<u8, 42> code{
    0,97,115,109,1,0,0,0, 1,7,1,96,2,127,127,1,127,
    3,2,1,0, 7,5,1,1,102,0,0, 10,12,1,10,0,32,0,32,1,115,65,0,106,11};
BlockFunc NewEntry(u8 tag) {
    Require(tag <= 63, "tag must fit a positive one-byte signed LEB");
    auto tagged = code;
    tagged[tagged.size() - 3] = tag;
    return InstantiateBlock(tagged.data(), tagged.size());
}
void Check(BlockFunc f, u32 tag) {
    Require(f && f(0x1234, 0x5678) == 0x444c + tag, "wrong entry result or live entry clobbered");
}
void Release(BlockFunc f) {
    hold_entry(reinterpret_cast<uptr>(f));
    ReleaseBlock(f);
    check_released(reinterpret_cast<uptr>(f));
}
}

int main() {
    check_no_scan();
    const auto a = NewEntry(1), b = NewEntry(2);
    Require(a != b, "distinct instances share a slot");
    Check(a, 1); Check(b, 2); check_no_scan();
    Release(a); check_no_scan();
    const unsigned before_reuse = table_size();
    const auto c = NewEntry(3);
    Require(table_size() == before_reuse, "free slot was not reused");
    Check(c, 3); Check(b, 2); check_no_scan();

    const auto host = reinterpret_cast<BlockFunc>(add_host_entry(
        reinterpret_cast<uptr>(&NativeEntry), reinterpret_cast<uptr>(c)));
    Require(host(3, 4) == 107 && NativeEntry(3, 4) == 24, "host function clobbered");
    check_registered(reinterpret_cast<uptr>(c));
    Release(c);
    const auto d = NewEntry(4);
    check_registered(reinterpret_cast<uptr>(d));
    Check(d, 4); Check(b, 2);
    // A generic host slot must be available to the generated-block allocator.
    remove_host_entry(reinterpret_cast<uptr>(host));
    const unsigned host_released = table_size();
    const auto e = NewEntry(5);
    Require(table_size() == host_released, "host free slot not reused");
    check_registered(reinterpret_cast<uptr>(e));
    Check(e, 5);

    unsigned high_water = 0;
    for (int cycle = 0; cycle < 2; ++cycle) {
        std::array<BlockFunc, 128> entries;
        for (size_t i = 0; i < entries.size(); ++i) {
            const u8 tag = static_cast<u8>(32 + i % 32);
            auto f = NewEntry(tag);
            Require(std::find(entries.begin(), entries.begin() + i, f) == entries.begin() + i,
                    "live generated entries share a slot");
            entries[i] = f;
            Check(f, tag);
            check_registered(reinterpret_cast<uptr>(f));
        }
        for (size_t i = 0; i < entries.size(); ++i) Check(entries[i], static_cast<u32>(32 + i % 32));
        Check(b, 2); Check(d, 4); Check(e, 5);
        if (cycle == 0) high_water = table_size();
        else Require(table_size() == high_water, "repeated population grows table");
        for (auto f : entries) Release(f);
        Check(b, 2); Check(d, 4); Check(e, 5);
    }
    const unsigned before_host_reuse = table_size();
    const auto host_again = reinterpret_cast<BlockFunc>(add_host_entry(
        reinterpret_cast<uptr>(&NativeEntry), reinterpret_cast<uptr>(b)));
    Require(table_size() == before_host_reuse && host_again(1, 2) == 103,
            "host cannot reuse a generated-entry slot");
    remove_host_entry(reinterpret_cast<uptr>(host_again));
    Release(b); Release(d); Release(e);
    std::puts("PASS runtime table ownership, lazy map and shared slot reuse");
}
