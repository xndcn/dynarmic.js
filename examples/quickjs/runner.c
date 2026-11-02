/* The same embedding host is compiled for ARM32 and WebAssembly. No filesystem,
 * modules, shell or network APIs are exposed to the embedded JavaScript. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "workloads.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* QuickJS's portable allocator cannot discover allocation sizes on all libc
 * implementations. Track them explicitly so its memory limit works equally
 * on newlib and Emscripten, including realloc and interpreter array growth. */
typedef union {
    max_align_t alignment;
    size_t size;
} AllocationHeader;

static size_t allocation_size(const void *pointer) {
    return pointer ? ((const AllocationHeader *)pointer)[-1].size : 0;
}
static void *allocate(JSMallocState *state, size_t size) {
    if (size == 0 || size > SIZE_MAX - sizeof(AllocationHeader)) return NULL;
    const size_t total = size + sizeof(AllocationHeader);
    if (state->malloc_size > state->malloc_limit || total > state->malloc_limit - state->malloc_size) return NULL;
    AllocationHeader *header = malloc(total);
    if (!header) return NULL;
    header->size = size;
    state->malloc_count++;
    state->malloc_size += total;
    return header + 1;
}
static void release(JSMallocState *state, void *pointer) {
    if (!pointer) return;
    state->malloc_count--;
    state->malloc_size -= allocation_size(pointer) + sizeof(AllocationHeader);
    free((AllocationHeader *)pointer - 1);
}
static void *resize(JSMallocState *state, void *pointer, size_t size) {
    if (!pointer) return allocate(state, size);
    if (size == 0) { release(state, pointer); return NULL; }
    const size_t previous = allocation_size(pointer);
    if (size > SIZE_MAX - sizeof(AllocationHeader)) return NULL;
    if (size > previous && (state->malloc_size > state->malloc_limit || size - previous > state->malloc_limit - state->malloc_size)) return NULL;
    AllocationHeader *header = realloc((AllocationHeader *)pointer - 1, size + sizeof(AllocationHeader));
    if (!header) return NULL;
    header->size = size;
    state->malloc_size = state->malloc_size - previous + size;
    return header + 1;
}

static double clock_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    register uint32_t value __asm__("r0");
    __asm__ volatile("svc #4" : "=r"(value) :: "memory");
    /* Extend the wrapping host microsecond clock; runs may start near wrap. */
    static uint32_t previous;
    static uint64_t epoch;
    if (value < previous) epoch += UINT64_C(1) << 32;
    previous = value;
    return (double)(epoch + value) / 1000.0;
#endif
}

static JSValue js_now(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, clock_ms());
}

static size_t memory_limit = 8 * 1024 * 1024;

int qjs_set_memory_limit(unsigned int bytes) {
    if (bytes < 1024 * 1024 || bytes > 192 * 1024 * 1024) return 2;
    memory_limit = bytes;
    return 0;
}

static int execute(const char *source, size_t length, const char *call) {
    const JSMallocFunctions allocator = {allocate, release, resize, allocation_size};
    JSRuntime *rt = JS_NewRuntime2(&allocator, NULL);
    if (!rt) return 3;
    JS_SetMemoryLimit(rt, memory_limit);
    JS_SetMaxStackSize(rt, 512 * 1024);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 4; }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "benchNow", JS_NewCFunction(ctx, js_now, "benchNow", 0));
    JS_FreeValue(ctx, global);
    JSValue value = JS_Eval(ctx, source, length, "bundled-benchmark.js", JS_EVAL_TYPE_GLOBAL);
    int status = 0;
    if (call && !JS_IsException(value)) {
        JS_FreeValue(ctx, value);
        value = JS_Eval(ctx, call, strlen(call), "benchmark", JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, exception);
        printf("QuickJS error: %s\n", message ? message : "unknown exception");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, exception);
        status = 1;
    } else {
        const char *json = JS_ToCString(ctx, value);
        if (json) { puts(json); JS_FreeCString(ctx, json); }
        else status = 5;
    }
    JS_FreeValue(ctx, value);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return status;
}

int qjs_bench(int profile) {
    if (profile != 0 && profile != 1) return 2;
    const char *call = profile ? "JSON.stringify(QuickJSBench.run('full', benchNow))" : "JSON.stringify(QuickJSBench.run('light', benchNow))";
    return execute((const char *)workloads_js, sizeof(workloads_js) - 1, call);
}

/* The caller supplies a NUL-terminated bundled source and excludes the NUL from
 * length. Its final expression must be the JSON report string. This small
 * bridge is for the shipped benchmark sources, not a general sandbox API. */
int qjs_eval(const char *source, unsigned int length) {
    if (!source || length == 0 || length > 4 * 1024 * 1024 || source[length] != '\0') return 2;
    return execute(source, length, NULL);
}

#ifndef __EMSCRIPTEN__
int guest_main(void) {
    volatile const uint32_t *request = (volatile const uint32_t *)0xf000;
    if (request[0] != 2) return qjs_bench(request[0]);
    const uint32_t address = request[1], length = request[2];
    const uint32_t ram_size = request[4];
    if (ram_size < 16 * 1024 * 1024 || ram_size > 256 * 1024 * 1024 ||
        address < 0x100000 || address >= ram_size || length >= ram_size - address ||
        qjs_set_memory_limit(request[3]) != 0) return 2;
    return qjs_eval((const char *)address, length);
}
#endif
