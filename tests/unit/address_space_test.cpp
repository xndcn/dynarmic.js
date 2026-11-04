// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/icl/interval_set.hpp>

#include "dynarmic/backend/wasm/address_space.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::Wasm {

// Compile-time-only access for cache ownership tests. No execution or emission
// hook is added to AddressSpace; the aliases below model future cache states.
struct AddressSpaceTestAccess {
    using Entry = AddressSpace::BlockEntry;
    using EntryPtr = std::shared_ptr<Entry>;
    using EntryMap = decltype(AddressSpace::block_entries);

    static EntryPtr MakeEntry(BlockFunc fn, u32* link_slots, size_t module_bytes) {
        auto entry = std::make_shared<Entry>();
        entry->fn = fn;
        entry->link_slots = link_slots;
        entry->num_link_slots = 1;
        entry->module_bytes = module_bytes;
        entry->range = {0x1000, 0x1003};
        entry->region_id = 7;
        return entry;
    }

    template <typename Space>
    static void InstallEntry(Space& space, u64 descriptor, const EntryPtr& entry) {
        const auto result = space.block_entries.emplace(descriptor, MapValue<typename EntryMap::mapped_type>(entry));
        if (!result.second) {
            throw std::runtime_error{"duplicate test descriptor"};
        }
        space.cached_module_bytes += entry->module_bytes;
        ++space.cached_block_count;
    }

    template <typename Space>
    static void InstallAliases(Space& space, u64 first, u64 second, const EntryPtr& entry) {
        const auto first_result = space.block_entries.emplace(first, MapValue<typename EntryMap::mapped_type>(entry));
        if (!first_result.second) {
            throw std::runtime_error{"duplicate first test descriptor"};
        }
        const auto second_result = space.block_entries.emplace(second, MapValue<typename EntryMap::mapped_type>(entry));
        if (!second_result.second) {
            throw std::runtime_error{"duplicate second test descriptor"};
        }
        space.cached_module_bytes += entry->module_bytes;
        ++space.cached_block_count;
    }

    template <typename Space>
    static void InstallDispatch(Space& space, u64 descriptor, const EntryPtr& entry) {
        space.dispatch_cache[FastDispatchSlot(descriptor)] = {descriptor, entry.get()};
    }

    template <typename Space>
    static void InstallFastDispatch(Space& space, u64 descriptor, const EntryPtr& entry) {
        auto& fast = space.fast_dispatch[FastDispatchSlot(descriptor)];
        fast.descriptor = descriptor;
        fast.index = static_cast<u32>(reinterpret_cast<uptr>(entry->fn));
    }

    template <typename Space>
    static bool FastDispatchMatches(const Space& space, u64 descriptor, const EntryPtr& entry) {
        const auto& fast = space.fast_dispatch[FastDispatchSlot(descriptor)];
        return fast.descriptor == descriptor &&
               fast.index == static_cast<u32>(reinterpret_cast<uptr>(entry->fn));
    }

    static u64 LastUsed(const EntryPtr& entry) {
        return entry->last_used;
    }

    template <typename Space>
    static auto GenerateRegion(const Space& space, u64 descriptor) {
        return space.GenerateRegion(IR::LocationDescriptor{descriptor});
    }

    template <typename Space>
    static void Trim(Space& space, size_t capacity, bool pending_link = false) {
        space.cache_capacity = capacity;
        space.suspend_trim = pending_link;
        space.TrimCache();
        space.suspend_trim = false;
    }

    template <typename Space>
    static bool HasRecordedLink(const Space& space, const u32* slot) {
        return space.patched_slots.find(const_cast<u32*>(slot)) != space.patched_slots.end();
    }

    template <typename Space>
    static bool DispatchMatches(const Space& space, u64 descriptor, const EntryPtr& entry) {
        const auto& fast = space.dispatch_cache[FastDispatchSlot(descriptor)];
        return fast.descriptor == descriptor && fast.entry == entry.get();
    }

    template <typename Space>
    static size_t CachedBlockCount(const Space& space) {
        if constexpr (requires { space.cached_block_count; }) {
            return space.cached_block_count;
        } else {
            return space.block_entries.size();
        }
    }

