#include <cstdint>
#include <cstdio>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
static FILE* g_log = nullptr;
static u32 g_acc = 2166136261u;
static void mix(u32 v) {
    if (g_log) std::fprintf(g_log, "%08x\n", v);
    g_acc = (g_acc ^ v) * 16777619u;
}
static u64 lane(const u32* a, int i) { return (u64)a[2 * i] | ((u64)a[2 * i + 1] << 32); }
static void op_vqmovn_s64(const u32* a, u32* r) {
    for (int i = 0; i < 2; i++) {
        s64 x = (s64)lane(a, i);
        r[i] = x > 0x7FFFFFFFLL ? 0x7FFFFFFFu : x < -0x80000000LL ? 0x80000000u : (u32)x;
    }
}
static void op_vqmovn_u64(const u32* a, u32* r) {
    for (int i = 0; i < 2; i++) {
        u64 x = lane(a, i);
        r[i] = x > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (u32)x;
    }
}
static void op_vqmovun_s64(const u32* a, u32* r) {
    for (int i = 0; i < 2; i++) {
        s64 x = (s64)lane(a, i);
        r[i] = x < 0 ? 0u : x > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (u32)x;
    }
}
#include "neon6_body.inc"
int main() {
    g_log = std::fopen("ref_mix.log", "w");
    test_body();
    if (g_log) std::fclose(g_log);
    std::printf("checksum %08x\n", g_acc);
}
