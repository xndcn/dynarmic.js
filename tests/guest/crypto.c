// ARMv8 crypto extension guest test: AES-128 (FIPS-197 appendix C.1) encrypt
// and decrypt built from AESE/AESMC/AESD/AESIMC, and SHA-256("abc") built from
// SHA256H/H2/SU0/SU1. Both have published answers, so the test is self-checking.
// Writes the pass magic only if every check matches.

#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define OUT ((volatile u32*)0x00080000)
#define MAGIC_ADDR ((volatile u32*)0x00080040)

typedef struct {
    u32 w[4];
} __attribute__((aligned(16))) V;

#define OP2(nm, insn)                                         \
    static V nm(V d, V m) {                                   \
        __asm__ volatile("vldmia %0, {q0}\n vldmia %1, {q1}\n" insn "\n vstmia %0, {q0}\n" \
                         :: "r"(&d), "r"(&m) : "q0", "q1", "memory"); \
        return d;                                             \
    }
#define OP1(nm, insn)                                         \
    static V nm(V d) {                                        \
        __asm__ volatile("vldmia %0, {q0}\n" insn "\n vstmia %0, {q0}\n" \
                         :: "r"(&d) : "q0", "memory");        \
        return d;                                             \
    }

OP2(aese, "aese.8 q0, q1")
OP2(aesd, "aesd.8 q0, q1")
OP1(aesmc, "aesmc.8 q0, q0")
OP1(aesimc, "aesimc.8 q0, q0")
OP2(sha256su0, "sha256su0.32 q0, q1")

static V sha256su1(V d, V n, V m) {
    __asm__ volatile("vldmia %0, {q0}\n vldmia %1, {q1}\n vldmia %2, {q2}\n"
                     "sha256su1.32 q0, q1, q2\n vstmia %0, {q0}\n"
                     :: "r"(&d), "r"(&n), "r"(&m) : "q0", "q1", "q2", "memory");
    return d;
}
// sha256h: abcd = H(abcd, efgh, wk); sha256h2: efgh = H2(efgh, abcd, wk)
static V sha256h(V abcd, V efgh, V wk) {
    __asm__ volatile("vldmia %0, {q0}\n vldmia %1, {q1}\n vldmia %2, {q2}\n"
                     "sha256h.32 q0, q1, q2\n vstmia %0, {q0}\n"
                     :: "r"(&abcd), "r"(&efgh), "r"(&wk) : "q0", "q1", "q2", "memory");
    return abcd;
}
static V sha256h2(V efgh, V abcd, V wk) {
    __asm__ volatile("vldmia %0, {q0}\n vldmia %1, {q1}\n vldmia %2, {q2}\n"
                     "sha256h2.32 q0, q1, q2\n vstmia %0, {q0}\n"
                     :: "r"(&efgh), "r"(&abcd), "r"(&wk) : "q0", "q1", "q2", "memory");
    return efgh;
}

static V veor(V a, V b) {
    for (int i = 0; i < 4; i++)
        a.w[i] ^= b.w[i];
    return a;
}

