// Offline static-gate emitter. This measurement tool is not part of the JIT.
#define main block_dump_unused_main
#include "../../../tests/tools/block_dump.cpp"
#undef main

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <unordered_map>

#include "dynarmic/backend/wasm/region_plan.h"
#include "dynarmic/ir/opcodes.h"

namespace {

const std::vector<u64> baseline{
    0x12020, 0x1202c, 0x12044, 0x1204c, 0x1205c, 0x12068, 0x12074, 0x12080,
    0x12084, 0x1208c, 0x12090, 0x1209c, 0x120b4, 0x120b8, 0x120c4, 0x120d0,
    0x120dc, 0x120e8, 0x120f4, 0x12104, 0x12114, 0x12124, 0x12134, 0x121fc,
    0x1220c, 0x12218, 0x12220, 0x12228, 0x12230, 0x12238, 0x12240, 0x12268,
    0x1226c, 0x12274, 0x1227c, 0x122a0, 0x122a8, 0x122b0, 0x122c0, 0x122cc,
    0x122d8, 0x122e0, 0x122f0, 0x122f8, 0x12308, 0x12320, 0x12330, 0x1233c,
    0x12350, 0x12354, 0x12358, 0x1236c, 0x12370, 0x12398, 0x1239c, 0x123a4,
    0x123ac, 0x123b0, 0x123c8, 0x123e0, 0x123ec, 0x12424, 0x12428, 0x125e0,
};
const std::vector<u64> candidate{
    0x12020, 0x1202c, 0x12044, 0x1204c, 0x1205c, 0x12068, 0x12074, 0x12080,
    0x12084, 0x1208c, 0x12090, 0x1209c, 0x120b4, 0x120b8, 0x120c4, 0x120d0,
    0x120dc, 0x120e8, 0x120f4, 0x12104, 0x12114, 0x12124, 0x12134, 0x121fc,
    0x1220c, 0x12218, 0x12220, 0x12228, 0x12230, 0x12238, 0x12240, 0x12268,
    0x1226c, 0x12274, 0x1227c, 0x122a0, 0x122a8, 0x122b0, 0x122c0, 0x122cc,
    0x122d8, 0x122e0, 0x122f0, 0x122f8, 0x12308, 0x12320, 0x12330, 0x1233c,
    0x12354, 0x12358, 0x1236c, 0x12370, 0x1237c, 0x1239c, 0x123a4, 0x123ac,
    0x123b0, 0x123c8, 0x123e0, 0x123ec, 0x12418, 0x12424, 0x12428, 0x125e0,
};

struct LayoutEdges {
    const std::unordered_map<u64, u32>& indices;
    const std::vector<u32>& return_sites;
    u32 self;
    std::vector<u32>& out;

