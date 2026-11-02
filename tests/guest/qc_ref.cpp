// Host reference for qc.c: lane-wise exact arithmetic with explicit saturation detection.
#include <cstdint>
#include <cstdio>
#include <cstring>
typedef uint32_t u32;
typedef uint64_t u64;
typedef __int128 i128;
typedef unsigned __int128 u128;
static u32 g_acc = 2166136261u;
static FILE* g_log;
static void mix(u32 v) { fprintf(g_log, "%08x\n", v); g_acc = (g_acc ^ v) * 16777619u; }

static const u32 QC = 1u << 27;
static i128 lane_s(const u32* v, int bits, int i) {
    u64 x = 0; std::memcpy(&x, (const uint8_t*)v + i * bits / 8, bits / 8);
    if (bits < 64) { u64 m = u64{1} << (bits - 1); return (i128)(int64_t)((x ^ m) - m); }
    return (i128)(int64_t)x;
}
static i128 lane_u(const u32* v, int bits, int i) {
    u64 x = 0; std::memcpy(&x, (const uint8_t*)v + i * bits / 8, bits / 8); return (i128)x;
}
static void put(u32* v, int bits, int i, i128 x) { u64 y = (u64)x; std::memcpy((uint8_t*)v + i * bits / 8, &y, bits / 8); }
static i128 smax(int b) { return ((i128)1 << (b - 1)) - 1; }
static i128 smin(int b) { return -((i128)1 << (b - 1)); }
static i128 umax(int b) { return ((i128)1 << b) - 1; }
static i128 clampv(i128 x, i128 lo, i128 hi, bool& sat) { if (x < lo) { sat = true; return lo; } if (x > hi) { sat = true; return hi; } return x; }

template<int B, bool S, bool SUB> static u32 addsub(const u32* a, const u32* b, u32* r) {
    bool sat = false;
    for (int i = 0; i < 128 / B; i++) {
        i128 x = S ? lane_s(a, B, i) : lane_u(a, B, i), y = S ? lane_s(b, B, i) : lane_u(b, B, i);
        i128 e = SUB ? x - y : x + y;
        put(r, B, i, S ? clampv(e, smin(B), smax(B), sat) : clampv(e, 0, umax(B), sat));
    }
    return sat ? QC : 0;
}
template<int B, bool NEG> static u32 absneg(const u32* a, const u32*, u32* r) {
    bool sat = false;
    for (int i = 0; i < 128 / B; i++) {
        i128 x = lane_s(a, B, i);
        i128 e = NEG ? -x : (x < 0 ? -x : x);
        put(r, B, i, clampv(e, smin(B), smax(B), sat));
    }
    return sat ? QC : 0;
}
template<int B, bool RND> static u32 dmulh(const u32* a, const u32* b, u32* r) {
    bool sat = false;
    for (int i = 0; i < 128 / B; i++) {
        i128 e = 2 * lane_s(a, B, i) * lane_s(b, B, i) + (RND ? ((i128)1 << (B - 1)) : 0);
        e >>= B;
        put(r, B, i, clampv(e, smin(B), smax(B), sat));
    }
    return sat ? QC : 0;
}
template<int B> static u32 dmull(const u32* a, const u32* b, u32* r) {
    bool sat = false;
    for (int i = 0; i < 64 / B; i++) {
        i128 e = 2 * lane_s(a, B, i) * lane_s(b, B, 1);  // by-scalar form: d2[1]
        put(r, 2 * B, i, clampv(e, smin(2 * B), smax(2 * B), sat));
    }
    return sat ? QC : 0;
}
// shift by signed amount (low byte of the b lane) or fixed immediate
template<int B, bool S> static u32 qshl_core(const u32* a, int i, int sh, u32* r, bool& sat) {
    i128 x = S ? lane_s(a, B, i) : lane_u(a, B, i);
    i128 e;
    if (sh >= 0) {
        if (x == 0) e = 0;
        else if (sh >= B) e = x < 0 ? smin(120) : smax(120);  // any nonzero value overflows B bits
        else e = x * ((i128)1 << sh);
    } else {
        int rs = -sh; if (rs > 120) rs = 120;
        e = x >> rs;  // arithmetic for negative
    }
    put(r, B, i, S ? clampv(e, smin(B), smax(B), sat) : clampv(e, 0, umax(B), sat));
    return 0;
}
template<int B, bool S> static u32 qshl_reg(const u32* a, const u32* b, u32* r) {
    bool sat = false;
    for (int i = 0; i < 128 / B; i++) { int sh = (int8_t)(lane_u(b, B, i) & 0xff); qshl_core<B, S>(a, i, sh, r, sat); }
    return sat ? QC : 0;
}
template<int B, bool S> static u32 qshl_imm(const u32* a, const u32*, u32* r) {
    constexpr int imm = B == 8 ? 3 : B == 16 ? 5 : B == 32 ? 9 : 17;
    bool sat = false;
    for (int i = 0; i < 128 / B; i++) qshl_core<B, S>(a, i, imm, r, sat);
    return sat ? QC : 0;
}
template<int B> static u32 qshlu_imm(const u32* a, const u32*, u32* r) {
    constexpr int imm = B == 8 ? 3 : B == 16 ? 5 : B == 32 ? 9 : 17;
    bool sat = false;
    for (int i = 0; i < 128 / B; i++) {
        i128 e = lane_s(a, B, i) * ((i128)1 << imm);
        put(r, B, i, clampv(e, 0, umax(B), sat));
    }
    return sat ? QC : 0;
}
// narrow: source lanes of B bits -> B/2 bits in the low half; kind 0 s->s, 1 u->u, 2 s->u
template<int B, int K> static u32 narrow(const u32* a, const u32*, u32* r) {
    bool sat = false;
    r[0] = r[1] = r[2] = r[3] = 0;
    for (int i = 0; i < 128 / B; i++) {
        i128 x = K == 1 ? lane_u(a, B, i) : lane_s(a, B, i);
        int nb = B / 2;
        i128 v = K == 0 ? clampv(x, smin(nb), smax(nb), sat) : clampv(x, 0, umax(nb), sat);
        put(r, nb, i, v);
    }
    return sat ? QC : 0;
}

