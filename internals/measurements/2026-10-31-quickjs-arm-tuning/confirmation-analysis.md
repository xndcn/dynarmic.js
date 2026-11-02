# ARM QuickJS compiler comparison

66/66 process observations completed; 0 failed. Baseline: **o2-switch**.
Each reported repeat is a fresh Node process. The upstream iteration count and the kernel harness's three samples are retained inside each observation; they are not counted as independent process repeats.

Environment: v24.19.0 / linux x64; 12th Gen Intel(R) Core(TM) i7-12700; 20 logical CPUs; 33314803712 bytes RAM.

| Variant | Guest bytes | Size / baseline | Selected-metric speedup | Eight-suite speedup | Eight-suite round range |
| --- | ---: | ---: | ---: | ---: | ---: |
| o2-switch | 683252 | 1.000× | 1.000 | 1.000 | 1.000–1.000 |
| o2-switch-thumb | 512644 | 0.750× | 1.002 | 0.994 | 0.919–1.013 |

Speedup is baseline time / candidate time; above 1 favors the candidate. Within each process round the eight-suite value weights all eight official suites equally; the table reports the median and range across independent rounds. It is absent unless every case has the requested paired repeats. Selected-metric and ten-case aggregates use the same per-round-first calculation. Selected metrics also include each kernel separately when kernels/full was requested. All round ratios are retained in JSON.

| Variant | Case / metric | Paired processes | Candidate median | Speedup median | Speedup min–max |
| --- | --- | ---: | ---: | ---: | ---: |
| o2-switch | Richards/Richards / msPerIteration | 3 | 44.347 ms | 1.000 | 1.000–1.000 |
| o2-switch | DeltaBlue/DeltaBlue / msPerIteration | 3 | 79.715 ms | 1.000 | 1.000–1.000 |
| o2-switch | Crypto/Encrypt / msPerIteration | 3 | 90.736 ms | 1.000 | 1.000–1.000 |
| o2-switch | Crypto/Decrypt / msPerIteration | 3 | 1648.944 ms | 1.000 | 1.000–1.000 |
| o2-switch | RayTrace/RayTrace / msPerIteration | 3 | 895.673 ms | 1.000 | 1.000–1.000 |
| o2-switch | EarleyBoyer/Earley / msPerIteration | 3 | 110.455 ms | 1.000 | 1.000–1.000 |
| o2-switch | EarleyBoyer/Boyer / msPerIteration | 3 | 1550.218 ms | 1.000 | 1.000–1.000 |
| o2-switch | RegExp/RegExp / msPerIteration | 3 | 4165.437 ms | 1.000 | 1.000–1.000 |
| o2-switch | Splay/Splay / msPerIteration | 3 | 37.224 ms | 1.000 | 1.000–1.000 |
| o2-switch | NavierStokes/NavierStokes / msPerIteration | 3 | 1323.241 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / lcgMs | 3 | 278.186 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / sieveMs | 3 | 72.512 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / piMs | 3 | 118.219 ms | 1.000 | 1.000–1.000 |
| o2-switch-thumb | Richards/Richards / msPerIteration | 3 | 46.323 ms | 0.952 | 0.944–0.958 |
| o2-switch-thumb | DeltaBlue/DeltaBlue / msPerIteration | 3 | 80.810 ms | 0.977 | 0.652–1.004 |
| o2-switch-thumb | Crypto/Encrypt / msPerIteration | 3 | 95.933 ms | 0.946 | 0.939–1.040 |
| o2-switch-thumb | Crypto/Decrypt / msPerIteration | 3 | 1732.128 ms | 0.949 | 0.896–0.952 |
| o2-switch-thumb | RayTrace/RayTrace / msPerIteration | 3 | 770.877 ms | 0.997 | 0.996–1.181 |
| o2-switch-thumb | EarleyBoyer/Earley / msPerIteration | 3 | 106.719 ms | 1.010 | 0.933–1.039 |
| o2-switch-thumb | EarleyBoyer/Boyer / msPerIteration | 3 | 1572.849 ms | 0.992 | 0.975–1.013 |
| o2-switch-thumb | RegExp/RegExp / msPerIteration | 3 | 4248.603 ms | 0.972 | 0.855–1.008 |
| o2-switch-thumb | Splay/Splay / msPerIteration | 3 | 36.729 ms | 1.013 | 1.008–1.025 |
| o2-switch-thumb | NavierStokes/NavierStokes / msPerIteration | 3 | 1266.040 ms | 1.040 | 1.022–1.058 |
| o2-switch-thumb | kernels/full / lcgMs | 3 | 282.715 ms | 0.975 | 0.963–1.013 |
| o2-switch-thumb | kernels/full / sieveMs | 3 | 66.591 ms | 1.089 | 1.075–1.112 |
| o2-switch-thumb | kernels/full / piMs | 3 | 111.297 ms | 1.062 | 1.001–1.102 |

Inspect per-case regressions, the complete-call times, emitted Wasm sizes and compilation counters before selecting a candidate. One process round is a screen; it does not establish a stable performance improvement. Full observations, raw iteration counts, environment, executable hashes and original build manifests remain in the experiment directory.
