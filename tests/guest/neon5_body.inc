// Shared test body for neon5.c (guest) and neon5_ref.cpp (host reference).
// Requires: mix(u32), typedefs, and all op_* functions defined.
// Same FP_VECS bit patterns as neon4 (incl. quiet/signaling NaNs in 4/5).

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

// op(n, m, addend, r) for vfma/vfms.
static void run3f(void (*op)(const u32*, const u32*, const u32*, u32*)) {
    u32 r[8];
    for (int i = 0; i < NFPV; i++) {
        for (int w = 0; w < 8; w++)
            r[w] = 0xDEADC0DEu;
        op(FP_VECS[i], FP_VECS[(i + 3) % NFPV], FP_VECS[(i + 7) % NFPV], r);
        for (int w = 0; w < 4; w++)
            mix(r[w]);
    }
}

// Scalar (S-register) ops over the flattened vectors.
static void run2s(void (*op)(const u32*, const u32*, u32*)) {
    u32 r;
    for (int i = 0; i < NFPV * 4; i++) {
        const u32* flat = &FP_VECS[0][0];
        op(&flat[i], &flat[(i + 13) % (NFPV * 4)], &r);
        mix(r);
    }
}

static void test_body(void) {
    run3f(op_vfma_f32);
    run3f(op_vfms_f32);

    run1f(op_vrintn_f32, 4);
    run1f(op_vrinta_f32, 4);
    run1f(op_vrintp_f32, 4);
    run1f(op_vrintm_f32, 4);
    run1f(op_vrintz_f32, 4);

    run2f(op_vminnm_f32, 4);
    run2f(op_vmaxnm_f32, 4);

    run1f(op_vcvtn_s32_f32, 4);
    run1f(op_vcvtn_u32_f32, 4);
    run1f(op_vcvta_s32_f32, 4);
    run1f(op_vcvta_u32_f32, 4);
    run1f(op_vcvtp_s32_f32, 4);
    run1f(op_vcvtp_u32_f32, 4);
    run1f(op_vcvtm_s32_f32, 4);
    run1f(op_vcvtm_u32_f32, 4);

    run2s(op_vminnm_s);
    run2s(op_vmaxnm_s);
}
