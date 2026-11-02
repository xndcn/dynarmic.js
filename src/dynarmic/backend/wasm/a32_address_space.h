// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include "dynarmic/backend/wasm/address_space.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/interface/A32/config.h"

namespace Dynarmic::Backend::Wasm {

class A32AddressSpace final : public AddressSpace {
public:
    explicit A32AddressSpace(const A32::UserConfig& conf, ProfileCounters* profiler = nullptr,
                             CompileStats* compile_stats = nullptr,
                             RegionDiagnostics* region_diagnostics = nullptr,
                             const BoundaryState* boundary_state = nullptr,
                             bool specialize_boundary_hooks = false);
    ~A32AddressSpace() override;

    IR::Block GenerateIR(IR::LocationDescriptor descriptor) const override;
    void FoldConditionalBranch(IR::Block& block) const;

private:
    GuestRange RangeOf(const IR::Block& block) const override;
    u64 GuestPC(IR::LocationDescriptor location) const override { return A32::LocationDescriptor{location}.PC(); }
    EmitConfig MakeEmitConfig() const override;
    bool ChainLinkBlock() const override { return !conf.enable_cycle_counting; }
    bool RegionCompatible(IR::LocationDescriptor entry, IR::LocationDescriptor target) const override;

    const A32::UserConfig conf;
    const BoundaryState* boundary_state = nullptr;
    const bool specialize_boundary_hooks = false;
    // Flat guest window derived from fastmem_pointer + page_table (see EmitConfig).
    u32 flat_base = 0;
    u32 flat_limit = 0;
};

}  // namespace Dynarmic::Backend::Wasm
