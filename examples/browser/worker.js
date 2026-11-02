// Web Worker host for the dynarmic WASM JIT. The JIT compiles guest blocks and
// regions with the synchronous WebAssembly.Module() constructor. A Worker
// avoids blocking the page and main-thread compilation limits in some browsers.
//
// Message: {bin, mode, arch} (arch 0 = ARM32, 1 = AArch64; mode = arm_init
// memory mode). Reply: console text, run time, per-kernel times from the
// guest's SVC #3 marks, and the JIT compile statistics of this run.

importScripts('arm_jit.js');

const LOAD_BASE = 0x10000;
let armPromise = null;
let busy = false;

function checkFeatures() {
    if (typeof WebAssembly !== 'object' || WebAssembly === null) {
        throw new Error('WebAssembly is unavailable. Use a current browser with WebAssembly enabled.');
    }
    // (module (func (return_call 0))) — validate, never execute this recursive function.
    const tailCalls = new Uint8Array([
        0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0,
        3, 2, 1, 0, 10, 6, 1, 4, 0, 18, 0, 11,
    ]);
    // (module (func (param i32) (result v128) (i8x16.splat (local.get 0))))
    const simd = new Uint8Array([
        0, 97, 115, 109, 1, 0, 0, 0, 1, 6, 1, 96, 1, 127, 1, 123,
        3, 2, 1, 0, 10, 8, 1, 6, 0, 32, 0, 253, 15, 11,
    ]);
    const missing = [];
    if (!WebAssembly.validate(tailCalls)) missing.push('tail calls');
    if (!WebAssembly.validate(simd)) missing.push('SIMD');
    if (missing.length) {
        throw new Error(`This browser does not support WebAssembly ${missing.join(' and ')}. Update your browser and try again.`);
    }
}

function getArm() {
    if (!armPromise) {
        checkFeatures();
        const t0 = performance.now();
        armPromise = createArmJit().then((arm) => {
            arm.loadMs = performance.now() - t0;
            return arm;
        }).catch((error) => {
            // A transient module fetch/instantiation failure must not poison
            // every later request handled by this Worker.
            armPromise = null;
            throw error;
        });
    }
    return armPromise;
}

function statsSnapshot(arm) {
    const raw = arm.dynarmicWasmStats || {};
    // arm_init creates a fresh JIT. Read that instance's compile snapshot;
    // top-level legacy fields may still describe the previous architecture.
    const instance = raw.instances?.[String(raw.activeInstance)] || {};
    const snapshot = Object.fromEntries(Object.entries(instance.compile || {})
        .filter(([, value]) => typeof value === 'number'));
    return snapshot;
}

self.onmessage = async (event) => {
    const {bin, mode, arch} = event.data || {};
    // Message handlers can overlap while the module loads. Do not let a second
    // request overwrite the first one's guest RAM or reuse its JIT state.
    if (busy) {
        self.postMessage({ok: false, arch, error: 'Worker is busy. Wait for the current reply before sending another guest.'});
        return;
    }
    busy = true;
    try {
        if (!(bin instanceof ArrayBuffer)) throw new TypeError('guest binary must be an ArrayBuffer');
        if (bin.byteLength === 0) throw new RangeError('guest binary must not be empty');
        if (arch !== 0 && arch !== 1) throw new RangeError('architecture must be 0 (ARM32) or 1 (AArch64)');
        if (!Number.isInteger(mode) || mode < 0 || mode > 2) throw new RangeError('memory mode must be 0, 1 or 2');
        const arm = await getArm();
        const ram = arm._arm_ram();
        const ramSize = arm._arm_ram_size();
        if (bin.byteLength > ramSize - LOAD_BASE) {
            throw new RangeError(`guest binary must contain 1 to ${ramSize - LOAD_BASE} bytes`);
        }
        arm.HEAPU8.fill(0, ram, ram + ramSize);
        arm.HEAPU8.set(new Uint8Array(bin), ram + LOAD_BASE);
        arm._arm_console_clear();
        if (arm._arm_init(mode, arch) !== 0) throw new Error('guest initialization failed');

        const t0 = performance.now();
        const haltReason = arm._arm_run(LOAD_BASE);
        const runMs = performance.now() - t0;
        if (haltReason !== 0x01000000) throw new Error(`guest fault: halt reason 0x${(haltReason >>> 0).toString(16).padStart(8, '0')}`);
        const after = statsSnapshot(arm);
        const stats = {};
        for (const [key, value] of Object.entries(after)) stats[key] = value;

        // Each arm_init creates a new JIT; its counters already describe this run.
        for (const [key, value] of Object.entries(arm.dynarmicWasmStats?.profile || {})) {
            if (typeof value === "number") stats["profile_" + key] = value;
        }

        const marks = Array.from(new Float64Array(arm.HEAPU8.buffer, arm._arm_marks(), arm._arm_mark_count()));
        const kernelMs = marks.slice(1).map((t, i) => t - marks[i]);
        const len = arm._arm_console_len();
        const text = new TextDecoder().decode(arm.HEAPU8.subarray(arm._arm_console(), arm._arm_console() + len));
        self.postMessage({ok: true, arch, text, haltReason, runMs, kernelMs, loadMs: arm.loadMs, stats});
    } catch (e) {
        self.postMessage({ok: false, arch, error: String((e && e.stack) || e)});
    } finally {
        busy = false;
    }
};
