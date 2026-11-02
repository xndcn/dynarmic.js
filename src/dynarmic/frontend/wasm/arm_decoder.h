// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

#include "dynarmic/frontend/A32/decoder/arm.h"

namespace Dynarmic::A32 {
namespace IndexedArm {

// Keep upstream's 12-bit lookup projection and stable specificity ordering.
// Only bucket membership moves to compile time; handlers and argument decoding
// still come from the upstream arm.inc and matcher factory.
constexpr u32 Project(u32 word) {
    return ((word >> 4) & 0x00F) | ((word >> 16) & 0xFF0);
}

struct Pattern {
    u32 mask = 0;
    u32 expected = 0;
    unsigned specificity = 0;

    consteval Pattern(const char (&bits)[33]) {
        for (unsigned i = 0; i < 32; ++i) {
            if (bits[i] == '0' || bits[i] == '1') {
                mask |= u32{1} << (31 - i);
                expected |= static_cast<u32>(bits[i] == '1') << (31 - i);
                ++specificity;
            }
        }
    }
};

inline constexpr auto patterns = std::to_array<Pattern>({
#define INST(fn, name, bits) Pattern{bits},
#include "dynarmic/frontend/A32/decoder/arm.inc"
#undef INST
});

inline constexpr size_t bucket_count = 0x1000;
inline constexpr size_t candidate_count = [] {
    size_t count = 0;
    for (const auto& pattern : patterns) {
        count += size_t{1} << (12 - std::popcount(Project(pattern.mask)));
    }
    return count;
}();
static_assert(patterns.size() < std::numeric_limits<u16>::max());
static_assert(candidate_count < std::numeric_limits<u16>::max());

struct Index {
    std::array<u16, bucket_count + 1> offsets{};
    std::array<u16, candidate_count> candidates{};
};

// Enumerate only matching buckets instead of scanning all 4096 for each rule.
// This also keeps constexpr evaluation below the compiler's default step limit.
template<typename F>
constexpr void ForBuckets(const Pattern& pattern, F visit) {
    const u32 free = (~Project(pattern.mask)) & (bucket_count - 1);
    const u32 expected = Project(pattern.expected);
    u32 subset = free;
    do {
        visit(expected | subset);
        if (subset == 0) break;
        subset = (subset - 1) & free;
    } while (true);
}

inline constexpr Index index = [] {
    Index result;
    std::array<u16, patterns.size()> order{};
    for (size_t i = 0; i < patterns.size(); ++i) {
        // Stable insertion sort: equal-mask-popcount rules retain arm.inc order.
        size_t j = i;
        while (j > 0 && patterns[order[j - 1]].specificity < patterns[i].specificity) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = static_cast<u16>(i);
        ForBuckets(patterns[i], [&](u32 bucket) { ++result.offsets[bucket + 1]; });
    }
    for (size_t i = 1; i < result.offsets.size(); ++i) {
        result.offsets[i] += result.offsets[i - 1];
    }
    auto cursor = result.offsets;
    for (const u16 id : order) {
        ForBuckets(patterns[id], [&](u32 bucket) { result.candidates[cursor[bucket]++] = id; });
    }
    return result;
}();
static_assert(index.offsets.back() == candidate_count);

template<typename V>
struct StaticMatcher {
    using Return = typename V::instruction_return_type;
    const char* name;
    u32 mask;
    u32 expected;
    Return (*handler)(V&, u32);

    const char* GetName() const { return name; }
    u32 GetMask() const { return mask; }
    u32 GetExpected() const { return expected; }
    bool Matches(u32 instruction) const { return (instruction & mask) == expected; }
    Return call(V& visitor, u32 instruction) const {
        ASSERT(Matches(instruction));
        return handler(visitor, instruction);
    }
};

// Specialize the upstream argument decoder on a compile-time member pointer.
// Each generated function has no captures, so its address can live directly
// in read-only data. No std::function, allocation or lazy initialization is
// needed. In particular, no large one-time initializer enters TranslateArm
// and causes the engine to optimize cold construction code on hot lookups.
template<typename V, auto fn, auto bits>
typename V::instruction_return_type Dispatch(V& visitor, u32 instruction) {
    using Detail = Decoder::detail::detail<ArmMatcher<V>>;
    constexpr size_t count = mcl::parameter_count_v<decltype(fn)>;
    constexpr auto args = Detail::template GetArgInfo<count>(bits);
    const auto proxy = Detail::template VisitorCaller<decltype(fn)>::Make(
        std::make_index_sequence<count>{}, fn, std::get<0>(args), std::get<1>(args));
    return proxy(visitor, instruction);
}

template<typename V, auto fn, auto bits>
constexpr StaticMatcher<V> MakeMatcher(const char* name) {
    using Detail = Decoder::detail::detail<ArmMatcher<V>>;
    constexpr auto mask_expected = Detail::GetMaskAndExpect(bits);
    return {name, std::get<0>(mask_expected), std::get<1>(mask_expected), &Dispatch<V, fn, bits>};
}

template<typename V>
inline constexpr auto matchers = std::to_array<StaticMatcher<V>>({
#define INST(fn, name, bits) MakeMatcher<V, &V::fn, Decoder::detail::StringToArray<32>(bits)>(name),
#include "dynarmic/frontend/A32/decoder/arm.inc"
#undef INST
});

}  // namespace IndexedArm

template<typename V>
std::optional<std::reference_wrapper<const IndexedArm::StaticMatcher<V>>> DecodeArmIndexed(u32 instruction) {
    const auto& matchers = IndexedArm::matchers<V>;
    const u32 bucket = IndexedArm::Project(instruction);
    for (u16 i = IndexedArm::index.offsets[bucket]; i < IndexedArm::index.offsets[bucket + 1]; ++i) {
        const auto& matcher = matchers[IndexedArm::index.candidates[i]];
        if (matcher.Matches(instruction)) return matcher;
    }
    return std::nullopt;
}

}  // namespace Dynarmic::A32