// ---- software AES-128 key expansion ----
static u8 sbox[256];
static u8 xt(u8 x) { return (u8)((x << 1) ^ ((x >> 7) * 0x1b)); }
static void init_sbox(void) {
    // generate the AES S-box (multiplicative inverse + affine transform)
    u8 p = 1, q = 1;
    do {
        p = p ^ (u8)(p << 1) ^ (u8)((p & 0x80) ? 0x1b : 0);
        q ^= q << 1; q ^= q << 2; q ^= q << 4;
        if (q & 0x80) q ^= 0x09;
        u8 x = q ^ (u8)((q << 1) | (q >> 7)) ^ (u8)((q << 2) | (q >> 6)) ^
               (u8)((q << 3) | (q >> 5)) ^ (u8)((q << 4) | (q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
}
static V rk[11];
static void expand_key(const u8* key) {
    u8 w[176];
    for (int i = 0; i < 16; i++) w[i] = key[i];
    u8 rc = 1;
    for (int i = 16; i < 176; i += 4) {
        u8 t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if ((i & 15) == 0) {
            u8 t0 = t[0];
            t[0] = sbox[t[1]] ^ rc; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[t0];
            rc = xt(rc);
        }
        for (int k = 0; k < 4; k++) w[i + k] = w[i - 16 + k] ^ t[k];
    }
    for (int r = 0; r < 11; r++)
        for (int i = 0; i < 4; i++)
            rk[r].w[i] = (u32)w[r * 16 + i * 4] | ((u32)w[r * 16 + i * 4 + 1] << 8) |
                         ((u32)w[r * 16 + i * 4 + 2] << 16) | ((u32)w[r * 16 + i * 4 + 3] << 24);
}

static V load_bytes(const u8* b) {
    V v;
    for (int i = 0; i < 4; i++)
        v.w[i] = (u32)b[i * 4] | ((u32)b[i * 4 + 1] << 8) | ((u32)b[i * 4 + 2] << 16) | ((u32)b[i * 4 + 3] << 24);
    return v;
}

static int test_aes(void) {
    u8 key[16], pt[16];
    static const u8 expect_ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                     0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    for (int i = 0; i < 16; i++) {
        key[i] = (u8)i;
        pt[i] = (u8)(i * 0x11);
    }
    init_sbox();
    expand_key(key);

    V s = load_bytes(pt);
    for (int r = 0; r < 9; r++)
        s = aesmc(aese(s, rk[r]));
    s = aese(s, rk[9]);
    s = veor(s, rk[10]);
    V ct = load_bytes(expect_ct);
    for (int i = 0; i < 4; i++)
        if (s.w[i] != ct.w[i]) return 1;

    // decrypt back
    V z = {{0, 0, 0, 0}};
    V d = aesd(ct, rk[10]);
    for (int r = 9; r >= 1; r--)
        d = aesd(aesimc(veor(d, rk[r])), z);
    d = veor(d, rk[0]);
    V p = load_bytes(pt);
    for (int i = 0; i < 4; i++)
        if (d.w[i] != p.w[i]) return 2;
    return 0;
}

static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static int test_sha256(void) {
    // padded single block for "abc"
    u32 blk[16] = {0x61626380u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x18};
    V st0 = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a}};
    V st1 = {{0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}};
    V save0 = st0, save1 = st1;
    V w[4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) w[i].w[j] = blk[i * 4 + j];

    for (int i = 0; i < 16; i++) {
        V wk, kk = {{K[4 * i], K[4 * i + 1], K[4 * i + 2], K[4 * i + 3]}};
        V cur = w[i & 3];
        for (int j = 0; j < 4; j++) wk.w[j] = cur.w[j] + kk.w[j];
        V tmp = st0;
        st0 = sha256h(st0, st1, wk);
        st1 = sha256h2(st1, tmp, wk);
        if (i < 12) {
            // w[i+4] = su1(su0(w[i], w[i+1]), w[i+2], w[i+3])
            V next = sha256su1(sha256su0(w[i & 3], w[(i + 1) & 3]), w[(i + 2) & 3], w[(i + 3) & 3]);
            w[i & 3] = next;
        }
    }
    for (int j = 0; j < 4; j++) {
        st0.w[j] += save0.w[j];
        st1.w[j] += save1.w[j];
    }
    static const u32 expect[8] = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
                                  0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
    for (int j = 0; j < 4; j++) {
        if (st0.w[j] != expect[j]) return 1;
        if (st1.w[j] != expect[4 + j]) return 2;
    }
    return 0;
}

int guest_main(void) {
    const int a = test_aes();
    const int s = test_sha256();
    OUT[0] = (u32)a;
    OUT[1] = (u32)s;
    if (a == 0 && s == 0)
        *MAGIC_ADDR = 0x600DC0DEu;
    return a | (s << 4);
}
