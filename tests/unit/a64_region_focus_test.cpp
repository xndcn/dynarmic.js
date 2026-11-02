// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dynarmic/backend/wasm/a64_address_space.h"
#include "dynarmic/backend/wasm/a64_region_focus.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::Wasm {

struct AddressSpaceTestAccess {
    static std::optional<RegionSelectionFocus> Focus(const AddressSpace& space,
                                                     IR::LocationDescriptor entry) {
        return space.FocusForEntry(entry);
    }

    static std::pair<std::vector<IR::Block>, size_t> GenerateRegion(
            const AddressSpace& space, IR::LocationDescriptor entry) {
        return space.GenerateRegion(entry);
    }
};

}  // namespace Dynarmic::Backend::Wasm

namespace {

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;
using D = IR::LocationDescriptor;

constexpr std::string_view kEntry = "0x000000000000dead";
constexpr std::string_view kExit = "0x000000000000deae";
constexpr std::string_view kDirect = "0x000000000000deaf";
constexpr std::string_view kValid = "0x000000000000dead,0x000000000000deae,0x000000000000deaf";

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error{message};
}

template<typename Exception, typename Callback>
void Throws(Callback callback, const std::string& diagnostic) {
    try {
        callback();
    } catch (const Exception& error) {
        Require(std::string{error.what()}.find(diagnostic) != std::string::npos,
                "wrong diagnostic: " + std::string{error.what()});
        return;
    }
    throw std::runtime_error{"expected exception containing: " + diagnostic};
}

std::string Join(std::vector<std::string_view> fields) {
    std::string result;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) result += ',';
        result += fields[i];
    }
    return result;
}

void EmptyDisablesFocus() {
    Require(!ParseA64RegionFocus(""), "the entire empty string must disable focus");
}

void MinimalFocusParsesExactDescriptors() {
    const auto focus = ParseA64RegionFocus(kValid);
    Require(focus.has_value(), "valid minimal focus was rejected");
    Require(focus->entry.Value() == 0xdead && focus->exit_block.Value() == 0xdeae &&
                focus->direct_target.Value() == 0xdeaf,
            "minimal descriptors were not parsed exactly");
    Require(focus->return_blocks.empty(), "minimal focus acquired a return block");
}

void ReturnsParseAndOwnTheirStorage() {
    const std::string text = Join({kEntry, kExit, kDirect,
                                   "0x0000000000000000", "0x0000000000000001",
                                   "0x7fffffffffffffff", "0x8000000000000000",
                                   "0xffffffffffffffff"});
    const auto focus = ParseA64RegionFocus(text);
    Require(focus.has_value(), "valid increasing returns were rejected");
    const std::vector<u64> expected{0, 1, 0x7fffffffffffffff, 0x8000000000000000,
                                    0xffffffffffffffff};
    Require(focus->return_blocks.size() == expected.size(), "wrong return count");
    for (size_t i = 0; i < expected.size(); ++i) {
        Require(focus->return_blocks[i].Value() == expected[i], "wrong return descriptor");
    }

    auto copy = focus;
    copy->return_blocks[0] = D{0xaaaa};
    copy->return_blocks.push_back(D{0xbbbb});
    Require(focus->return_blocks[0].Value() == 0 && focus->return_blocks.size() == expected.size(),
            "focus copies must deeply own return descriptors");
}

void BoundaryValuesParse() {
    const auto focus = ParseA64RegionFocus(
        "0x0000000000000000,0xffffffffffffffff,0x8000000000000000");
    Require(focus.has_value() && focus->entry.Value() == 0 &&
                focus->exit_block.Value() == 0xffffffffffffffff &&
                focus->direct_target.Value() == 0x8000000000000000,
            "u64 boundary descriptors were not parsed exactly");
}

