# ARM QuickJS compiler comparison

60/60 process observations completed; 0 failed. Baseline: **o2-switch**.
Each reported repeat is a fresh Node process. The upstream iteration count and the kernel harness's three samples are retained inside each observation; they are not counted as independent process repeats.

Environment: v24.19.0 / linux x64; 12th Gen Intel(R) Core(TM) i7-12700; 20 logical CPUs; 33314803712 bytes RAM.

| Variant | Guest bytes | Size / baseline | Selected-metric speedup | Eight-suite speedup |
| --- | ---: | ---: | ---: | ---: |
| o2-switch | 683252 | 1.000× | 1.000 | incomplete |
| o2-switch-lto | 689892 | 1.010× | 0.980 | incomplete |
| o2-threaded | 685748 | 1.004× | 0.964 | incomplete |
| o2-threaded-lto | 692452 | 1.013× | 0.948 | incomplete |
| o3-switch | 815516 | 1.194× | 1.014 | incomplete |
| o3-switch-lto | 844108 | 1.235× | 0.979 | incomplete |
| o3-threaded | 818588 | 1.198× | 0.963 | incomplete |
| o3-threaded-lto | 847628 | 1.241× | 0.874 | incomplete |
| os-switch | 524980 | 0.768× | 0.789 | incomplete |
| os-switch-lto | 521132 | 0.763× | 0.830 | incomplete |
| os-threaded | 529140 | 0.774× | 0.798 | incomplete |
| os-threaded-lto | 525292 | 0.769× | 0.783 | incomplete |

Speedup is baseline time / candidate time; above 1 favors the candidate. The eight-suite value weights all eight official suites equally and is absent unless every case has the requested paired repeats. Selected-metric aggregates also include each kernel separately when kernels/full was requested.

