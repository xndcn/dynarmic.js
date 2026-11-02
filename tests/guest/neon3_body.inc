// Shared test body for neon3.c (guest) and neon3_ref.c (host reference).
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

// Shift-amount vectors: byte lanes cycle through interesting signed amounts
// (edge cases: 0, +-1, W-1, W, W+1, 127, -128, -(W), -(W+1) for all W).
static const u32 AMT_VECS[8][4] = {
    {0x03020100u, 0x07050301u, 0x00010203u, 0x04050607u},
    {0x09080704u, 0x100F0E0Du, 0x201F1811u, 0x3F212040u},
    {0x807F4107u, 0xFFFEFDFCu, 0xF9F8F7FBu, 0xF0F1F2F3u},
    {0xC1C0BFC2u, 0x9C8182A0u, 0xE0DFE1D0u, 0x05FB04FCu},
    {0x0102FFFEu, 0x0807F8F9u, 0x1011F0EFu, 0x2021E0DFu},
    {0x403FC0BFu, 0x7F808100u, 0x06050403u, 0xFAF9FCFBu},
    {0x0C0D0E0Fu, 0xF4F3F2F1u, 0x30313233u, 0xD0D1D2D3u},
    {0x00000001u, 0xFFFFFFFFu, 0x7F7F8080u, 0x0101FEFEu},
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

// same, but the second operand comes from AMT_VECS (for shift-by-register)
static void run2amt(void (*op)(const u32*, const u32*, u32*), int words) {
    u32 r[8];
    for (int i = 0; i < NVEC; i++) {
        for (int w = 0; w < 8; w++)
            r[w] = 0xDEADC0DEu;
        op(IN_VECS[i], AMT_VECS[(i + 3) % NVEC], r);
        for (int w = 0; w < words; w++)
            mix(r[w]);
    }
}

static void test_body(void) {
    run2(op_vqadd_s32, 4);
    run2(op_vqadd_u32, 4);
    run2(op_vqsub_s32, 4);
    run2(op_vqsub_u32, 4);

    run2(op_vqabs_s8, 4);
    run2(op_vqabs_s16, 4);
    run2(op_vqabs_s32, 4);
    run2(op_vqneg_s8, 4);
    run2(op_vqneg_s16, 4);
    run2(op_vqneg_s32, 4);

    run2amt(op_vqshl_s8, 4);
    run2amt(op_vqshl_s16, 4);
    run2amt(op_vqshl_s32, 4);
    run2amt(op_vqshl_s64, 4);
    run2amt(op_vqshl_u8, 4);
    run2amt(op_vqshl_u16, 4);
    run2amt(op_vqshl_u32, 4);
    run2amt(op_vqshl_u64, 4);

    run2(op_vqshli_s8_3, 4);
    run2(op_vqshli_s16_9, 4);
    run2(op_vqshli_s32_20, 4);
    run2(op_vqshli_s64_40, 4);
    run2(op_vqshli_u8_5, 4);
    run2(op_vqshli_u16_13, 4);
    run2(op_vqshli_u32_31, 4);
    run2(op_vqshli_u64_63, 4);

    run2(op_vqshlu_s8_5, 4);
    run2(op_vqshlu_s16_13, 4);
    run2(op_vqshlu_s32_1, 4);
    run2(op_vqshlu_s64_63, 4);

    run2amt(op_vrshl_s8, 4);
    run2amt(op_vrshl_s16, 4);
    run2amt(op_vrshl_s32, 4);
    run2amt(op_vrshl_s64, 4);
    run2amt(op_vrshl_u8, 4);
    run2amt(op_vrshl_u16, 4);
    run2amt(op_vrshl_u32, 4);
    run2amt(op_vrshl_u64, 4);

    run2(op_vqdmulh_s16, 4);
    run2(op_vqdmulh_s32, 4);
    run2(op_vqrdmulh_s16, 4);
    run2(op_vqrdmulh_s32, 4);

    run2(op_vmul_p8, 4);
    run2(op_vmull_p8, 4);

    run2(op_vrecpe_u32, 4);
    run2(op_vrsqrte_u32, 4);
}