    static size_t CachedModuleBytes(const AddressSpace& space) {
        return space.cached_module_bytes;
    }

private:
    template <typename Mapped>
    static Mapped MapValue(const EntryPtr& entry) {
        if constexpr (std::is_same_v<Mapped, EntryPtr>) {
            return entry;
        } else {
            return *entry;
        }
    }
};

}  // namespace Dynarmic::Backend::Wasm

namespace {

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;
using TestAccess = AddressSpaceTestAccess;

class TestAddressSpace final : public AddressSpace {
public:
    using AddressSpace::AddressSpace;

    size_t chain_blocks = 0;
    bool boundary_enabled = true;
    bool chain_link_blocks = false;
    bool reuse_entries = false;
    BoundaryState boundary{};

protected:
    IR::Block GenerateIR(IR::LocationDescriptor descriptor) const override {
        IR::Block block{descriptor};
        if (chain_blocks != 0 && descriptor.Value() + 4 < 0x1000 + chain_blocks * 4) {
            block.SetTerminal(IR::Term::LinkBlock{IR::LocationDescriptor{descriptor.Value() + 4}});
        } else {
            block.SetTerminal(IR::Term::ReturnToDispatch{});
        }
        return block;
    }
    GuestRange RangeOf(const IR::Block& block) const override {
        return {block.Location().Value(), block.Location().Value() + 3};
    }
    u64 GuestPC(IR::LocationDescriptor location) const override { return location.Value(); }
    EmitConfig MakeEmitConfig() const override {
        EmitConfig config{};
        config.enable_cycle_counting = true;
        config.boundary_state = boundary_enabled ? &boundary : nullptr;
        config.enable_region_multi_entry = reuse_entries;
        return config;
    }
    bool ChainLinkBlock() const override { return chain_link_blocks; }
    bool RegionCompatible(IR::LocationDescriptor, IR::LocationDescriptor) const override { return true; }
};

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL address space: " << message << '\n';
        std::exit(1);
    }
}

TestAccess::EntryPtr MakeEntry() {
    auto* slots = static_cast<u32*>(std::malloc(sizeof(u32)));
    Require(slots != nullptr, "allocating test link slot");
    *slots = 0;
    return TestAccess::MakeEntry(reinterpret_cast<BlockFunc>(0x1234), slots, 123);
}

void TestBoundaryRegionLimits() {
    TestAddressSpace boundary_space;
    boundary_space.chain_blocks = 65;
    const auto boundary = TestAccess::GenerateRegion(boundary_space, 0x1000);
    Require(boundary.first.size() == 64,
            "opt-in A32 boundary regions must retain the ordinary 64-block cap");

    TestAddressSpace ordinary;
    ordinary.chain_blocks = 65;
    ordinary.boundary_enabled = false;
    ordinary.chain_link_blocks = true;
    const auto normal = TestAccess::GenerateRegion(ordinary, 0x1000);
    Require(normal.first.size() == 64,
            "ordinary region selection must retain its default 64-block cap");
}

void RequireOneLiveAllocation(const AddressSpace& space) {
    Require(TestAccess::CachedBlockCount(space) == 1,
            "two descriptors must count as one cached allocation");
    Require(TestAccess::CachedModuleBytes(space) == 123,
            "two descriptors must count module bytes once");
}

void TestSharedSelectionStopsAtCachedEntry() {
    TestAddressSpace space;
    space.chain_blocks = 8;
    TestAccess::InstallEntry(space, 0x100c, MakeEntry());
    Require(TestAccess::GenerateRegion(space, 0x1000).first.size() == 8,
            "ordinary selection must keep its previous behavior");
    space.reuse_entries = true;
    Require(TestAccess::GenerateRegion(space, 0x1000).first.size() == 3,
            "shared selection must chain into a cached owner without retranslation");
}

void TestFastDispatchLookup() {
    TestAddressSpace space;
    auto entry = MakeEntry();
    const IR::LocationDescriptor cached{0x1000};
    const IR::LocationDescriptor collision{0x4001000};
    Require(FastDispatchSlot(cached.Value()) == FastDispatchSlot(collision.Value()),
            "test descriptors must collide in the direct-mapped table");
    TestAccess::InstallDispatch(space, cached.Value(), entry);
    Require(space.Get(cached) == entry->fn,
            "dispatcher lookup must use a matching fast-dispatch entry");
    Require(space.Get(collision) == nullptr,
            "a colliding descriptor must not reuse another block");
    space.ClearCache();
    Require(space.Get(cached) == nullptr,
            "cache invalidation must drop dispatcher fast-path entries");
}

