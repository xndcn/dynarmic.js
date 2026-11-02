// Shared test driver: runs every op over a grid of boundary-heavy vectors and
// mixes result words + the QC bit into the checksum/log.
static const u32 kW[20] = {
    0x00000000, 0x00000001, 0x0000007f, 0x00000080, 0x000000ff, 0x00007fff, 0x00008000, 0x0000ffff,
    0x7fffffff, 0x80000000, 0xffffffff, 0x40000000, 0x01020304, 0x80808080, 0x7f7f7f7f, 0xfffefffe,
    0x00000003, 0x0000fff8, 0x00000020, 0x00000040};
#define NV 28
static void make_vec(u32 k, u32* v) {
    for (u32 l = 0; l < 4; l++) v[l] = kW[(k * 3 + l * 7 + (k >> 2) * l) % 20];
}
#define Q2(nm) \
    for (u32 i = 0; i < NV; i++) for (u32 j = 0; j < NV; j++) { \
        u32 a[4], b[4], r[4] = {0, 0, 0, 0}; make_vec(i, a); make_vec(j, b); \
        u32 f = op_##nm(a, b, r); \
        mix(r[0]); mix(r[1]); mix(r[2]); mix(r[3]); mix((f >> 27) & 1); }
#define Q1(nm) \
    for (u32 i = 0; i < NV * 4; i++) { \
        u32 a[4], b[4] = {0, 0, 0, 0}, r[4] = {0, 0, 0, 0}; make_vec(i % NV, a); a[i % 4] ^= (i / NV) * 0x01010101u; \
        u32 f = op_##nm(a, b, r); \
        mix(r[0]); mix(r[1]); mix(r[2]); mix(r[3]); mix((f >> 27) & 1); }
static void test_body(void) {
#ifndef NOBODY
#include "qc_ops.def"
#endif
}
