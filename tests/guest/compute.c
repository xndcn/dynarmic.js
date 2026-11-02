// Shared compute workload, compiled both for the guest (A32 bare-metal) and
// for the host (reference checksum). Pure integer code, freestanding-friendly.
//
// ComputeResult() mixes in a FNV-1a-style checksum of every intermediate
// result so any divergence between the JIT and the host changes the hash.

#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

static u64 g_hash = 1469598103934665603ULL;

static void mix(u64 v) {
    g_hash ^= v;
    g_hash *= 1099511628211ULL;
}

// --- integer multiply/divide chains ---
static void test_muldiv(void) {
    u64 acc = 0;
    for (u32 i = 1; i < 1000; i++) {
        acc += (u64)i * (u64)(i ^ 0x5A5A);
        acc ^= (u64)(s64)(s32)(i * 2654435761u) >> 3;
        mix(acc);
        mix((u64)(u32)(1000000007u / (i | 1)));
        mix((u64)(u32)(0x80000000u % (i * 3 + 7)));
        mix((u64)(u32)((s32)(0x80000000u + i) / (s32)(i + 3)));
        mix((u64)(s64)(0x7FFFFFFFFFFFFFFFLL / (s64)(i + 5)));
        mix(acc >> 13);
    }
}

// --- bit manipulation ---
static void test_bits(void) {
    u32 x = 0x12345678u;
    for (u32 i = 0; i < 500; i++) {
        x = (x << 7) | (x >> 25);
        mix(x);
        x ^= __builtin_clz(x | 1);
        mix(x);
        x += __builtin_popcount(x);
        mix(x);
        x ^= __builtin_bswap32(x) >> 3;
        mix(x);
        u32 r = 0;
        for (int b = 0; b < 32; b++)  // manual bit-reverse
            r |= ((x >> b) & 1u) << (31 - b);
        mix(r);
        x += r >> 5;
        mix(x);
    }
}

// --- 64-bit arithmetic ---
static void test_wide(void) {
    u64 a = 0x0123456789ABCDEFULL;
    s64 b = -0x123456789ABLL;
    for (u32 i = 0; i < 300; i++) {
        a = a * 6364136223846793005ULL + 1442695040888963407ULL;
        mix(a);
        mix(a >> 32);
        b += (s64)(a >> 11);
        mix((u64)b);
        mix((u64)(b >> 17));
    }
}

// --- memory-heavy: sort + checksum over a buffer ---
static u8 buf[4096];

static void test_memory(void) {
    u32 x = 0xDEADBEEF;
    for (u32 i = 0; i < sizeof(buf); i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (u8)(x >> 16);
    }
    // insertion sort on u8 (byte loads/stores, branches)
    for (u32 i = 1; i < sizeof(buf); i++) {
        u8 v = buf[i];
        u32 j = i;
        while (j > 0 && buf[j - 1] > v) {
            buf[j] = buf[j - 1];
            j--;
        }
        buf[j] = v;
    }
    u32 c = 0;
    for (u32 i = 0; i < sizeof(buf); i++) {
        c = (c * 31u) + buf[i];
        if ((i & 15) == 0)
            mix(c);
    }
    mix(c);
}

// --- string-ish ops: byte search/copy like strlen/memcpy ---
static char text[2048];

static void test_string(void) {
    for (u32 i = 0; i < sizeof(text) - 1; i++)
        text[i] = (char)('a' + (i * 7 + 3) % 26);
    text[sizeof(text) - 1] = 0;
    u32 len = 0;
    while (text[len])
        len++;
    mix(len);
    u32 freq[26] = {0};
    for (u32 i = 0; i < len; i++)
        freq[(u8)text[i] - 'a']++;
    for (u32 i = 0; i < 26; i++)
        mix(freq[i]);
}

// --- switch/call-heavy state machine ---
static u32 state_step(u32 s, u32 i) {
    switch (s & 7) {
    case 0: return s + i * 3;
    case 1: return s ^ (i << 5);
    case 2: return (s >> 3) + 0x9E3779B9u;
    case 3: return s * 5 - i;
    case 4: return __builtin_bswap32(s) + i;
    case 5: return (s << 9) | (s >> 23);
    case 6: return s ^ 0xA5A5A5A5u;
    default: return s - 0x13572468u;
    }
}

static void test_statemachine(void) {
    u32 s = 42;
    for (u32 i = 0; i < 20000; i++)
        s = state_step(s, i);
    mix(s);
}

u64 ComputeResultStage(int stage);  // defined below

static u64 run_stage(int stage) {
    g_hash = 1469598103934665603ULL;
    switch (stage) {
    case 0: test_muldiv(); break;
    case 1: test_bits(); break;
    case 2: test_wide(); break;
    case 3: test_memory(); break;
    case 4: test_string(); break;
    case 5: test_statemachine(); break;
    }
    return g_hash;
}

u64 ComputeResultStage(int stage) {
    return run_stage(stage);
}

u64 ComputeResult(void) {
    test_muldiv();
    test_bits();
    test_wide();
    test_memory();
    test_string();
    test_statemachine();
    return g_hash;
}