void TestFastDispatchRefill() {
    TestAddressSpace space;
    auto entry = MakeEntry();
    const IR::LocationDescriptor cached{0x1000};
    TestAccess::InstallAliases(space, cached.Value(), 0x2000, entry);
    Require(space.Get(cached) == entry->fn,
            "map lookup must find a block whose fast entry was invalidated");
    Require(TestAccess::DispatchMatches(space, cached.Value(), entry),
            "map lookup must refill the dispatcher fast path");
}

void TestLruTrimClearsDispatchCaches() {
    TestAddressSpace space;
    constexpr u64 hot_descriptor = 0x100000;
    auto hot = MakeEntry();
    TestAccess::InstallEntry(space, hot_descriptor, hot);
    TestAccess::InstallDispatch(space, hot_descriptor, hot);
    TestAccess::InstallFastDispatch(space, hot_descriptor, hot);
    Require(space.Get(IR::LocationDescriptor{hot_descriptor}) == hot->fn,
            "hot block must start on the native dispatch path");

    for (u64 i = 0; i < 1024; ++i) {
        TestAccess::InstallEntry(space, 0x2000 + i * 4, MakeEntry());
    }
    const IR::LocationDescriptor fallback{0x2000};
    Require(space.Get(fallback) != nullptr,
            "cold map entry must be available before trimming");
    Require(TestAccess::LastUsed(hot) == 1,
            "native dispatch hit must update LRU metadata");
    Require(space.Get(IR::LocationDescriptor{hot_descriptor}) == hot->fn &&
                TestAccess::LastUsed(hot) == 3,
            "a second native hit must remain newer than the fallback hit");
    space.RecordLink(hot->link_slots, IR::LocationDescriptor{fallback});
    Require(TestAccess::HasRecordedLink(space, hot->link_slots),
            "test must start with a recorded link from the hot block to a cold target");

    TestAccess::Trim(space, 1025, true);
    Require(TestAccess::CachedBlockCount(space) == 1025 &&
                TestAccess::FastDispatchMatches(space, hot_descriptor, hot) &&
                TestAccess::HasRecordedLink(space, hot->link_slots),
            "pending link resolution must defer eviction and preserve source slots");
    TestAccess::Trim(space, 1025);
    Require(space.Get(fallback) == nullptr,
            "trim must evict cold descriptors");
    Require(TestAccess::CachedBlockCount(space) == 1 &&
                TestAccess::CachedModuleBytes(space) == 123,
            "trim must retain accounting for the hot allocation");
    Require(!TestAccess::FastDispatchMatches(space, hot_descriptor, hot),
            "trim must clear generated-code dispatch entries");
    Require(*hot->link_slots == 0 && !TestAccess::HasRecordedLink(space, hot->link_slots),
            "trim must unlink surviving blocks whose target was evicted");
    Require(space.Get(IR::LocationDescriptor{hot_descriptor}) == hot->fn,
            "trim must preserve the hot block through map fallback");
    Require(TestAccess::DispatchMatches(space, hot_descriptor, hot),
            "hot map fallback must refill the cleared native dispatch entry");
}

void TestLruTrimCountsSharedAllocations() {
    TestAddressSpace space;
    auto shared = MakeEntry();
    TestAccess::InstallAliases(space, 0x1000, 0x2000, shared);
    for (u64 i = 0; i < 1023; ++i) {
        TestAccess::InstallEntry(space, 0x10000 + i * 4, MakeEntry());
    }
    Require(space.Get(IR::LocationDescriptor{0x1000}) == shared->fn,
            "shared allocation must be newer than cold entries");

    TestAccess::Trim(space, 1025);
    Require(space.Get(IR::LocationDescriptor{0x1000}) == shared->fn &&
                space.Get(IR::LocationDescriptor{0x2000}) == shared->fn &&
                TestAccess::CachedBlockCount(space) == 1024,
            "lookup aliases must not consume allocation capacity");

    auto cold = MakeEntry();
    TestAccess::InstallAliases(space, 0x3000, 0x4000, cold);
    space.RecordLink(shared->link_slots, IR::LocationDescriptor{0x4000});
    TestAccess::Trim(space, 1025);
    Require(space.Get(IR::LocationDescriptor{0x1000}) == shared->fn &&
                space.Get(IR::LocationDescriptor{0x2000}) == shared->fn,
            "trim must preserve every alias of the hot allocation");
    Require(space.Get(IR::LocationDescriptor{0x3000}) == nullptr &&
                space.Get(IR::LocationDescriptor{0x4000}) == nullptr &&
                *shared->link_slots == 0 && !TestAccess::HasRecordedLink(space, shared->link_slots),
            "trim must remove every cold alias and unlink incoming edges");
    Require(TestAccess::CachedBlockCount(space) == 1 &&
                TestAccess::CachedModuleBytes(space) == 123,
            "trim must count the surviving shared allocation once");
}

