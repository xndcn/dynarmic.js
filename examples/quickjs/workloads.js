/* One unchanged JavaScript source for the browser engine and both QuickJS builds.
 * Timings cover only each kernel call. Parsing, engine setup and reporting are
 * outside these samples; the caller separately measures end-to-end execution. */
(function (global) {
    'use strict';
    const profiles = {
        light: {lcg: 20000, sieve: 8192, pi: 20000},
        full: {lcg: 200000, sieve: 65536, pi: 200000},
    };
    function lcg(n) {
        let x = 12345;
        for (let i = 0; i < n; i++) {
            x = (Math.imul(x, 1664525) + 1013904223) >>> 0;
            x = (x ^ (x >>> 13)) >>> 0;
        }
        return x;
    }
    function sieve(n) {
        const flags = new Uint8Array(n);
        flags.fill(1);
        flags[0] = flags[1] = 0;
        for (let i = 2; i * i < n; i++) {
            if (flags[i]) for (let j = i * i; j < n; j += i) flags[j] = 0;
        }
        let count = 0;
        for (let i = 0; i < n; i++) count += flags[i];
        return count;
    }
    function pi(n) {
        let sum = 0, sign = 1;
        for (let k = 0; k < n; k++) {
            sum += sign / (2 * k + 1);
            sign = -sign;
        }
        return Math.trunc(sum * 4 * 1000000);
    }
    const kernels = {lcg, sieve, pi};
    // Fixed independently checked reference values, not runtime voting.
    const expected = {
        light: {lcg: 2778886796, sieve: 1028, pi: 3141542},
        full: {lcg: 428440788, sieve: 6542, pi: 3141587},
    };
    function run(profile, now) {
        if (!Object.prototype.hasOwnProperty.call(profiles, profile)) throw new RangeError('Unknown workload profile');
        if (typeof now !== 'function') throw new TypeError('A monotonic clock is required');
        const parameters = profiles[profile];
        const outputs = {}, coldMs = {}, samplesMs = {}, summary = {};
        for (const name of Object.keys(kernels)) {
            const samples = [];
            for (let attempt = 0; attempt < 5; attempt++) {
                const start = now();
                const value = kernels[name](parameters[name]);
                const elapsed = now() - start;
                if (!Number.isFinite(elapsed) || elapsed < 0) throw new Error('Invalid monotonic timing');
                if (value !== expected[profile][name]) throw new Error(name + ' result mismatch: ' + value);
                outputs[name] = value;
                if (attempt === 0) coldMs[name] = elapsed;
                if (attempt >= 2) samples.push(elapsed);
            }
            samplesMs[name] = samples;
            const sorted = samples.slice().sort((a, b) => a - b);
            const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
            summary[name] = {medianMs: sorted[1], minMs: sorted[0], maxMs: sorted[2], meanMs: mean,
                stddevMs: Math.sqrt(samples.reduce((sum, x) => sum + (x - mean) * (x - mean), 0) / samples.length)};
        }
        return {profile, parameters, outputs, coldMs, samplesMs, summary, warmupRuns: 1, measuredRuns: 3};
    }
    global.QuickJSBench = {profiles, expected, kernels, run};
})(globalThis);