#define op_qadd_s8 addsub<8, true, false>
#define op_qadd_s16 addsub<16, true, false>
#define op_qadd_s32 addsub<32, true, false>
#define op_qadd_s64 addsub<64, true, false>
#define op_qadd_u8 addsub<8, false, false>
#define op_qadd_u16 addsub<16, false, false>
#define op_qadd_u32 addsub<32, false, false>
#define op_qadd_u64 addsub<64, false, false>
#define op_qsub_s8 addsub<8, true, true>
#define op_qsub_s16 addsub<16, true, true>
#define op_qsub_s32 addsub<32, true, true>
#define op_qsub_s64 addsub<64, true, true>
#define op_qsub_u8 addsub<8, false, true>
#define op_qsub_u16 addsub<16, false, true>
#define op_qsub_u32 addsub<32, false, true>
#define op_qsub_u64 addsub<64, false, true>
#define op_qabs_s8 absneg<8, false>
#define op_qabs_s16 absneg<16, false>
#define op_qabs_s32 absneg<32, false>
#define op_qneg_s8 absneg<8, true>
#define op_qneg_s16 absneg<16, true>
#define op_qneg_s32 absneg<32, true>
#define op_qdmulh_s16 dmulh<16, false>
#define op_qdmulh_s32 dmulh<32, false>
#define op_qrdmulh_s16 dmulh<16, true>
#define op_qrdmulh_s32 dmulh<32, true>
#define op_qdmull_s16 dmull<16>
#define op_qdmull_s32 dmull<32>
#define op_qshl_s8 qshl_reg<8, true>
#define op_qshl_s16 qshl_reg<16, true>
#define op_qshl_s32 qshl_reg<32, true>
#define op_qshl_s64 qshl_reg<64, true>
#define op_qshl_u8 qshl_reg<8, false>
#define op_qshl_u16 qshl_reg<16, false>
#define op_qshl_u32 qshl_reg<32, false>
#define op_qshl_u64 qshl_reg<64, false>
#define op_qshli_s8 qshl_imm<8, true>
#define op_qshli_s16 qshl_imm<16, true>
#define op_qshli_s32 qshl_imm<32, true>
#define op_qshli_s64 qshl_imm<64, true>
#define op_qshli_u8 qshl_imm<8, false>
#define op_qshli_u16 qshl_imm<16, false>
#define op_qshli_u32 qshl_imm<32, false>
#define op_qshli_u64 qshl_imm<64, false>
#define op_qshlu_s8 qshlu_imm<8>
#define op_qshlu_s16 qshlu_imm<16>
#define op_qshlu_s32 qshlu_imm<32>
#define op_qshlu_s64 qshlu_imm<64>
#define op_qmovn_s16 narrow<16, 0>
#define op_qmovn_s32 narrow<32, 0>
#define op_qmovn_s64 narrow<64, 0>
#define op_qmovn_u16 narrow<16, 1>
#define op_qmovn_u32 narrow<32, 1>
#define op_qmovn_u64 narrow<64, 1>
#define op_qmovun_s16 narrow<16, 2>
#define op_qmovun_s32 narrow<32, 2>
#define op_qmovun_s64 narrow<64, 2>

#include "qc_body.inc"
int main() {
    g_log = fopen("qc_ref.log", "w");
    test_body();
    fclose(g_log);
    printf("acc=%08x\n", g_acc);
}