void CrossRoleRepetitionParses() {
    const auto self_loop = ParseA64RegionFocus(
        "0x000000000000dead,0x000000000000dead,0x000000000000dead");
    Require(self_loop && self_loop->entry == self_loop->exit_block &&
                self_loop->entry == self_loop->direct_target,
            "a focused direct self-loop must permit repeated roles");

    const auto repeated_return = ParseA64RegionFocus(
        "0x000000000000dead,0x000000000000dead,0x000000000000dead,0x000000000000dead");
    Require(repeated_return && repeated_return->return_blocks.size() == 1 &&
                repeated_return->return_blocks[0] == repeated_return->entry,
            "a return may name the same descriptor as another focus role");
}

void LargeIncreasingReturnListParses() {
    std::string text{kValid};
    constexpr size_t count = 10000;
    for (size_t i = 0; i < count; ++i) {
        constexpr char digits[] = "0123456789abcdef";
        std::string field(18, '0');
        field[1] = 'x';
        size_t value = i;
        for (size_t position = field.size(); position-- > 2 && value != 0;) {
            field[position] = digits[value % 16];
            value /= 16;
        }
        text += ',';
        text += field;
    }
    const auto focus = ParseA64RegionFocus(text);
    Require(focus && focus->return_blocks.size() == count,
            "10,000 strictly increasing returns were rejected");
    for (size_t i = 0; i < count; ++i) {
        Require(focus->return_blocks[i].Value() == i, "large return list changed descriptor order");
    }
}

void MalformedGrammarFailsClosed() {
    const std::vector<std::string> malformed{
        " ", "\t", "\n", "0x000000000000dead",
        "0x000000000000dead,0x000000000000deae",
        ",0x000000000000dead,0x000000000000deae,0x000000000000deaf",
        "0x000000000000dead,,0x000000000000deae,0x000000000000deaf",
        "0x000000000000dead,0x000000000000deae,0x000000000000deaf,",
        "0x000000000000dead,0x000000000000deae,,0x000000000000deaf",
        " 0x000000000000dead,0x000000000000deae,0x000000000000deaf",
        "0x000000000000dead ,0x000000000000deae,0x000000000000deaf",
        "0x000000000000dead, 0x000000000000deae,0x000000000000deaf",
        "0x000000000000dead,0x000000000000deae,0x000000000000deaf ",
        "0x000000000000dead,0x000000000000deae,0x000000000000deaf\n",
        "0x000000000000dead;0x000000000000deae;0x000000000000deaf",
        "0x000000000000dead,0x000000000000deae,0x000000000000deaf;",
        "+0x00000000000dead,0x000000000000deae,0x000000000000deaf",
        "-0x00000000000dead,0x000000000000deae,0x000000000000deaf",
        "0X000000000000dead,0x000000000000deae,0x000000000000deaf",
        "0x000000000000DEAD,0x000000000000deae,0x000000000000deaf",
        "0x000000000000deaD,0x000000000000deae,0x000000000000deaf",
        "0x000000000000dea,0x000000000000deae,0x000000000000deaf",
        "0x000000000000deadd,0x000000000000deae,0x000000000000deaf",
        "0000000000000dead,0x000000000000deae,0x000000000000deaf",
        "0x000000000000deag,0x000000000000deae,0x000000000000deaf",
        "0x000000000000dead,0x000000000000deae,0x000000000000deafjunk",
        "0x000000000000dead,0x000000000000deae,0x000000000000deaf,0x0000000000000000,0x0000000000000000",
        "0x000000000000dead,0x000000000000deae,0x000000000000deaf,0x0000000000000001,0x0000000000000000",
        "0x000000000000dead,0x000000000000deae,0x000000000000deaf,0xffffffffffffffff,0x0000000000000000",
    };
    for (const std::string& text : malformed) {
        Require(!ParseA64RegionFocus(text), "malformed focus was accepted: " + text);
    }

    const std::string with_nul = std::string{kValid} + std::string{"\0junk", 5};
    Require(!ParseA64RegionFocus(std::string_view{with_nul.data(), with_nul.size()}),
            "embedded NUL or trailing characters were accepted");
}