    void Add(Dynarmic::IR::LocationDescriptor next) {
        const auto it = indices.find(next.Value());
        if (it != indices.end() && it->second != self) out.push_back(it->second);
    }
    void AddReturnSites() {
        for (u32 site : return_sites) if (site != self) out.push_back(site);
    }
    void operator()(const Dynarmic::IR::Term::LinkBlock& term) { Add(term.next); }
    void operator()(const Dynarmic::IR::Term::LinkBlockFast& term) { Add(term.next); }
    void operator()(const Dynarmic::IR::Term::PopRSBHint&) { AddReturnSites(); }
    void operator()(const Dynarmic::IR::Term::FastDispatchHint&) { AddReturnSites(); }
    void operator()(const Dynarmic::IR::Term::If& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const Dynarmic::IR::Term::CheckBit& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const Dynarmic::IR::Term::CheckHalt& term) { boost::apply_visitor(*this, term.else_); }
    template<typename T> void operator()(const T&) {}
};

std::string LayoutOf(const std::vector<Dynarmic::IR::Block>& blocks) {
    std::unordered_map<u64, u32> indices;
    for (u32 i = 0; i < blocks.size(); ++i) indices.emplace(blocks[i].Location().Value(), i);
    std::vector<u32> return_sites;
    for (const auto& block : blocks) {
        for (const auto& inst : block) {
            if (inst.GetOpcode() == Dynarmic::IR::Opcode::PushRSB) {
                const auto it = indices.find(inst.GetArg(0).GetU64());
                if (it != indices.end()) return_sites.push_back(it->second);
            }
        }
    }
    Dynarmic::Backend::Wasm::RegionGraph graph(blocks.size());
    for (u32 i = 0; i < blocks.size(); ++i) {
        LayoutEdges collector{indices, return_sites, i, graph[i]};
        if (blocks[i].HasConditionFailedLocation()) collector.Add(blocks[i].ConditionFailedLocation());
        boost::apply_visitor(collector, blocks[i].GetTerminal());
    }
    return Dynarmic::Backend::Wasm::PlanRegion(graph, 1) ? "structured" : "fallback";
}

struct Result {
    size_t blocks = 0;
    std::vector<double> milliseconds;
    std::vector<u8> bytes;
    size_t entry_body_bytes = 0;
    std::string layout = "unavailable";
};

bool WriteModule(const std::filesystem::path& output, const std::vector<u8>& bytes) {
    std::ofstream out{output, std::ios::binary};
    if (!out.is_open()) {
        std::cerr << "failed to open output module: " << output << '\n';
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::cerr << "failed to write output module: " << output << '\n';
        out.close();
        return false;
    }
    out.close();
    if (!out) {
        std::cerr << "failed to close output module: " << output << '\n';
        return false;
    }
    return true;
}

Result EmitTrials(const std::vector<u64>& descriptors,
                  const Dynarmic::Backend::Wasm::A64AddressSpace& space,
                  const Dynarmic::A64::UserConfig& conf,
                  const std::filesystem::path& output) {
    using Clock = std::chrono::steady_clock;
    Result result;
    result.blocks = descriptors.size();
    for (int trial = 0; trial < 20; ++trial) {
        std::vector<Dynarmic::IR::Block> blocks;
        blocks.reserve(descriptors.size());
        for (u64 pc : descriptors) {
            blocks.push_back(space.GenerateIR(Dynarmic::A64::LocationDescriptor{pc, Dynarmic::FP::FPCR{}}));
        }
        Dynarmic::Backend::Wasm::EmitConfig emit_conf{false, true};
        emit_conf.frontend = Dynarmic::Backend::Wasm::Frontend::A64;
        emit_conf.a64_conf = &conf;
        if (result.bytes.empty()) result.layout = LayoutOf(blocks);
        // Translation is deliberately complete before the emission-only timer.
        const auto start = Clock::now();
        auto emitted = Dynarmic::Backend::Wasm::EmitWASM(std::move(blocks), emit_conf, 1);
        const auto stop = Clock::now();
        result.milliseconds.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        if (result.bytes.empty()) {
            result.bytes = emitted.module_bytes;
            result.entry_body_bytes = emitted.entry_body_bytes;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
            result.layout = emitted.region_layout == Dynarmic::Backend::Wasm::RegionDiagnosticLayout::Fallback
                                ? "fallback" : "structured";
#endif
            if (!WriteModule(output, emitted.module_bytes)) std::exit(1);
        } else {
            if (result.bytes != emitted.module_bytes || result.entry_body_bytes != emitted.entry_body_bytes) {
                std::cerr << "nondeterministic emitted module\n";
                std::exit(1);
            }
        }
    }
    return result;
}

void Print(const char* name, const Result& result) {
    auto sorted = result.milliseconds;
    std::sort(sorted.begin(), sorted.end());
    std::cout << "  \"" << name << "\": {\n"
              << "    \"blocks\": " << result.blocks << ",\n"
              << "    \"module_bytes\": " << result.bytes.size() << ",\n"
              << "    \"entry_body_bytes\": " << result.entry_body_bytes << ",\n"
              << "    \"layout\": \"" << result.layout << "\",\n"
              << "    \"native_emit_ms_upper_median_of_20\": " << sorted[10] << ",\n"
              << "    \"native_emit_ms_all\": [";
    for (size_t i = 0; i < result.milliseconds.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << result.milliseconds[i];
    }
    std::cout << "]\n  }";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: emit_probe <a64-flat.bin@base_hex> <existing-output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory{argv[2]};
    if (!std::filesystem::is_directory(output_directory)) {
        std::cerr << "output directory does not exist\n";
        return 2;
    }
    for (const char* name : {"baseline.wasm", "candidate.wasm"}) {
        const auto output = output_directory / name;
        if (std::filesystem::exists(output)) {
            std::cerr << "refusing to overwrite output module: " << output << '\n';
            return 2;
        }
    }
    std::vector<u8> image;
    Dynarmic::A32::VAddr base = 0;
    if (!LoadProgram(argv[1], image, base)) return 1;
    static Callbacks callbacks{std::move(image), base};
    Dynarmic::A32::UserConfig conf;
    conf.callbacks = &callbacks;
    static Callbacks64 callbacks64{callbacks};
    Dynarmic::A64::UserConfig conf64;
    conf64.callbacks = &callbacks64;
    const Dynarmic::Backend::Wasm::A64AddressSpace space{conf64};
    std::cout << std::setprecision(12) << "{\n";
    const auto a = EmitTrials(baseline, space, conf64, output_directory / "baseline.wasm");
    std::cout << "  \"configuration\": \"native offline emitter, no inline memory, no chaining slots; EmitWASM only; entry index 1\",\n";
    Print("baseline", a);
    std::cout << ",\n";
    const auto b = EmitTrials(candidate, space, conf64, output_directory / "candidate.wasm");
    Print("candidate", b);
    std::cout << "\n}\n";
    return 0;
}
