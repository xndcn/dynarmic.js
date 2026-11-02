// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dynarmic/frontend/wasm/arm_decoder.h"
#include "dynarmic/frontend/A32/translate/impl/a32_translate_impl.h"

using namespace Dynarmic;
using namespace Dynarmic::A32;

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL arm decoder: %s\n", message);
        std::exit(1);
    }
}

template<typename Actual>
void SameMatcher(const Actual& actual, const ArmMatcher<TranslatorVisitor>& expected) {
    Require(actual.GetMask() == expected.GetMask(), "mask differs");
    Require(actual.GetExpected() == expected.GetExpected(), "expected bits differ");
    Require(std::strcmp(actual.GetName(), expected.GetName()) == 0, "rule priority differs");
}

void CheckWord(u32 word) {
    const auto expected = DecodeArm<TranslatorVisitor>(word);
    const auto actual = DecodeArmIndexed<TranslatorVisitor>(word);
    Require(bool(actual) == bool(expected), "match presence differs");
    if (actual) SameMatcher(actual->get(), expected->get());
}
struct ArgumentVisitor {
    using instruction_return_type = std::array<u32, 6>;
    size_t calls = 0;
    instruction_return_type Decode(IR::Cond cond, bool flags, Reg rn, Reg rd, int rotate, u8 immediate) {
        ++calls;
        return {static_cast<u32>(cond), flags, static_cast<u32>(rn), static_cast<u32>(rd),
                static_cast<u32>(rotate), immediate};
    }
    instruction_return_type NoArgs() { ++calls; return {42}; }
    instruction_return_type Const(u32 value) const { return {value}; }
};

void CheckArguments() {
    // Typed fields, truncation to bool/u8, zero-argument and const handlers.
    // Use the upstream factory as the reference and explicit instruction-bit
    // expectations as a second oracle for the heterogeneous handler.
    constexpr auto bits = Decoder::detail::StringToArray<32>("cccc0010101Snnnnddddrrrrvvvvvvvv");
    const auto reference = Decoder::detail::detail<ArmMatcher<ArgumentVisitor>>::GetMatcher<bits>(
        &ArgumentVisitor::Decode, "args");
    constexpr auto actual = IndexedArm::MakeMatcher<ArgumentVisitor, &ArgumentVisitor::Decode, bits>("args");
    u32 random = 0xDECADE01;
    ArgumentVisitor old_visitor, new_visitor;
    for (size_t i = 0; i < 65536; ++i) {
        random = random * 1664525 + 1013904223;
        const u32 word = actual.GetExpected() | (random & ~actual.GetMask());
        const auto result = actual.call(new_visitor, word);
        Require(result == reference.call(old_visitor, word), "handler argument dispatch differs");
        Require(result == ArgumentVisitor::instruction_return_type{
            word >> 28, (word >> 20) & 1, (word >> 16) & 15, (word >> 12) & 15,
            (word >> 8) & 15, word & 255}, "handler fields differ from encoding");
    }
    Require(new_visitor.calls == 65536 && old_visitor.calls == 65536, "handler call count differs");
    constexpr auto zero = IndexedArm::MakeMatcher<ArgumentVisitor, &ArgumentVisitor::NoArgs,
        Decoder::detail::StringToArray<32>("11111111111111111111111111111111")>("zero");
    Require(zero.call(new_visitor, 0xFFFFFFFF) == ArgumentVisitor::instruction_return_type{42}, "zero arguments");
    constexpr auto constant = IndexedArm::MakeMatcher<const ArgumentVisitor, &ArgumentVisitor::Const,
        Decoder::detail::StringToArray<32>("vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv")>("const");
    Require(constant.call(new_visitor, 0xF1234567) == ArgumentVisitor::instruction_return_type{0xF1234567}, "const handler");
}
}  // namespace

int main() {
    CheckArguments();
    // The reference retains the original upstream runtime builder. Comparing
    // every ordered bucket establishes equivalence for all possible words:
    // lookup projection, candidate masks, expectations and order are identical.
    const auto reference = GetArmDecodeTable<TranslatorVisitor>();
    const auto& matchers = IndexedArm::matchers<TranslatorVisitor>;
    size_t total = 0;
    for (u32 bucket = 0; bucket < reference.size(); ++bucket) {
        const u16 begin = IndexedArm::index.offsets[bucket];
        const u16 end = IndexedArm::index.offsets[bucket + 1];
        Require(end >= begin && end <= IndexedArm::index.candidates.size(), "invalid bucket span");
        Require(end - begin == reference[bucket].size(), "bucket membership differs");
        for (u16 i = begin; i < end; ++i) {
            const u16 id = IndexedArm::index.candidates[i];
            Require(id < matchers.size(), "candidate index out of range");
            SameMatcher(matchers[id], reference[bucket][i - begin]);
            ++total;
        }
        const u32 word = ((bucket & 0xF) << 4) | ((bucket & 0xFF0) << 16);
        Require(detail::ToFastLookupIndexArm(word) == IndexedArm::Project(word), "lookup projection differs");
    }
    // Exercise the lookup itself, including condition fields and near-misses.
    for (const auto& matcher : matchers) {
        for (u32 cond = 0; cond < 16; ++cond) {
            const u32 word = (matcher.GetExpected() & 0x0FFFFFFF) | (cond << 28);
            CheckWord(word);
            CheckWord(word | ~matcher.GetMask());
            for (unsigned bit = 0; bit < 32; ++bit) CheckWord(word ^ (u32{1} << bit));
        }
    }
    u32 random = 0xC0DEC0DE;
    for (size_t i = 0; i < 1'000'000; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        CheckWord(random);
    }
    std::printf("PASS arm decoder: %zu buckets, %zu candidates, %zu rules, 1000000 random words\n",
                reference.size(), total, matchers.size());
}
