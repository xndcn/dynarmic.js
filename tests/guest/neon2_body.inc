// Shared test body for neon2.c (guest) and neon2_ref.c (host reference).
// Requires: mix(u32), typedefs, and all op_* functions defined.

static const u32 IN_VECS[8][4] = {
    {0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu},
    {0xFF7F80FEu, 0x01A55A7Eu, 0x8081807Fu, 0xFFFFFFFFu},
    {0x7FFFFFFFu, 0x80000000u, 0x00000000u, 0xFFFFFFFFu},
    {0x01010101u, 0x02020202u, 0x7F7F7F7Fu, 0x80808080u},
    {0xDEADBEEFu, 0x12345678u, 0xCAFEBABEu, 0x0BADF00Du},
    {0x55555555u, 0xAAAAAAAAu, 0x33333333u, 0xCCCCCCCCu},
    {0x0000007Fu, 0x0000FF80u, 0x007FFFFFu, 0x00800000u},
    {0xF0F0F0F0u, 0x0F0F0F0Fu, 0x12348765u, 0x9ABCDEF0u},
};
#define NVEC 8

// Flat 32-byte table for vtbl/vtbx (4 D registers); must be identical in
// guest and reference, so it lives here.
static const u32 TABLE32[8] = {
    0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu,
    0xFF7F80FEu, 0x01A55A7Eu, 0x8081807Fu, 0xFFFFFFFFu,
};

static void run2(void (*op)(const u32*, const u32*, u32*), int words) {
    u32 r[8];
    for (int i = 0; i < NVEC; i++) {
        for (int w = 0; w < 8; w++)
            r[w] = 0xDEADC0DEu;
        op(IN_VECS[i], IN_VECS[(i + 3) % NVEC], r);
        for (int w = 0; w < words; w++)
            mix(r[w]);
    }
}

static void run_tbl(void (*op)(const u32*, const u32*, u32*)) {
    u32 r[4];
    for (int i = 0; i < NVEC; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(TABLE32, IN_VECS[(i + 5) % NVEC], r);
        mix(r[0]);
        mix(r[1]);
    }
}

static void run_tbx(void (*op)(const u32*, const u32*, const u32*, u32*)) {
    u32 r[4];
    for (int i = 0; i < NVEC; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(TABLE32, IN_VECS[(i + 5) % NVEC], IN_VECS[(i + 1) % NVEC], r);
        mix(r[0]);
        mix(r[1]);
    }
}

static void test_body(void) {
    run2(op_vuzp_8_q, 8);
    run2(op_vuzp_16_q, 8);
    run2(op_vuzp_32_q, 8);
    run2(op_vuzp_8_d, 4);
    run2(op_vuzp_16_d, 4);
    run2(op_vuzp_32_d, 4);
    run2(op_vzip_8_q, 8);
    run2(op_vzip_16_q, 8);
    run2(op_vzip_32_q, 8);
    run2(op_vzip_8_d, 4);
    run2(op_vzip_16_d, 4);
    run2(op_vzip_32_d, 4);
    run2(op_vtrn_8_q, 8);
    run2(op_vtrn_16_q, 8);
    run2(op_vtrn_32_q, 8);
    run2(op_vtrn_8_d, 4);
    run2(op_vtrn_16_d, 4);
    run2(op_vtrn_32_d, 4);
    run2(op_vrev16_8, 4);
    run2(op_vrev32_8, 4);
    run2(op_vrev32_16, 4);
    run2(op_vrev64_8, 4);
    run2(op_vrev64_16, 4);
    run2(op_vrev64_32, 4);
    run2(op_vext_8_q3, 4);
    run2(op_vext_8_q9, 4);
    run2(op_vext_8_d3, 2);
    run2(op_vext_8_d6, 2);
    run2(op_vqadd_s8, 4);
    run2(op_vqadd_s16, 4);
    run2(op_vqadd_u8, 4);
    run2(op_vqadd_u16, 4);
    run2(op_vqsub_s8, 4);
    run2(op_vqsub_s16, 4);
    run2(op_vqsub_u8, 4);
    run2(op_vqsub_u16, 4);
    run2(op_vabd_s8, 4);
    run2(op_vabd_s16, 4);
    run2(op_vabd_s32, 4);
    run2(op_vabd_u8, 4);
    run2(op_vabd_u16, 4);
    run2(op_vabd_u32, 4);
    run2(op_vqmovn_s16, 2);
    run2(op_vqmovn_s32, 2);
    run2(op_vqmovn_u16, 2);
    run2(op_vqmovn_u32, 2);
    run2(op_vqmovun_s16, 2);
    run2(op_vqmovun_s32, 2);
    run2(op_vclz_8, 4);
    run2(op_vclz_16, 4);
    run2(op_vclz_32, 4);
    run2(op_vshl_s8, 4);
    run2(op_vshl_s16, 4);
    run2(op_vshl_s32, 4);
    run2(op_vshl_s64, 4);
    run2(op_vshl_u8, 4);
    run2(op_vshl_u16, 4);
    run2(op_vshl_u32, 4);
    run2(op_vshl_u64, 4);
    run_tbl(op_vtbl1);
    run_tbl(op_vtbl2);
    run_tbl(op_vtbl4);
    run_tbx(op_vtbx2);
    run_tbx(op_vtbx3);
}