void TestRangeInvalidation() {
    ProfileCounters profile;
    TestAddressSpace space{&profile};
    auto entry = MakeEntry();
    TestAccess::InstallAliases(space, 0x1000, 0x2000, entry);
    TestAccess::InstallDispatch(space, 0x1000, entry);
    TestAccess::InstallDispatch(space, 0x2000, entry);
    RequireOneLiveAllocation(space);
    Require(space.Get(IR::LocationDescriptor{0x1000}) == entry->fn &&
                space.Get(IR::LocationDescriptor{0x2000}) == entry->fn,
            "both descriptors must resolve to the shared function");

    boost::icl::interval_set<u64> ranges;
    ranges.insert(boost::icl::discrete_interval<u64>::closed(0x1000, 0x1003));
    space.InvalidateRanges(ranges);
    Require(space.Get(IR::LocationDescriptor{0x1000}) == nullptr &&
                space.Get(IR::LocationDescriptor{0x2000}) == nullptr,
            "range invalidation must drop every descriptor of the allocation");
    Require(TestAccess::CachedBlockCount(space) == 0 && TestAccess::CachedModuleBytes(space) == 0,
            "range invalidation must subtract one allocation and its bytes once");
#if defined(DYNARMIC_WASM_PROFILE)
    Require(profile.invalidated_blocks == 1,
            "range invalidation profile must count allocations, not descriptors");
#endif
}

void TestClearCache() {
    ProfileCounters profile;
    TestAddressSpace space{&profile};
    auto entry = MakeEntry();
    std::weak_ptr<TestAccess::Entry> observation = entry;
    TestAccess::InstallAliases(space, 0x1000, 0x2000, entry);
    space.ClearCache();
    Require(!observation.expired(), "clear must not destroy an allocation with an external owner");
    Require(TestAccess::CachedBlockCount(space) == 0 && TestAccess::CachedModuleBytes(space) == 0,
            "clear must reset allocation accounting");
#if defined(DYNARMIC_WASM_PROFILE)
    Require(profile.invalidated_blocks == 1,
            "clear profile must count allocations, not descriptors");
#endif
    entry.reset();
    Require(observation.expired(), "the final shared owner must destroy the allocation");
}

void TestDestruction() {
    auto entry = MakeEntry();
    std::weak_ptr<TestAccess::Entry> observation = entry;
    {
        TestAddressSpace space;
        TestAccess::InstallAliases(space, 0x1000, 0x2000, entry);
    }
    Require(!observation.expired(), "cache destruction must respect external shared owners");
    entry.reset();
    Require(observation.expired(), "cache destruction and the final owner must release once");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1 || std::string_view{argv[1]} == "region_limits") {
        TestBoundaryRegionLimits();
        TestSharedSelectionStopsAtCachedEntry();
    }
    if (argc == 1 || std::string_view{argv[1]} == "dispatch") {
        TestFastDispatchLookup();
    }
    if (argc == 1 || std::string_view{argv[1]} == "refill") {
        TestFastDispatchRefill();
    }
    if (argc == 1 || std::string_view{argv[1]} == "trim") {
        TestLruTrimClearsDispatchCaches();
    }
    if (argc == 1 || std::string_view{argv[1]} == "trim_aliases") {
        TestLruTrimCountsSharedAllocations();
    }
    if (argc == 1 || std::string_view{argv[1]} == "invalidate") {
        TestRangeInvalidation();
    }
    if (argc == 1 || std::string_view{argv[1]} == "clear") {
        TestClearCache();
    }
    if (argc == 1 || std::string_view{argv[1]} == "destroy") {
        TestDestruction();
    }
    std::cout << "PASS address space shared allocation lifecycle\n";
}