| Variant | Case / metric | Paired processes | Candidate median | Speedup median | Speedup min–max |
| --- | --- | ---: | ---: | ---: | ---: |
| o2-switch | Richards/Richards / msPerIteration | 1 | 50.076 ms | 1.000 | 1.000–1.000 |
| o2-switch | DeltaBlue/DeltaBlue / msPerIteration | 1 | 112.766 ms | 1.000 | 1.000–1.000 |
| o2-switch | Crypto/Encrypt / msPerIteration | 1 | 124.983 ms | 1.000 | 1.000–1.000 |
| o2-switch | Splay/Splay / msPerIteration | 1 | 70.879 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / lcgMs | 1 | 445.210 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / sieveMs | 1 | 131.355 ms | 1.000 | 1.000–1.000 |
| o2-switch | kernels/full / piMs | 1 | 186.859 ms | 1.000 | 1.000–1.000 |
| o2-switch-lto | Richards/Richards / msPerIteration | 1 | 72.296 ms | 0.693 | 0.693–0.693 |
| o2-switch-lto | DeltaBlue/DeltaBlue / msPerIteration | 1 | 111.286 ms | 1.013 | 1.013–1.013 |
| o2-switch-lto | Crypto/Encrypt / msPerIteration | 1 | 129.940 ms | 0.962 | 0.962–0.962 |
| o2-switch-lto | Splay/Splay / msPerIteration | 1 | 60.989 ms | 1.162 | 1.162–1.162 |
| o2-switch-lto | kernels/full / lcgMs | 1 | 422.784 ms | 1.053 | 1.053–1.053 |
| o2-switch-lto | kernels/full / sieveMs | 1 | 122.183 ms | 1.075 | 1.075–1.075 |
| o2-switch-lto | kernels/full / piMs | 1 | 191.454 ms | 0.976 | 0.976–0.976 |
| o2-threaded | Richards/Richards / msPerIteration | 1 | 67.732 ms | 0.739 | 0.739–0.739 |
| o2-threaded | DeltaBlue/DeltaBlue / msPerIteration | 1 | 118.152 ms | 0.954 | 0.954–0.954 |
| o2-threaded | Crypto/Encrypt / msPerIteration | 1 | 124.975 ms | 1.000 | 1.000–1.000 |
| o2-threaded | Splay/Splay / msPerIteration | 1 | 82.451 ms | 0.860 | 0.860–0.860 |
| o2-threaded | kernels/full / lcgMs | 1 | 431.710 ms | 1.031 | 1.031–1.031 |
| o2-threaded | kernels/full / sieveMs | 1 | 106.365 ms | 1.235 | 1.235–1.235 |
| o2-threaded | kernels/full / piMs | 1 | 186.049 ms | 1.004 | 1.004–1.004 |
| o2-threaded-lto | Richards/Richards / msPerIteration | 1 | 70.052 ms | 0.715 | 0.715–0.715 |
| o2-threaded-lto | DeltaBlue/DeltaBlue / msPerIteration | 1 | 114.398 ms | 0.986 | 0.986–0.986 |
| o2-threaded-lto | Crypto/Encrypt / msPerIteration | 1 | 125.251 ms | 0.998 | 0.998–0.998 |
| o2-threaded-lto | Splay/Splay / msPerIteration | 1 | 64.500 ms | 1.099 | 1.099–1.099 |
| o2-threaded-lto | kernels/full / lcgMs | 1 | 474.545 ms | 0.938 | 0.938–0.938 |
| o2-threaded-lto | kernels/full / sieveMs | 1 | 122.708 ms | 1.070 | 1.070–1.070 |
| o2-threaded-lto | kernels/full / piMs | 1 | 210.871 ms | 0.886 | 0.886–0.886 |
| o3-switch | Richards/Richards / msPerIteration | 1 | 64.228 ms | 0.780 | 0.780–0.780 |
| o3-switch | DeltaBlue/DeltaBlue / msPerIteration | 1 | 112.021 ms | 1.007 | 1.007–1.007 |
| o3-switch | Crypto/Encrypt / msPerIteration | 1 | 127.583 ms | 0.980 | 0.980–0.980 |
| o3-switch | Splay/Splay / msPerIteration | 1 | 56.409 ms | 1.257 | 1.257–1.257 |
| o3-switch | kernels/full / lcgMs | 1 | 445.526 ms | 0.999 | 0.999–0.999 |
| o3-switch | kernels/full / sieveMs | 1 | 113.032 ms | 1.162 | 1.162–1.162 |
| o3-switch | kernels/full / piMs | 1 | 190.805 ms | 0.979 | 0.979–0.979 |
| o3-switch-lto | Richards/Richards / msPerIteration | 1 | 64.005 ms | 0.782 | 0.782–0.782 |
| o3-switch-lto | DeltaBlue/DeltaBlue / msPerIteration | 1 | 114.466 ms | 0.985 | 0.985–0.985 |
| o3-switch-lto | Crypto/Encrypt / msPerIteration | 1 | 133.149 ms | 0.939 | 0.939–0.939 |
| o3-switch-lto | Splay/Splay / msPerIteration | 1 | 64.288 ms | 1.103 | 1.103–1.103 |
| o3-switch-lto | kernels/full / lcgMs | 1 | 454.847 ms | 0.979 | 0.979–0.979 |
| o3-switch-lto | kernels/full / sieveMs | 1 | 116.598 ms | 1.127 | 1.127–1.127 |
| o3-switch-lto | kernels/full / piMs | 1 | 190.309 ms | 0.982 | 0.982–0.982 |
| o3-threaded | Richards/Richards / msPerIteration | 1 | 68.471 ms | 0.731 | 0.731–0.731 |
| o3-threaded | DeltaBlue/DeltaBlue / msPerIteration | 1 | 117.776 ms | 0.957 | 0.957–0.957 |
| o3-threaded | Crypto/Encrypt / msPerIteration | 1 | 138.947 ms | 0.900 | 0.900–0.900 |
| o3-threaded | Splay/Splay / msPerIteration | 1 | 45.742 ms | 1.550 | 1.550–1.550 |
| o3-threaded | kernels/full / lcgMs | 1 | 528.064 ms | 0.843 | 0.843–0.843 |
| o3-threaded | kernels/full / sieveMs | 1 | 122.233 ms | 1.075 | 1.075–1.075 |
| o3-threaded | kernels/full / piMs | 1 | 214.746 ms | 0.870 | 0.870–0.870 |
| o3-threaded-lto | Richards/Richards / msPerIteration | 1 | 74.793 ms | 0.670 | 0.670–0.670 |
| o3-threaded-lto | DeltaBlue/DeltaBlue / msPerIteration | 1 | 125.742 ms | 0.897 | 0.897–0.897 |
| o3-threaded-lto | Crypto/Encrypt / msPerIteration | 1 | 150.875 ms | 0.828 | 0.828–0.828 |
| o3-threaded-lto | Splay/Splay / msPerIteration | 1 | 71.926 ms | 0.985 | 0.985–0.985 |
| o3-threaded-lto | kernels/full / lcgMs | 1 | 498.564 ms | 0.893 | 0.893–0.893 |
| o3-threaded-lto | kernels/full / sieveMs | 1 | 132.979 ms | 0.988 | 0.988–0.988 |
| o3-threaded-lto | kernels/full / piMs | 1 | 208.027 ms | 0.898 | 0.898–0.898 |
| os-switch | Richards/Richards / msPerIteration | 1 | 79.420 ms | 0.631 | 0.631–0.631 |
| os-switch | DeltaBlue/DeltaBlue / msPerIteration | 1 | 133.468 ms | 0.845 | 0.845–0.845 |
| os-switch | Crypto/Encrypt / msPerIteration | 1 | 165.962 ms | 0.753 | 0.753–0.753 |
| os-switch | Splay/Splay / msPerIteration | 1 | 80.883 ms | 0.876 | 0.876–0.876 |
| os-switch | kernels/full / lcgMs | 1 | 533.467 ms | 0.835 | 0.835–0.835 |
| os-switch | kernels/full / sieveMs | 1 | 167.760 ms | 0.783 | 0.783–0.783 |
| os-switch | kernels/full / piMs | 1 | 225.537 ms | 0.829 | 0.829–0.829 |
| os-switch-lto | Richards/Richards / msPerIteration | 1 | 77.171 ms | 0.649 | 0.649–0.649 |
| os-switch-lto | DeltaBlue/DeltaBlue / msPerIteration | 1 | 139.854 ms | 0.806 | 0.806–0.806 |
| os-switch-lto | Crypto/Encrypt / msPerIteration | 1 | 149.675 ms | 0.835 | 0.835–0.835 |
| os-switch-lto | Splay/Splay / msPerIteration | 1 | 73.623 ms | 0.963 | 0.963–0.963 |
| os-switch-lto | kernels/full / lcgMs | 1 | 508.933 ms | 0.875 | 0.875–0.875 |
| os-switch-lto | kernels/full / sieveMs | 1 | 143.041 ms | 0.918 | 0.918–0.918 |
| os-switch-lto | kernels/full / piMs | 1 | 231.957 ms | 0.806 | 0.806–0.806 |
| os-threaded | Richards/Richards / msPerIteration | 1 | 80.427 ms | 0.623 | 0.623–0.623 |
| os-threaded | DeltaBlue/DeltaBlue / msPerIteration | 1 | 135.122 ms | 0.835 | 0.835–0.835 |
| os-threaded | Crypto/Encrypt / msPerIteration | 1 | 164.764 ms | 0.759 | 0.759–0.759 |
| os-threaded | Splay/Splay / msPerIteration | 1 | 73.420 ms | 0.965 | 0.965–0.965 |
| os-threaded | kernels/full / lcgMs | 1 | 542.085 ms | 0.821 | 0.821–0.821 |
| os-threaded | kernels/full / sieveMs | 1 | 149.194 ms | 0.880 | 0.880–0.880 |
| os-threaded | kernels/full / piMs | 1 | 248.787 ms | 0.751 | 0.751–0.751 |
| os-threaded-lto | Richards/Richards / msPerIteration | 1 | 80.474 ms | 0.622 | 0.622–0.622 |
| os-threaded-lto | DeltaBlue/DeltaBlue / msPerIteration | 1 | 139.767 ms | 0.807 | 0.807–0.807 |
| os-threaded-lto | Crypto/Encrypt / msPerIteration | 1 | 172.170 ms | 0.726 | 0.726–0.726 |
| os-threaded-lto | Splay/Splay / msPerIteration | 1 | 74.925 ms | 0.946 | 0.946–0.946 |
| os-threaded-lto | kernels/full / lcgMs | 1 | 546.676 ms | 0.814 | 0.814–0.814 |
| os-threaded-lto | kernels/full / sieveMs | 1 | 152.550 ms | 0.861 | 0.861–0.861 |
| os-threaded-lto | kernels/full / piMs | 1 | 250.476 ms | 0.746 | 0.746–0.746 |

Inspect per-case regressions, the complete-call times, emitted Wasm sizes and compilation counters before selecting a candidate. One process round is a screen; it does not establish a stable performance improvement. Full observations, raw iteration counts, environment, executable hashes and original build manifests remain in the experiment directory.
