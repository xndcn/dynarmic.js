# ARM QuickJS compiler comparison

70/70 process observations completed; 0 failed. Baseline: **o2-switch**.
Each reported repeat is a fresh Node process. The upstream iteration count and the kernel harness's three samples are retained inside each observation; they are not counted as independent process repeats.

Environment: v24.19.0 / linux x64; 12th Gen Intel(R) Core(TM) i7-12700; 20 logical CPUs; 33314803712 bytes RAM.

| Variant | Guest bytes | Size / baseline | Selected-metric speedup | Eight-suite speedup | Eight-suite round range |
| --- | ---: | ---: | ---: | ---: | ---: |
| o2-switch | 683252 | 1.000× | 1.000 | incomplete | incomplete–incomplete |
| o3-switch | 815516 | 1.194× | 0.955 | incomplete | incomplete–incomplete |
| o3-threaded | 818588 | 1.198× | 0.889 | incomplete | incomplete–incomplete |
| o2-switch-thumb | 512644 | 0.750× | 0.927 | incomplete | incomplete–incomplete |
| o3-switch-thumb | 604604 | 0.885× | 0.915 | incomplete | incomplete–incomplete |
| o2-threaded-no-gcse | 691252 | 1.012× | 0.943 | incomplete | incomplete–incomplete |
| os-threaded-no-crossjumping | 544564 | 0.797× | 0.769 | incomplete | incomplete–incomplete |

Speedup is baseline time / candidate time; above 1 favors the candidate. Within each process round the eight-suite value weights all eight official suites equally; the table reports the median and range across independent rounds. It is absent unless every case has the requested paired repeats. Selected-metric and ten-case aggregates use the same per-round-first calculation. Selected metrics also include each kernel separately when kernels/full was requested. All round ratios are retained in JSON.

