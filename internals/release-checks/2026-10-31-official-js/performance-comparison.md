# QuickJS execution-path comparison

Source: the complete local Chromium 133 run recorded in browser-chromium.json on 2026-10-31 (Asia/Shanghai). QuickJS 2025-09-13; V8 benchmark suite v7 from pinned QuickJS extras. No new benchmark run was performed for this analysis.

Metric: measured elapsed time divided by completed benchmark iterations, after the upstream warmup. Module loading and setup are excluded from the per-iteration metric. The ratio is dynarmic.js + ARM QuickJS time / direct QuickJS Wasm time.

| Case | dynarmic.js + QuickJS (ms/iteration) | QuickJS Wasm (ms/iteration) | Time ratio |
| --- | ---: | ---: | ---: |
| Richards/Richards | 48.96 | 3.57 | 13.73× |
| DeltaBlue/DeltaBlue | 85.80 | 6.31 | 13.60× |
| Crypto/Encrypt | 94.07 | 6.38 | 14.75× |
| Crypto/Decrypt | 1764.28 | 114.27 | 15.44× |
| RayTrace/RayTrace | 803.01 | 67.97 | 11.81× |
| EarleyBoyer/Earley | 114.45 | 9.52 | 12.02× |
| EarleyBoyer/Boyer | 1634.57 | 111.89 | 14.61× |
| RegExp/RegExp | 4292.96 | 259.45 | 16.55× |
| Splay/Splay | 40.33 | 3.35 | 12.05× |
| NavierStokes/NavierStokes | 1443.89 | 89.42 | 16.15× |

Equal-case geometric mean: 13.9738×. Equal-suite geometric mean (8 suites): 13.9324×.

One complete harness run per case is retained. The minimum of 32 iterations is not 32 independent experiments. These observations do not supply a multi-run confidence interval or isolate individual translator/compiler/runtime costs.
