// Shared test body for neon4.c (guest) and neon4_ref.cpp (host reference).
// Requires: mix(u32), FPSCR_MIX(), typedefs, and all op_* functions defined.

// Float bit-pattern vectors. Indexes 4 and 5 carry NaNs (quiet + signaling);
// index 6 carries infinities/zeros; the NaN-free subset is {0,1,2,3,6,7,8,9}.
static const u32 FP_VECS[10][4] = {
    {0x3F000000u /*0.5*/, 0x3FC00000u /*1.5*/, 0x40200000u /*2.5*/, 0x3F800000u /*1.0*/},
    {0xBF000000u /*-0.5*/, 0xBFC00000u /*-1.5*/, 0xC0200000u /*-2.5*/, 0xBF800000u /*-1.0*/},
    {0x42C80000u /*100*/, 0x3DCCCCCDu /*0.1*/, 0xC2480000u /*-50*/, 0x447A0000u /*1000*/},
    {0x00000001u /*min subnorm*/, 0x80000000u /*-0.0*/, 0x007FFFFFu /*max subnorm*/, 0x33800000u},
    {0x7FC00000u /*qnan*/, 0x7FC12345u /*qnan+payload*/, 0xFFC00000u /*-qnan*/, 0x7F800001u /*snan*/},
    {0x7FE00000u /*qnan*/, 0xFFC54321u /*-qnan+payload*/, 0x7FC00001u /*qnan*/, 0xFF800001u /*-snan*/},
    {0x7F800000u /*+inf*/, 0xFF800000u /*-inf*/, 0x00000000u /*+0*/, 0x80000000u /*-0*/},
    {0x4F000000u /*2^31*/, 0xCF000000u /*-2^31*/, 0x4EFFFFFFu, 0xCEFFFFFFu},
    {0x7E967699u /*~1e38*/, 0x00800000u /*min norm*/, 0xBF7FFFFFu /*~-1.0*/, 0x3EAAAAABu /*1/3*/},
    {0x3EFFFFFFu /*0.49999997*/, 0x40400000u /*3.0*/, 0xC0600000u /*-3.5*/, 0x3F000001u /*0.5+ulp*/},
};
#define NFPV 10

static const int NONAN_IDX[8] = {0, 1, 2, 3, 6, 7, 8, 9};

// Integer patterns for the int->float conversions.
static const u32 INT_VECS[4][4] = {
    {0u, 1u, 0xFFFFFFFFu, 0x80000000u},
    {5u, 0x7FFFFFFFu, 0xFFFFFFFBu, 12345u},
    {0x0000007Fu, 0xFFFFFF81u, 1000000u, 0xFFF0BDC0u},
    {0x12345678u, 0xEDCBA988u, 0x00010000u, 0xFFFF0000u},
};
#define NIV 4

static void run2f(void (*op)(const u32*, const u32*, u32*), int words) {
    u32 r[8];
    for (int i = 0; i < NFPV; i++) {
        for (int w = 0; w < 8; w++)
            r[w] = 0xDEADC0DEu;
        op(FP_VECS[i], FP_VECS[(i + 3) % NFPV], r);
        for (int w = 0; w < words; w++)
            mix(r[w]);
    }
}

// NaN-free operand pairs (V8 canonicalizes NaN in f32x4.min/max).
static void run2f_nn(void (*op)(const u32*, const u32*, u32*), int words) {
    u32 r[8];
    for (int i = 0; i < 8; i++) {
        for (int w = 0; w < 8; w++)
            r[w] = 0xDEADC0DEu;
        op(FP_VECS[NONAN_IDX[i]], FP_VECS[NONAN_IDX[(i + 3) % 8]], r);
        for (int w = 0; w < words; w++)
            mix(r[w]);
    }
}

static void run1f(void (*op)(const u32*, const u32*, u32*), int words) {
    u32 r[8];
    for (int i = 0; i < NFPV; i++) {
        for (int w = 0; w < 8; w++)
            r[w] = 0xDEADC0DEu;
        op(FP_VECS[i], FP_VECS[0], r);
        for (int w = 0; w < words; w++)
            mix(r[w]);
    }
}

static void run1i(void (*op)(const u32*, const u32*, u32*)) {
    u32 r[8];
    for (int i = 0; i < NIV; i++) {
        for (int w = 0; w < 8; w++)
            r[w] = 0xDEADC0DEu;
        op(INT_VECS[i], INT_VECS[0], r);
        for (int w = 0; w < 4; w++)
            mix(r[w]);
    }
}

static void test_body(void) {
    run2f(op_vadd_f32, 4);
    run2f(op_vsub_f32, 4);
    run2f(op_vmul_f32, 4);
    run2f(op_vabd_f32, 4);

    run1f(op_vabs_f32, 4);
    run1f(op_vneg_f32, 4);

    run2f(op_vceq_f32, 4);
    run2f(op_vcgt_f32, 4);
    run2f(op_vcge_f32, 4);
    run2f(op_vcage_f32, 4);
    run2f(op_vcagt_f32, 4);

    run2f_nn(op_vmin_f32, 4);
    run2f_nn(op_vmax_f32, 4);

    run2f(op_vpadd_f32, 2);

    run1f(op_vcvt_s32_f32_3, 4);
    run1f(op_vcvt_u32_f32_7, 4);
    run1f(op_vcvt_s32_f32, 4);
    run1f(op_vcvt_u32_f32, 4);

    run1i(op_vcvt_f32_s32_5);
    run1i(op_vcvt_f32_u32_11);
    run1i(op_vcvt_f32_s32);
    run1i(op_vcvt_f32_u32);

    run1f(op_vrecpe_f32, 4);
    run1f(op_vrsqrte_f32, 4);
    run2f(op_vrecps_f32, 4);
    run2f(op_vrsqrts_f32, 4);

    run1f(op_vcvt_f32_f16, 4);
    run1f(op_vcvt_f16_f32, 2);

    FPSCR_MIX();
}
