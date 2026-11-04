// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <optional>

#include "dynarmic/backend/wasm/address_space.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/config.h"

namespace Dynarmic::Backend::Wasm {

class A64AddressSpace final : public AddressSpace {
public:
    explicit A64AddressSpace(const A64::UserConfig& conf, ProfileCounters* profiler = nullptr,
                             CompileStats* compile_stats = nullptr,
                             RegionDiagnostics* region_diagnostics = nullptr,
                             bool enable_fallback_multi_entry = false,
                             const BoundaryState* boundary_state = nullptr);
    ~A64AddressSpace() override;

    IR::Block GenerateIR(IR::LocationDescriptor descriptor) const override;

private:
    GuestRange RangeOf(const IR::Block& block) const override;
    u64 GuestPC(IR::LocationDescriptor location) const override { return A64::LocationDescriptor{location}.PC(); }
    EmitConfig MakeEmitConfig() const override;
    bool ChainLinkBlock() const override { return !conf.enable_cycle_counting; }
    bool RegionCompatible(IR::LocationDescriptor entry, IR::LocationDescriptor target) const override;
    std::optional<RegionSelectionFocus> FocusForEntry(IR::LocationDescriptor entry) const override;

    const A64::UserConfig conf;
    const bool enable_fallback_multi_entry;
    const BoundaryState* boundary_state = nullptr;
    const std::optional<RegionSelectionFocus> region_focus;
    // Flat guest window derived from fastmem_pointer + page_table (see EmitConfig).
    u32 flat_base = 0;
    u32 flat_limit = 0;
};

}  // namespace Dynarmic::Backend::Wasm
