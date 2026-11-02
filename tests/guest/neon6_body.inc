// Shared body for neon6.c (guest) and neon6_ref.cpp (host reference).
// 64-bit lanes as {lo, hi} word pairs, two lanes per vector.
static const u32 V64[][4] = {
    {0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u},  // 0, 1
    {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu},  // -1, -2
    {0x7FFFFFFFu, 0x00000000u, 0x80000000u, 0x00000000u},  // INT32_MAX, 2^31
    {0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x00000001u},  // UINT32_MAX, 2^32
    {0x80000000u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu},  // INT32_MIN, INT32_MIN-1
    {0x80000001u, 0xFFFFFFFFu, 0x00000001u, 0x80000000u},  // INT32_MIN+1, huge negative
    {0xFFFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x80000000u},  // INT64_MAX, INT64_MIN
    {0x00000000u, 0xFFFFFFFFu, 0x12345678u, 0x9ABCDEF0u},
};
#define NV64 8

static void run_narrow(void (*op)(const u32*, u32*)) {
    u32 r[4];
    for (int i = 0; i < NV64; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(V64[i], r);
        mix(r[0]);
        mix(r[1]);
    }
}

static void test_body(void) {
    run_narrow(op_vqmovn_s64);
    run_narrow(op_vqmovn_u64);
    run_narrow(op_vqmovun_s64);
}
