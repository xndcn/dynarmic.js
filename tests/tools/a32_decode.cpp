// Show which dynarmic A32 decoder entry matches each instruction word, in the
// order the frontend tries them (VFP, ASIMD, then ARM / Thumb). Handy for
// checking an encoding before blaming the backend.
//
// usage: a32_decode [--thumb] <hex>...
//   ARM:    a32_decode e5933000          -> arm_LDR_imm
//   Thumb:  a32_decode --thumb 4408 f8d13000
//           (16-bit halfwords as 4 digits; 32-bit as first halfword then second)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <mcl/bit/bit_field.hpp>

#include "dynarmic/frontend/A32/decoder/arm.h"
#include "dynarmic/frontend/A32/decoder/asimd.h"
#include "dynarmic/frontend/A32/decoder/thumb16.h"
#include "dynarmic/frontend/A32/decoder/thumb32.h"
#include "dynarmic/frontend/A32/decoder/vfp.h"
#include "dynarmic/frontend/A32/translate/impl/a32_translate_impl.h"

using namespace Dynarmic;
using namespace Dynarmic::A32;

namespace {

using V = TranslatorVisitor;

// Mirrors the helpers private to translate_thumb.cpp.
u32 ConvertASIMDInstruction(u32 thumb_instruction) {
    if ((thumb_instruction & 0xEF000000) == 0xEF000000) {
        const bool U = mcl::bit::get_bit<28>(thumb_instruction);
        return 0xF2000000 | (U << 24) | (thumb_instruction & 0x00FFFFFF);
    }
    if ((thumb_instruction & 0xFF000000) == 0xF9000000) {
        return 0xF4000000 | (thumb_instruction & 0x00FFFFFF);
    }
    return 0xF7F0A000;  // UDF
}

bool MaybeVFPOrASIMDInstruction(u32 thumb_instruction) {
    return (thumb_instruction & 0xEC000000) == 0xEC000000 || (thumb_instruction & 0xFF100000) == 0xF9000000;
}

const char* DecodeArmWord(u32 insn) {
    if (const auto d = DecodeVFP<V>(insn)) {
        return d->get().GetName();
    }
    if (const auto d = DecodeASIMD<V>(insn)) {
        return d->get().GetName();
    }
    if (const auto d = DecodeArm<V>(insn)) {
        return d->get().GetName();
    }
    return nullptr;
}

const char* DecodeThumbWord(u32 insn, bool is_16bit) {
    if (is_16bit) {
        if (const auto d = DecodeThumb16<V>(static_cast<u16>(insn))) {
            return d->get().GetName();
        }
        return nullptr;
    }
    if (MaybeVFPOrASIMDInstruction(insn)) {
        if (const auto d = DecodeVFP<V>(insn)) {
            return d->get().GetName();
        }
        if (const auto d = DecodeASIMD<V>(ConvertASIMDInstruction(insn))) {
            return d->get().GetName();
        }
    }
    if (const auto d = DecodeThumb32<V>(insn)) {
        return d->get().GetName();
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    bool thumb = false;
    int first = 1;
    if (argc > 1 && std::strcmp(argv[1], "--thumb") == 0) {
        thumb = true;
        first = 2;
    }
    if (first >= argc) {
        std::fprintf(stderr, "usage: %s [--thumb] <hex>...\n", argv[0]);
        return 2;
    }

    int unmatched = 0;
    for (int i = first; i < argc; i++) {
        const std::string text = argv[i];
        const u32 insn = static_cast<u32>(std::strtoul(text.c_str(), nullptr, 16));
        const bool is_16bit = thumb && text.size() <= 4;
        const char* name = thumb ? DecodeThumbWord(insn, is_16bit) : DecodeArmWord(insn);
        std::printf(is_16bit ? "%04x      %s\n" : "%08x  %s\n", insn, name ? name : "(no match: UDF)");
        unmatched += name == nullptr;
    }
    return unmatched ? 1 : 0;
}
