// VFP/softfp compute test: float/double arithmetic through real libm-free code.

#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;

// sqrt via the FP instruction directly (avoids libm/__errno in freestanding)
#if defined(__aarch64__)
static float vsqrt_f32(float x) {
    float r;
    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
}
static double vsqrt_f64(double x) {
    double r;
    __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}
#else
static float vsqrt_f32(float x) {
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
}
static double vsqrt_f64(double x) {
    double r;
    __asm__("vsqrt.f64 %0, %1" : "=w"(r) : "w"(x));
    return r;
}
#endif

static u64 g_hash = 1469598103934665603ULL;

static void mix(u64 v) {
    g_hash ^= v;
    g_hash *= 1099511628211ULL;
}
static void mixf(float f) {
    union { float f; u32 u; } u = {f};
    mix(u.u);
}
static void mixd(double d) {
    union { double d; u64 u; } u = {d};
    mix(u.u);
}

static void test_f32(void) {
    float acc = 1.0f;
    for (int i = 1; i < 200; i++) {
        float x = (float)i * 0.5f;
        acc = acc * 1.001f + x;
        acc = acc / 1.5f - 0.25f;
        float sq = acc < 0.0f ? -acc : acc;
#ifdef GUEST_BUILD
        sq = vsqrt_f32(sq);
#else
        sq = __builtin_sqrtf(sq);
#endif
        acc += sq * 0.1f;
        if ((i & 15) == 0)
            mixf(acc);
    }
    mixf(acc);
}

static void test_f64(void) {
    double acc = 1.0;
    for (int i = 1; i < 200; i++) {
        double x = (double)i / 3.0;
        acc = acc * 1.0001 + x;
        acc = acc / 1.7 - 0.125;
        double sq = acc < 0.0 ? -acc : acc;
#ifdef GUEST_BUILD
        sq = vsqrt_f64(sq);
#else
        sq = __builtin_sqrt(sq);
#endif
        acc += sq * 0.01;
        if ((i & 15) == 0)
            mixd(acc);
    }
    mixd(acc);
}

static void test_cvt(void) {
    for (int i = -100; i < 100; i++) {
        float f = (float)i * 1.5f;
        double d = (double)i * 0.25;
        mix((u32)(int)f);           // fptosi
        mix((u32)(int)d);
        mix((u32)(unsigned)(f + 1000.0f));  // fptoui
        mixf((float)(int)i * 2);    // sitofp
        mixd((double)(unsigned)(i + 200));
        mixf((float)d);             // fptrunc
        mixd((double)f);            // fpext
        int ti = (int)f;
        mixf((float)ti);            // roundtrip
    }
}

static void test_cmp(void) {
    float a = 0.0f;
    int lt = 0, gt = 0, eq = 0;
    for (int i = 0; i < 500; i++) {
        a += 0.37f;
        float b = (float)(i % 7) * 3.1f;
        if (a < b) lt++;
        else if (a > b) gt++;
        else eq++;
        double da = (double)a, db = b * 1.5;
        if (da <= db) lt += 2;
        if (da >= db) gt += 3;
        if (da == db) eq += 5;
    }
    mix(lt);
    mix(gt);
    mix(eq);
}

u64 ComputeResultFP(void) {
    test_f32();
    test_f64();
    test_cvt();
    test_cmp();
    return g_hash;
}