class PlainAddressSpace : public AddressSpace {
public:
    PlainAddressSpace() = default;

protected:
    IR::Block GenerateIR(D descriptor) const override { return IR::Block{descriptor}; }
    GuestRange RangeOf(const IR::Block& block) const override {
        return {block.Location().Value(), block.Location().Value() + 3};
    }
    u64 GuestPC(D location) const override { return location.Value(); }
    EmitConfig MakeEmitConfig() const override { return {}; }
    bool ChainLinkBlock() const override { return true; }
    bool RegionCompatible(D, D) const override { return true; }
};

class StaleFocusAddressSpace final : public PlainAddressSpace {
protected:
    std::optional<RegionSelectionFocus> FocusForEntry(D entry) const override {
        return RegionSelectionFocus{entry, D{0x1004}, D{0x1008}, {D{0x2000}}};
    }

    IR::Block GenerateIR(D descriptor) const override {
        IR::Block block{descriptor};
        if (descriptor.Value() < 0x1008) block.SetTerminal(IR::Term::LinkBlock{D{descriptor.Value() + 4}});
        return block;
    }
};

void AbsentBaseAndA64FocusStayOrdinary() {
    PlainAddressSpace plain;
    Require(!AddressSpaceTestAccess::Focus(plain, D{0x1000}), "base hook must be absent");

    const A64::UserConfig config{};
    const A64AddressSpace a64{config};
#if defined(DYNARMIC_JS_A64_REGION_FOCUS)
    const auto expected = ParseA64RegionFocus(DYNARMIC_JS_A64_REGION_FOCUS);
    Require(expected.has_value(), "configured test focus is invalid");
    auto selected = AddressSpaceTestAccess::Focus(a64, expected->entry);
    Require(selected && selected->entry == expected->entry &&
                selected->exit_block == expected->exit_block &&
                selected->direct_target == expected->direct_target &&
                selected->return_blocks.size() == expected->return_blocks.size(),
            "A64 exact entry did not receive its configured focus");
    selected->return_blocks.push_back(D{0x1234});
    const auto unchanged = AddressSpaceTestAccess::Focus(a64, expected->entry);
    Require(unchanged && unchanged->return_blocks.size() == expected->return_blocks.size(),
            "mutating a returned focus copy changed the A64 instance");
    const D nonentry{expected->entry.Value() ^ u64{1}};
    Require(!AddressSpaceTestAccess::Focus(a64, nonentry),
            "A64 returned focus for a non-entry descriptor");
    const D same_pc_alias{expected->entry.Value() ^ (u64{1} << 57)};
    Require(A64::LocationDescriptor{same_pc_alias}.PC() ==
                A64::LocationDescriptor{expected->entry}.PC(),
            "test alias must retain the PC");
    Require(!AddressSpaceTestAccess::Focus(a64, same_pc_alias),
            "same-PC/different-upper-bits descriptor must not match");
#else
    Require(!AddressSpaceTestAccess::Focus(a64, D{0xdead}),
            "default A64 configuration must not have focus");
#endif
}

void StaleFocusDoesNotFallBackToOrdinarySelection() {
    PlainAddressSpace ordinary;
    const auto ordinary_result = AddressSpaceTestAccess::GenerateRegion(ordinary, D{0x1000});
    Require(ordinary_result.first.size() == 1, "plain fixture changed");

    StaleFocusAddressSpace stale;
    Throws<std::runtime_error>(
        [&] { AddressSpaceTestAccess::GenerateRegion(stale, D{0x1000}); },
        "direct_target to return");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"empty disables", EmptyDisablesFocus},
        {"minimal exact descriptors", MinimalFocusParsesExactDescriptors},
        {"deep-owned returns", ReturnsParseAndOwnTheirStorage},
        {"u64 boundaries", BoundaryValuesParse},
        {"cross-role repetition", CrossRoleRepetitionParses},
        {"10,000 increasing returns", LargeIncreasingReturnListParses},
        {"malformed grammar", MalformedGrammarFailsClosed},
        {"absent/default/exact hook", AbsentBaseAndA64FocusStayOrdinary},
        {"stale focus has no ordinary fallback", StaleFocusDoesNotFallBackToOrdinarySelection},
    };
    size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) return 1;
    std::cout << "PASS " << tests.size() << " A64 region focus tests\n";
    return 0;
}
