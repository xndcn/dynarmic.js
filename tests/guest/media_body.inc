// Shared test body for the media/saturation test: included by media.c
// (ARM guest, real instructions) and media_ref.c (host, C semantics).
// Both mix an identical sequence of results into g_acc (FNV-1a).
//
// Required definitions from the includer:
//   mix(u32), read_q(), read_ge(),
//   op_* functions for every tested instruction.

// clang-format off
static const u32 AV[] = {
    0x00000000u, 0xFFFFFFFFu, 0x7F7F7F7Fu, 0x80808080u,
    0x01020304u, 0xFEFF8081u, 0x12345678u, 0x9ABCDEF0u,
    0x7FFFFFFFu, 0x80000000u, 0x00FF00FFu, 0xFF00FF00u,
    0x7FFF7FFFu, 0x80008000u, 0x00010001u, 0x5555AAAAu,
};
static const u32 BV[] = {
    0x01010101u, 0x7FFFFFFFu, 0x0101FF01u, 0x7F808001u,
    0xFF00FF00u, 0x02017F80u, 0x0F0F0F0Fu, 0x11111111u,
    0x00000001u, 0x80000000u, 0x00FF00FFu, 0x0F0FF0F0u,
    0x00010001u, 0xFFFF0001u, 0x80008000u, 0xAAAA5555u,
};
#define NV (sizeof(AV) / sizeof(AV[0]))

static void test_body(void) {
    for (u32 i = 0; i < NV; i++) {
        const u32 a = AV[i], b = BV[i];

        // --- sticky-Q scalar ops (tracked identically on both sides) ---
        mix(op_qadd(a, b));  mix(read_q());
        mix(op_qsub(a, b));  mix(read_q());
        mix(op_qdadd(a, b)); mix(read_q());
        mix(op_qdsub(a, b)); mix(read_q());

        mix(op_ssat(a, 1));  mix(read_q());
        mix(op_ssat(a, 8));  mix(read_q());
        mix(op_ssat(a, 16)); mix(read_q());
        mix(op_ssat(a, 32)); mix(read_q());
        mix(op_ssat_lsl(a, 12, 5)); mix(read_q());
        mix(op_ssat_asr(a, 16, 7)); mix(read_q());
        mix(op_usat(a, 1));  mix(read_q());
        mix(op_usat(a, 8));  mix(read_q());
        mix(op_usat(a, 16)); mix(read_q());
        mix(op_usat(a, 31)); mix(read_q());
        mix(op_usat_asr(a, 16, 7)); mix(read_q());

        mix(op_ssat16(a, 1));  mix(read_q());
        mix(op_ssat16(a, 9));  mix(read_q());
        mix(op_ssat16(a, 16)); mix(read_q());
        mix(op_usat16(a, 1));  mix(read_q());
        mix(op_usat16(a, 9));  mix(read_q());
        mix(op_usat16(a, 15)); mix(read_q());

        // --- parallel ops with GE ---
        mix(op_sadd8(a, b));  mix(read_ge());
        mix(op_sadd16(a, b)); mix(read_ge());
        mix(op_ssub8(a, b));  mix(read_ge());
        mix(op_ssub16(a, b)); mix(read_ge());
        mix(op_uadd8(a, b));  mix(read_ge());
        mix(op_uadd16(a, b)); mix(read_ge());
        mix(op_usub8(a, b));  mix(read_ge());
        mix(op_usub16(a, b)); mix(read_ge());
        mix(op_sasx(a, b));   mix(read_ge());
        mix(op_ssax(a, b));   mix(read_ge());
        mix(op_uasx(a, b));   mix(read_ge());
        mix(op_usax(a, b));   mix(read_ge());

        // --- halving (no flags) ---
        mix(op_shadd8(a, b));
        mix(op_shadd16(a, b));
        mix(op_shsub8(a, b));
        mix(op_shsub16(a, b));
        mix(op_uhadd8(a, b));
        mix(op_uhadd16(a, b));
        mix(op_uhsub8(a, b));
        mix(op_uhsub16(a, b));
        mix(op_shasx(a, b));
        mix(op_shsax(a, b));
        mix(op_uhasx(a, b));
        mix(op_uhsax(a, b));

        // --- parallel saturating (dynarmic does not set Q here) ---
        mix(op_qadd8(a, b));
        mix(op_qadd16(a, b));
        mix(op_qsub8(a, b));
        mix(op_qsub16(a, b));
        mix(op_uqadd8(a, b));
        mix(op_uqadd16(a, b));
        mix(op_uqsub8(a, b));
        mix(op_uqsub16(a, b));

        // --- abs-diff accumulate ---
        mix(op_usad8(a, b));
        mix(op_usada8(a, b, AV[(i + 3) % NV]));

        // --- sel: depends on GE left by this sadd8 ---
        (void)op_sadd8(a, b);
        mix(read_ge());
        mix(op_sel(AV[(i + 1) % NV], BV[(i + 2) % NV]));
        // again with GE from an unsigned sub
        (void)op_usub16(a, b);
        mix(read_ge());
        mix(op_sel(b, a));
    }
}
// clang-format on
