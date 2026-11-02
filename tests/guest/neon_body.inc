// Shared test body for neon.c (guest) and neon_ref.c (host reference).
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

static const u32 IN_SCALARS[7] = {
    0x00000000u, 0x00000001u, 0x0000007Fu, 0x00000080u,
    0x0000ABCDu, 0xDEADBEEFu, 0xFFFFFFFFu,
};
#define NSC 7

static void run2(void (*op)(const u32*, const u32*, u32*), int words) {
    u32 r[4];
    for (int i = 0; i < NVEC; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(IN_VECS[i], IN_VECS[(i + 3) % NVEC], r);
        for (int w = 0; w < words; w++)
            mix(r[w]);
    }
}

static void run_dup(void (*op)(u32, u32*)) {
    u32 r[4];
    for (int i = 0; i < NSC; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(IN_SCALARS[i], r);
        for (int w = 0; w < 4; w++)
            mix(r[w]);
    }
}

static void run_dupe(void (*op)(const u32*, u32*)) {
    u32 r[4];
    for (int i = 0; i < NVEC; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(IN_VECS[i], r);
        for (int w = 0; w < 4; w++)
            mix(r[w]);
    }
}

static void run_get(u32 (*op)(const u32*)) {
    for (int i = 0; i < NVEC; i++)
        mix(op(IN_VECS[i]));
}

static void run_set(void (*op)(const u32*, u32, u32*)) {
    u32 r[4];
    for (int i = 0; i < NVEC; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(IN_VECS[i], IN_SCALARS[(i * 5 + 1) % NSC], r);
        for (int w = 0; w < 4; w++)
            mix(r[w]);
    }
}

static void run_get64(void (*op)(const u32*, u32*)) {
    u32 r[2];
    for (int i = 0; i < NVEC; i++) {
        op(IN_VECS[i], r);
        mix(r[0]);
        mix(r[1]);
    }
}

static void run_set64(void (*op)(const u32*, u32, u32, u32*)) {
    u32 r[4];
    for (int i = 0; i < NVEC; i++) {
        r[0] = r[1] = r[2] = r[3] = 0xDEADC0DEu;
        op(IN_VECS[i], IN_SCALARS[i % NSC], IN_SCALARS[(i + 2) % NSC], r);
        for (int w = 0; w < 4; w++)
            mix(r[w]);
    }
}

static void test_body(void) {
    run2(op_vadd_i8, 4);
    run2(op_vadd_i16, 4);
    run2(op_vadd_i32, 4);
    run2(op_vadd_i64, 4);
    run2(op_vsub_i8, 4);
    run2(op_vsub_i16, 4);
    run2(op_vsub_i32, 4);
    run2(op_vsub_i64, 4);
    run2(op_vand, 4);
    run2(op_vbic, 4);
    run2(op_vorr, 4);
    run2(op_veor, 4);
    run2(op_vmul_i8, 4);
    run2(op_vmul_i16, 4);
    run2(op_vmul_i32, 4);
    run2(op_vmin_s8, 4);
    run2(op_vmin_s16, 4);
    run2(op_vmin_s32, 4);
    run2(op_vmin_u8, 4);
    run2(op_vmin_u16, 4);
    run2(op_vmin_u32, 4);
    run2(op_vmax_s8, 4);
    run2(op_vmax_s16, 4);
    run2(op_vmax_s32, 4);
    run2(op_vmax_u8, 4);
    run2(op_vmax_u16, 4);
    run2(op_vmax_u32, 4);
    run2(op_vceq_i8, 4);
    run2(op_vceq_i16, 4);
    run2(op_vceq_i32, 4);
    run2(op_vcgt_s8, 4);
    run2(op_vcgt_s16, 4);
    run2(op_vcgt_s32, 4);
    run2(op_vhadd_s8, 4);
    run2(op_vhadd_s16, 4);
    run2(op_vhadd_s32, 4);
    run2(op_vhadd_u8, 4);
    run2(op_vhadd_u16, 4);
    run2(op_vhadd_u32, 4);
    run2(op_vrhadd_s8, 4);
    run2(op_vrhadd_s16, 4);
    run2(op_vrhadd_s32, 4);
    run2(op_vrhadd_u8, 4);
    run2(op_vrhadd_u16, 4);
    run2(op_vrhadd_u32, 4);
    run2(op_vhsub_s8, 4);
    run2(op_vhsub_s16, 4);
    run2(op_vhsub_s32, 4);
    run2(op_vhsub_u8, 4);
    run2(op_vhsub_u16, 4);
    run2(op_vhsub_u32, 4);
    run2(op_vabs_s8, 4);
    run2(op_vabs_s16, 4);
    run2(op_vabs_s32, 4);
    run2(op_vcnt_8, 4);
    run2(op_vmvn, 4);
    run2(op_vpaddl_s8, 4);
    run2(op_vpaddl_s16, 4);
    run2(op_vpaddl_s32, 4);
    run2(op_vpaddl_u8, 4);
    run2(op_vpaddl_u16, 4);
    run2(op_vpaddl_u32, 4);
    run2(op_vmovl_s8, 4);
    run2(op_vmovl_s16, 4);
    run2(op_vmovl_s32, 4);
    run2(op_vmovl_u8, 4);
    run2(op_vmovl_u16, 4);
    run2(op_vmovl_u32, 4);
    run2(op_vmovn_i16, 2);
    run2(op_vmovn_i32, 2);
    run2(op_vmovn_i64, 2);
    run2(op_vmull_s8, 4);
    run2(op_vmull_s16, 4);
    run2(op_vmull_s32, 4);
    run2(op_vmull_u8, 4);
    run2(op_vmull_u16, 4);
    run2(op_vmull_u32, 4);
    run2(op_vpadd_i8, 2);
    run2(op_vpadd_i16, 2);
    run2(op_vpadd_i32, 2);
    run2(op_vshl_i8_3, 4);
    run2(op_vshl_i16_9, 4);
    run2(op_vshl_i32_31, 4);
    run2(op_vshl_i64_1, 4);
    run2(op_vshl_i64_63, 4);
    run2(op_vshr_u8_1, 4);
    run2(op_vshr_u8_7, 4);
    run2(op_vshr_u16_15, 4);
    run2(op_vshr_u32_1, 4);
    run2(op_vshr_u64_40, 4);
    run2(op_vshr_s8_5, 4);
    run2(op_vshr_s16_1, 4);
    run2(op_vshr_s32_31, 4);
    run2(op_vshr_s64_62, 4);
    run_dup(op_vdup_8);
    run_dup(op_vdup_16);
    run_dup(op_vdup_32);
    run_dupe(op_vdup_e16_l2);
    run_dupe(op_vdup_e32_l1);
    run_get(op_get8_d0l3);
    run_get(op_get16_d1l2);
    run_get(op_get32_d0l1);
    run_set(op_set8_d0l5);
    run_set(op_set16_d1l2);
    run_set(op_set32_d0l1);
    run_get64(op_get64_d1);
    run_set64(op_set64_d1);
}
