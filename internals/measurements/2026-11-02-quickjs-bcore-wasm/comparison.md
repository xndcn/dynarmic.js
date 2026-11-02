# dynarmic.js and bcore QuickJS comparison

60/60 task records: 60 passed, 0 failed, 0 missing. Verified: **true**.

Ratio = **bcore time / dynarmic time**; above 1 favors dynarmic. Timings are milliseconds per official invocation. Medians and min–max ranges describe independent paired rounds, not the upstream iterations inside each observation.

| Official case | Paired rounds | dynarmic ms, median [min–max] | bcore ms, median [min–max] | Paired ratio, median [min–max] |
| --- | ---: | ---: | ---: | ---: |
| Richards/Richards | 3/3 | 45.216 [43.326–46.277] | 90.229 [88.789–91.024] | 1.996 [1.967–2.049] |
| DeltaBlue/DeltaBlue | 3/3 | 79.377 [74.400–79.541] | 193.520 [181.106–194.712] | 2.434 [2.433–2.453] |
| Crypto/Encrypt | 3/3 | 91.879 [90.906–92.985] | 182.894 [174.700–200.525] | 1.991 [1.922–2.157] |
| Crypto/Decrypt | 3/3 | 1697.100 [1652.603–1756.572] | 3042.556 [3021.269–3058.934] | 1.793 [1.720–1.851] |
| RayTrace/RayTrace | 3/3 | 817.097 [757.366–820.241] | 2306.544 [2066.312–2335.572] | 2.823 [2.728–2.847] |
| EarleyBoyer/Earley | 3/3 | 107.523 [106.708–124.118] | 291.467 [284.952–322.191] | 2.670 [2.596–2.711] |
| EarleyBoyer/Boyer | 3/3 | 1547.888 [1502.491–1611.047] | 2847.937 [2712.884–2850.503] | 1.806 [1.769–1.840] |
| RegExp/RegExp | 3/3 | 4226.031 [4057.490–4263.897] | 8082.078 [7942.666–8115.469] | 1.895 [1.879–2.000] |
| Splay/Splay | 3/3 | 37.985 [37.680–40.402] | 86.778 [83.700–90.417] | 2.238 [2.203–2.303] |
| NavierStokes/NavierStokes | 3/3 | 1387.191 [1368.288–1421.644] | 3023.856 [3009.353–3075.725] | 2.180 [2.163–2.199] |

| Round | Complete | Eight-suite ratio | Ten-case ratio (secondary) |
| --- | --- | ---: | ---: |
| 0 | true | 2.197 | 2.167 |
| 1 | true | 2.194 | 2.159 |
| 2 | true | 2.192 | 2.161 |

Eight-suite ratio, median [min–max]: **2.194 [2.192–2.197]**.
Ten-case equal-weight ratio, median [min–max]: 2.161 [2.159–2.167].

Each complete round first combines Crypto and EarleyBoyer case ratios geometrically, then weights all eight suites equally. The final summary requires every planned round and case to pass. A complete surviving round is shown separately if another round is incomplete; it is not substituted for the requested experiment.

Paired ratios are computed before summarizing across rounds. Dividing the displayed unpaired timing medians can produce a different answer. Internal official samples are summarized within their observation and are not additional independent rounds.

The JSON retains the experiment metadata and original Worker reports, including iteration counts, elapsed batches, memory and compilation statistics. JSON and CSV retain loadMs and runMs separately. Adaptive iteration counts make whole-call runMs unsuitable for a fixed-work throughput comparison. Preparation and execution durations are not added to official timing.
