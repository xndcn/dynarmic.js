// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/a64_region_focus.h"

#include <charconv>
#include <system_error>

namespace Dynarmic::Backend::Wasm {
namespace {

constexpr size_t kDescriptorLength = 18;

bool ParseDescriptor(std::string_view field, u64& value) {
    if (field.size() != kDescriptorLength || field.substr(0, 2) != "0x") return false;
    for (size_t i = 2; i < field.size(); ++i) {
        const char c = field[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    const char* const first = field.data() + 2;
    const char* const last = field.data() + field.size();
    const auto [end, error] = std::from_chars(first, last, value, 16);
    return error == std::errc{} && end == last;
}

}  // namespace

std::optional<RegionSelectionFocus> ParseA64RegionFocus(std::string_view text) {
    if (text.empty()) return std::nullopt;

    RegionSelectionFocus focus{IR::LocationDescriptor{0}, IR::LocationDescriptor{0},
                               IR::LocationDescriptor{0}, {}};
    size_t field_index = 0;
    size_t begin = 0;
    u64 previous_return = 0;
    while (true) {
        size_t end = text.find(',', begin);
        if (end == std::string_view::npos) end = text.size();
        u64 value = 0;
        if (!ParseDescriptor(text.substr(begin, end - begin), value)) return std::nullopt;

        const IR::LocationDescriptor descriptor{value};
        if (field_index == 0) {
            focus.entry = descriptor;
        } else if (field_index == 1) {
            focus.exit_block = descriptor;
        } else if (field_index == 2) {
            focus.direct_target = descriptor;
        } else {
            if (field_index > 3 && value <= previous_return) return std::nullopt;
            focus.return_blocks.push_back(descriptor);
            previous_return = value;
        }
        ++field_index;
        if (end == text.size()) break;
        begin = end + 1;
    }
    if (field_index < 3) return std::nullopt;
    return focus;
}

}  // namespace Dynarmic::Backend::Wasm
