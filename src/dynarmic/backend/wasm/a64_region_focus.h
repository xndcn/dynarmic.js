// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <optional>
#include <string_view>

#include "dynarmic/backend/wasm/region_selector.h"

namespace Dynarmic::Backend::Wasm {

// Empty returns nullopt. Malformed values also return nullopt; build-time users
// must reject nullopt for a nonempty value rather than falling back.
std::optional<RegionSelectionFocus> ParseA64RegionFocus(std::string_view text);

}  // namespace Dynarmic::Backend::Wasm
