// Records the reviewed region/Wasm regression corpus. This tool uses
// the real address-space selector and emitter, but never publishes or runs code.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dynarmic/backend/wasm/a32_address_space.h"
#include "dynarmic/backend/wasm/a64_address_space.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"

namespace Dynarmic::Backend::Wasm {

struct AddressSpaceTestAccess {
    static std::pair<std::vector<IR::Block>, size_t> GenerateRegion(
            const AddressSpace& space, IR::LocationDescriptor descriptor) {
        return space.GenerateRegion(descriptor);
    }

    static EmitConfig MakeEmitConfig(const AddressSpace& space) {
        return space.MakeEmitConfig();
    }
};

}  // namespace Dynarmic::Backend::Wasm

namespace {

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;

constexpr u32 kLinkSlotsBase = 0x100000;
constexpr u32 kFastDispatchBase = 0x200000;
constexpr u32 kBrk = 0xd4200000;
constexpr u32 kRet = 0xd65f03c0;

using GuestImage = std::map<u64, u32>;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error{message};
}

constexpr u32 Branch(u64 from, u64 to) {
    return 0x14000000u | (static_cast<u32>((to - from) >> 2) & 0x03ffffffu);
}

constexpr u32 BranchCond(u64 from, u64 to, u32 condition) {
    return 0x54000000u | ((static_cast<u32>((to - from) >> 2) & 0x7ffffu) << 5) | condition;
}

constexpr u32 BranchLink(u64 from, u64 to) {
    return 0x94000000u | (static_cast<u32>((to - from) >> 2) & 0x03ffffffu);
}

constexpr u32 A32Branch(u64 from, u64 to) {
    return 0xea000000u | (static_cast<u32>((to - (from + 8)) >> 2) & 0x00ffffffu);
}

constexpr u32 A32BranchCond(u64 from, u64 to, u32 condition) {
    return ((condition & 0xfu) << 28) | 0x0a000000u |
           (static_cast<u32>((to - (from + 8)) >> 2) & 0x00ffffffu);
}

class Callbacks32 final : public A32::UserCallbacks {
public:
    explicit Callbacks32(GuestImage image) : image(std::move(image)) {}

    std::optional<u32> MemoryReadCode(u32 address) override {
        const auto it = image.find(address);
        if (it == image.end()) return std::nullopt;
        return it->second;
    }
    u8 MemoryRead8(u32) override { return 0; }
    u16 MemoryRead16(u32) override { return 0; }
    u32 MemoryRead32(u32 address) override { return MemoryReadCode(address).value_or(0); }
    u64 MemoryRead64(u32) override { return 0; }
    void MemoryWrite8(u32, u8) override {}
    void MemoryWrite16(u32, u16) override {}
    void MemoryWrite32(u32, u32) override {}
    void MemoryWrite64(u32, u64) override {}
    void InterpreterFallback(u32, size_t) override { throw std::runtime_error{"unexpected A32 interpreter fallback"}; }
    void CallSVC(u32) override { throw std::runtime_error{"unexpected A32 SVC"}; }
    void ExceptionRaised(u32, A32::Exception) override { throw std::runtime_error{"unexpected A32 exception"}; }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }

private:
    const GuestImage image;
};

class Callbacks64 final : public A64::UserCallbacks {
public:
    explicit Callbacks64(GuestImage image) : image(std::move(image)) {}

    std::optional<u32> MemoryReadCode(u64 address) override {
        const auto it = image.find(address);
        if (it == image.end()) return std::nullopt;
        return it->second;
    }
    u8 MemoryRead8(u64) override { return 0; }
    u16 MemoryRead16(u64) override { return 0; }
    u32 MemoryRead32(u64 address) override { return MemoryReadCode(address).value_or(0); }
    u64 MemoryRead64(u64) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, u16) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64, size_t) override { throw std::runtime_error{"unexpected A64 interpreter fallback"}; }
    void CallSVC(u32) override { throw std::runtime_error{"unexpected A64 SVC"}; }
    void ExceptionRaised(u64, A64::Exception) override { throw std::runtime_error{"unexpected A64 exception"}; }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }
    u64 GetCNTPCT() override { return 0; }

private:
    const GuestImage image;
};

IR::LocationDescriptor A32Location(u32 pc) {
    return A32::LocationDescriptor{pc, A32::PSR{}, A32::FPSCR{}};
}

IR::LocationDescriptor A64Location(u64 pc) {
    return A64::LocationDescriptor{pc, FP::FPCR{}};
}

