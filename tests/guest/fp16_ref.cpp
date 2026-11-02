// Host reference for fp16.c: calls FP::FPConvert from libdynarmic directly
// with FPCR=0 and ToNearest_TieEven, accumulating FPSR exception bits (0x9F)
// exactly as the wasm thunk does. Must print the same checksum as the guest.

#include <cstdint>
#include <cstdio>

static FILE* g_log = nullptr;

#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op.h"

using u32 = std::uint32_t;
using u64 = std::uint64_t;

static u32 g_acc = 2166136261u;
static void mix(u32 v) {
    if (g_log) std::fprintf(g_log, "%08x\n", v);
    g_acc = (g_acc ^ v) * 16777619u;
}

static u32 g_fpsr_exc = 0;

template<typename To, typename From>
static To cvt(u64 value) {
    Dynarmic::FP::FPSR fpsr{};
    const To result = Dynarmic::FP::FPConvert<To, From>(
        static_cast<From>(value), Dynarmic::FP::FPCR{0},
        Dynarmic::FP::RoundingMode::ToNearest_TieEven, fpsr);
    g_fpsr_exc |= fpsr.Value() & 0x9F;
    return result;
}

static const u32 HV[] = {
    0x0000u, 0x8000u, 0x0001u, 0x03FFu, 0x0400u, 0x3C00u,
    0xC000u, 0x3555u, 0x7BFFu, 0x7C00u, 0xFC00u, 0x7E00u,
};
static const u32 FV[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,
    0x00000001u, 0x33800000u, 0x3F800001u, 0x3EAAAAABu,
    0x477FE000u, 0x477FF000u, 0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0x7F800001u,
};
static const u64 DV[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL,
    0x3FF0000000000000ULL, 0xBFF0000000000000ULL,
    0x0000000000000001ULL, 0x3FF0000000000001ULL,
    0x3FD5555555555555ULL, 0x40EFFFC000000000ULL,
    0x40F0000000000000ULL, 0x7FF0000000000000ULL,
    0xFFF0000000000000ULL, 0x7FF8000000000000ULL,
    0x7FF0000000000001ULL,
};

int main() {
    g_log = std::fopen("ref_mix.log", "w");
    // VCVTT converts the same 16 input bits as VCVTB here (guest pre-shifts
    // them into the top half), so b and t variants produce identical values.
    for (u32 h : HV) {
        mix(cvt<u32, std::uint16_t>(h));
        mix(cvt<u32, std::uint16_t>(h));
        const u64 d0 = cvt<u64, std::uint16_t>(h);
        mix(static_cast<u32>(d0));
        mix(static_cast<u32>(d0 >> 32));
        const u64 d1 = cvt<u64, std::uint16_t>(h);
        mix(static_cast<u32>(d1));
        mix(static_cast<u32>(d1 >> 32));
    }
    // To-half conversions preserve the other half of the destination S
    // register; the guest's dst aliases the source register, so the preserved
    // half comes from the input value (low 32 bits for f64 sources).
    for (u32 f : FV) {
        const u32 h = cvt<std::uint16_t, u32>(f);
        mix((f & 0xFFFF0000u) | h);        // vcvtb.f16.f32
        mix((f & 0x0000FFFFu) | (h << 16)); // vcvtt.f16.f32
    }
    for (u64 d : DV) {
        const u32 lo = static_cast<u32>(d);
        const u32 h = cvt<std::uint16_t, u64>(d);
        mix((lo & 0xFFFF0000u) | h);        // vcvtb.f16.f64
        mix((lo & 0x0000FFFFu) | (h << 16)); // vcvtt.f16.f64
    }
    mix(g_fpsr_exc);
    if (g_log) std::fclose(g_log);
    std::printf("checksum %08x\n", g_acc);
    return 0;
}