| Variant | Case / metric | Paired processes | Candidate median | Speedup median | Speedup min–max |
| --- | --- | ---: | ---: | ---: | ---: |
| o2-switch | Richards/Richards / msPerIteration | 2 | 80.498 ms | 1.000 | 1.000–1.000 |
| o2-switch | DeltaBlue/DeltaBlue / msPerIteration | 2 | 86.955 ms | 1.000 | 1.000–1.000 |
| o2-switch | Crypto/Encrypt / msPerIteration | 2 | 98.586 ms | 1.000 | 1.000–1.000 |
| o2-switch | Splay/Splay / msPerIteration | 2 | 39.884 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / lcgMs | 2 | 290.390 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / sieveMs | 2 | 73.928 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / piMs | 2 | 125.077 ms | 1.000 | 1.000–1.000 |
| o3-switch | Richards/Richards / msPerIteration | 2 | 79.351 ms | 1.043 | 0.957–1.129 |
| o3-switch | DeltaBlue/DeltaBlue / msPerIteration | 2 | 111.650 ms | 0.805 | 0.660–0.950 |
| o3-switch | Crypto/Encrypt / msPerIteration | 2 | 102.094 ms | 0.966 | 0.960–0.972 |
| o3-switch | Splay/Splay / msPerIteration | 2 | 41.015 ms | 0.972 | 0.972–0.973 |
| o3-switch | kernels/full / lcgMs | 2 | 287.625 ms | 1.010 | 1.008–1.011 |
| o3-switch | kernels/full / sieveMs | 2 | 78.139 ms | 0.947 | 0.919–0.975 |
| o3-switch | kernels/full / piMs | 2 | 127.724 ms | 0.980 | 0.962–0.998 |
| o3-threaded | Richards/Richards / msPerIteration | 2 | 81.270 ms | 1.030 | 0.918–1.141 |
| o3-threaded | DeltaBlue/DeltaBlue / msPerIteration | 2 | 157.001 ms | 0.663 | 0.394–0.931 |
| o3-threaded | Crypto/Encrypt / msPerIteration | 2 | 108.711 ms | 0.907 | 0.895–0.920 |
| o3-threaded | Splay/Splay / msPerIteration | 2 | 40.742 ms | 0.979 | 0.958–0.999 |
| o3-threaded | kernels/full / lcgMs | 2 | 307.048 ms | 0.946 | 0.939–0.952 |
| o3-threaded | kernels/full / sieveMs | 2 | 82.506 ms | 0.897 | 0.874–0.920 |
| o3-threaded | kernels/full / piMs | 2 | 136.099 ms | 0.919 | 0.914–0.924 |
| o2-switch-thumb | Richards/Richards / msPerIteration | 2 | 84.243 ms | 1.002 | 0.876–1.127 |
| o2-switch-thumb | DeltaBlue/DeltaBlue / msPerIteration | 2 | 146.970 ms | 0.668 | 0.442–0.894 |
| o2-switch-thumb | Crypto/Encrypt / msPerIteration | 2 | 119.757 ms | 0.840 | 0.717–0.962 |
| o2-switch-thumb | Splay/Splay / msPerIteration | 2 | 39.010 ms | 1.023 | 0.977–1.068 |
| o2-switch-thumb | kernels/full / lcgMs | 2 | 302.638 ms | 0.960 | 0.959–0.960 |
| o2-switch-thumb | kernels/full / sieveMs | 2 | 69.978 ms | 1.057 | 1.037–1.077 |
| o2-switch-thumb | kernels/full / piMs | 2 | 117.494 ms | 1.064 | 1.050–1.078 |
| o3-switch-thumb | Richards/Richards / msPerIteration | 2 | 78.983 ms | 1.047 | 0.962–1.133 |
| o3-switch-thumb | DeltaBlue/DeltaBlue / msPerIteration | 2 | 102.495 ms | 0.849 | 0.830–0.868 |
| o3-switch-thumb | Crypto/Encrypt / msPerIteration | 2 | 138.984 ms | 0.711 | 0.668–0.754 |
| o3-switch-thumb | Splay/Splay / msPerIteration | 2 | 42.031 ms | 0.949 | 0.948–0.950 |
| o3-switch-thumb | kernels/full / lcgMs | 2 | 300.240 ms | 0.967 | 0.962–0.972 |
| o3-switch-thumb | kernels/full / sieveMs | 2 | 78.436 ms | 0.943 | 0.926–0.960 |
| o3-switch-thumb | kernels/full / piMs | 2 | 126.775 ms | 0.987 | 0.985–0.988 |
| o2-threaded-no-gcse | Richards/Richards / msPerIteration | 2 | 81.992 ms | 0.984 | 0.976–0.992 |
| o2-threaded-no-gcse | DeltaBlue/DeltaBlue / msPerIteration | 2 | 96.650 ms | 0.900 | 0.882–0.918 |
| o2-threaded-no-gcse | Crypto/Encrypt / msPerIteration | 2 | 107.305 ms | 0.919 | 0.916–0.921 |
| o2-threaded-no-gcse | Splay/Splay / msPerIteration | 2 | 40.938 ms | 0.974 | 0.973–0.975 |
| o2-threaded-no-gcse | kernels/full / lcgMs | 2 | 303.880 ms | 0.956 | 0.947–0.964 |
| o2-threaded-no-gcse | kernels/full / sieveMs | 2 | 77.524 ms | 0.954 | 0.951–0.957 |
| o2-threaded-no-gcse | kernels/full / piMs | 2 | 135.922 ms | 0.920 | 0.917–0.924 |
| os-threaded-no-crossjumping | Richards/Richards / msPerIteration | 2 | 95.967 ms | 0.878 | 0.770–0.986 |
| os-threaded-no-crossjumping | DeltaBlue/DeltaBlue / msPerIteration | 2 | 109.921 ms | 0.791 | 0.781–0.801 |
| os-threaded-no-crossjumping | Crypto/Encrypt / msPerIteration | 2 | 147.099 ms | 0.680 | 0.601–0.760 |
| os-threaded-no-crossjumping | Splay/Splay / msPerIteration | 2 | 44.083 ms | 0.905 | 0.898–0.913 |
| os-threaded-no-crossjumping | kernels/full / lcgMs | 2 | 380.083 ms | 0.764 | 0.762–0.766 |
| os-threaded-no-crossjumping | kernels/full / sieveMs | 2 | 106.079 ms | 0.698 | 0.669–0.727 |
| os-threaded-no-crossjumping | kernels/full / piMs | 2 | 177.151 ms | 0.708 | 0.683–0.732 |

Inspect per-case regressions, the complete-call times, emitted Wasm sizes and compilation counters before selecting a candidate. One process round is a screen; it does not establish a stable performance improvement. Full observations, raw iteration counts, environment, executable hashes and original build manifests remain in the experiment directory.
