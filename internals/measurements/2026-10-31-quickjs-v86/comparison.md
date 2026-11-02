# dynarmic.js and v86 QuickJS comparison

60/60 task records: 60 passed, 0 failed, 0 missing. Verified: **true**.

Ratio = **v86 time / dynarmic time**; above 1 favors dynarmic. Timings are milliseconds per official invocation. Medians and min–max ranges describe independent paired rounds, not the upstream iterations inside each observation.

| Official case | Paired rounds | dynarmic ms, median [min–max] | v86 ms, median [min–max] | Paired ratio, median [min–max] |
| --- | ---: | ---: | ---: | ---: |
| Richards/Richards | 3/3 | 53.515 [47.831–58.591] | 93.124 [89.320–121.724] | 1.867 [1.740–2.077] |
| DeltaBlue/DeltaBlue | 3/3 | 87.289 [86.357–87.625] | 166.212 [153.200–183.034] | 1.904 [1.774–2.089] |
| Crypto/Encrypt | 3/3 | 93.536 [93.203–97.294] | 170.981 [161.114–180.022] | 1.835 [1.722–1.850] |
| Crypto/Decrypt | 3/3 | 1763.144 [1715.272–1790.700] | 3249.697 [3232.031–3448.850] | 1.833 [1.815–2.011] |
| RayTrace/RayTrace | 3/3 | 807.466 [788.697–855.037] | 1550.528 [1456.084–1776.016] | 1.920 [1.846–2.077] |
| EarleyBoyer/Earley | 3/3 | 112.935 [110.982–146.317] | 204.372 [197.100–205.554] | 1.745 [1.405–1.841] |
| EarleyBoyer/Boyer | 3/3 | 1625.197 [1609.525–1689.591] | 3090.216 [2807.025–3115.088] | 1.829 [1.744–1.917] |
| RegExp/RegExp | 3/3 | 4357.300 [4248.684–4602.894] | 5469.897 [5446.575–5536.203] | 1.271 [1.183–1.287] |
| Splay/Splay | 3/3 | 40.275 [40.148–42.935] | 74.754 [69.856–76.000] | 1.770 [1.734–1.862] |
| NavierStokes/NavierStokes | 3/3 | 1445.565 [1416.253–1465.400] | 2347.847 [2344.778–2377.188] | 1.624 [1.600–1.679] |

| Round | Complete | Eight-suite ratio | Ten-case ratio (secondary) |
| --- | --- | ---: | ---: |
| 0 | true | 1.778 | 1.766 |
| 1 | true | 1.729 | 1.757 |
| 2 | true | 1.700 | 1.716 |

Eight-suite ratio, median [min–max]: **1.729 [1.700–1.778]**.
Ten-case equal-weight ratio, median [min–max]: 1.757 [1.716–1.766].

Each complete round first combines Crypto and EarleyBoyer case ratios geometrically, then weights all eight suites equally. The final summary requires every planned round and case to pass. A complete surviving round is shown separately if another round is incomplete; it is not substituted for the requested experiment.

Paired ratios are computed before summarizing across rounds. Dividing the displayed unpaired timing medians can produce a different answer. Internal official samples are summarized within their observation and are not additional independent rounds.

The JSON retains the experiment metadata and original Worker reports, including iteration counts, elapsed batches, memory and compilation statistics. JSON and CSV retain loadMs and runMs separately. Adaptive iteration counts make whole-call runMs unsuitable for a fixed-work throughput comparison. Preparation and execution durations are not added to official timing.

Recovery metadata is retained in summary.recovery: browser-crash-metadata.json, recovery-2.json, recovery.json. Interruptions and browser restarts are excluded from successful execution timings; recovery costs are not included in the throughput ratios.