A32::UserConfig ConfigFor(A32::UserCallbacks& callbacks) {
    A32::UserConfig config;
    config.callbacks = &callbacks;
    config.enable_cycle_counting = false;
    return config;
}

A64::UserConfig ConfigFor(A64::UserCallbacks& callbacks) {
    A64::UserConfig config;
    config.callbacks = &callbacks;
    config.enable_cycle_counting = false;
    return config;
}

struct RecordedCase {
    std::string name;
    u64 entry_descriptor;
    std::vector<u64> descriptors;
    size_t entry_index;
    std::vector<u8> wasm;
};

RecordedCase RecordCase(std::string name, const AddressSpace& space, IR::LocationDescriptor entry) {
    auto [blocks, entry_index] = AddressSpaceTestAccess::GenerateRegion(space, entry);
    Require(!blocks.empty() && entry_index < blocks.size(), name + ": invalid selected region");
    Require(blocks[entry_index].Location().Value() == entry.Value(), name + ": entry index does not identify the entry");

    RecordedCase result{std::move(name), entry.Value(), {}, entry_index, {}};
    for (const auto& block : blocks) result.descriptors.push_back(block.Location().Value());

    auto [repeat_blocks, repeat_entry_index] = AddressSpaceTestAccess::GenerateRegion(space, entry);
    Require(repeat_entry_index == entry_index && repeat_blocks.size() == blocks.size(),
            result.name + ": nondeterministic region selection");
    for (size_t index = 0; index < blocks.size(); ++index) {
        Require(repeat_blocks[index].Location().Value() == result.descriptors[index],
                result.name + ": nondeterministic descriptor order");
    }

    EmitConfig config = AddressSpaceTestAccess::MakeEmitConfig(space);
    config.link_slots_base = kLinkSlotsBase;
    config.fast_dispatch_base = kFastDispatchBase;
    config.function_names = false;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    config.transfer_record = nullptr;
#endif
    const EmittedBlockInfo first = EmitWASM(std::move(blocks), config, entry_index);
    const EmittedBlockInfo second = EmitWASM(std::move(repeat_blocks), config, entry_index);
    Require(first.module_bytes == second.module_bytes, result.name + ": nondeterministic emission");
    constexpr std::array<u8, 8> wasm_header{0, 97, 115, 109, 1, 0, 0, 0};
    Require(first.module_bytes.size() >= wasm_header.size() &&
                std::equal(wasm_header.begin(), wasm_header.end(), first.module_bytes.begin()),
            result.name + ": emitter returned an invalid Wasm header");
    result.wasm = first.module_bytes;
    return result;
}

GuestImage MaximumChainImage() {
    GuestImage image;
    constexpr u64 base = 0x4000;
    for (u64 index = 0; index < 65; ++index) {
        const u64 pc = base + index * 4;
        image.emplace(pc, index < 64 ? Branch(pc, pc + 4) : kBrk);
    }
    return image;
}

std::vector<RecordedCase> RecordCorpus() {
    std::vector<RecordedCase> cases;
    {
        Require(A32BranchCond(0x2004, 0x1000, 1) == 0x1afffbfd,
                "A32 BNE must use condition bits 31..28 and a PC+8 displacement");
        Require(A32Branch(0x1000, 0x2008) == 0xea000400,
                "A32 B must use a PC+8 displacement");
        Callbacks32 callbacks{GuestImage{
            {0x1000, A32Branch(0x1000, 0x2008)},
            {0x2000, A32BranchCond(0x2000, 0x2008, 0)},  // beq 0x2008
            {0x2004, A32BranchCond(0x2004, 0x1000, 1)},  // bne 0x1000
            {0x2008, 0xe12fff1e},  // bx lr
        }};
        const auto config = ConfigFor(callbacks);
        const A32AddressSpace space{config};
        cases.push_back(RecordCase("a32_conditional_backward", space, A32Location(0x2000)));
    }
    {
        Callbacks64 callbacks{GuestImage{{0x1000, kBrk}}};
        const auto config = ConfigFor(callbacks);
        const A64AddressSpace space{config};
        cases.push_back(RecordCase("a64_single_block", space, A64Location(0x1000)));
    }
    {
        Callbacks64 callbacks{GuestImage{
            {0x1000, Branch(0x1000, 0x2004)},
            {0x2000, BranchCond(0x2000, 0x1000, 1)},  // b.ne 0x1000
            {0x2004, kBrk},
        }};
        const auto config = ConfigFor(callbacks);
        const A64AddressSpace space{config};
        cases.push_back(RecordCase("a64_backward_conditional", space, A64Location(0x2000)));
    }
    {
        Callbacks64 callbacks{GuestImage{
            {0x3000, BranchLink(0x3000, 0x3010)},
            {0x3004, kBrk},
            {0x3010, kRet},
        }};
        const auto config = ConfigFor(callbacks);
        const A64AddressSpace space{config};
        cases.push_back(RecordCase("a64_bl_ret_push_rsb", space, A64Location(0x3000)));
    }
    {
        Callbacks64 callbacks{MaximumChainImage()};
        const auto config = ConfigFor(callbacks);
        const A64AddressSpace space{config};
        cases.push_back(RecordCase("a64_max_64_blocks", space, A64Location(0x4000)));
    }
    const std::vector<u64> expected_a32{0x1000, 0x2000, 0x2004, 0x2008};
    Require(cases[0].descriptors == expected_a32,
            "A32 conditional/backward fixture must select only valid mapped flow: blocks=" +
                std::to_string(cases[0].descriptors.size()));
    Require(cases[1].descriptors.size() == 1, "A64 single-block fixture became a region");
    Require(cases[2].descriptors.size() == 3 && cases[2].entry_index != 0,
            "A64 backward fixture lost its nonzero sorted entry index");
    Require(cases[3].descriptors.size() == 3, "A64 BL/RET fixture lost its PushRSB continuation");
    Require(cases[4].descriptors.size() == 64, "A64 chain did not hit the 64-block limit");
    return cases;
}

std::string Hex64(u64 value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

void WriteModule(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream out{path, std::ios::binary};
    Require(out.is_open(), "cannot create " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    Require(out.good(), "cannot write " + path.string());
}

void WriteManifest(const std::filesystem::path& path, const std::vector<RecordedCase>& cases,
                   const std::string& source_revision) {
    std::ofstream out{path};
    Require(out.is_open(), "cannot create " + path.string());
    out << "{\n"
        << "  \"schema\": \"dynarmic.js/region-corpus-v1\",\n"
        << "  \"source_revision\": " << (source_revision == "source-archive" ? "null" : "\"" + source_revision + "\"") << ",\n"
        << "  \"emission\": {\n"
        << "    \"link_slots_base\": " << kLinkSlotsBase << ",\n"
        << "    \"fast_dispatch_base\": " << kFastDispatchBase << ",\n"
        << "    \"function_names\": false,\n"
        << "    \"diagnostics\": false\n"
        << "  },\n"
        << "  \"cases\": [\n";
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index];
        out << "    {\n"
            << "      \"name\": \"" << item.name << "\",\n"
            << "      \"entry_descriptor\": \"" << Hex64(item.entry_descriptor) << "\",\n"
            << "      \"descriptors\": [\n";
        for (size_t descriptor = 0; descriptor < item.descriptors.size(); ++descriptor) {
            out << "        \"" << Hex64(item.descriptors[descriptor]) << "\""
                << (descriptor + 1 == item.descriptors.size() ? "\n" : ",\n");
        }
        out << "      ],\n"
            << "      \"entry_index\": " << item.entry_index << ",\n"
            << "      \"wasm\": \"" << item.name << ".wasm\",\n"
            << "      \"wasm_bytes\": " << item.wasm.size() << "\n"
            << "    }" << (index + 1 == cases.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    out.close();
    Require(out.good(), "cannot write " + path.string());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 || std::string{argv[1]} != "--output" || std::string{argv[3]} != "--source-revision") {
        std::cerr << "usage: region_corpus --output NEW_DIRECTORY --source-revision GIT_HASH_OR_source-archive\n";
        return 2;
    }
    try {
        const std::filesystem::path output{argv[2]};
        const std::string source_revision{argv[4]};
        Require(source_revision == "source-archive" ||
                    (source_revision.size() == 40 && source_revision.find_first_not_of("0123456789abcdef") == std::string::npos),
                "source revision must be a lowercase full Git hash or source-archive");
        Require(!std::filesystem::exists(output), "refusing stale output directory: " + output.string());
        const auto cases = RecordCorpus();
        Require(std::filesystem::create_directory(output), "output directory already exists: " + output.string());
        for (const auto& item : cases) {
            WriteModule(output / (item.name + ".wasm"), item.wasm);
            std::cout << "recorded " << item.name << " blocks=" << item.descriptors.size()
                      << " entry_index=" << item.entry_index << " bytes=" << item.wasm.size() << '\n';
        }
        WriteManifest(output / "manifest.json", cases, source_revision);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "region_corpus: " << error.what() << '\n';
        return 1;
    }
}
