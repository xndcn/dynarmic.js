# 现有 QuickJS JS benchmark：dynarmic.js 与 bcore Wasm

> 归档说明（2026-11-02）：bcore 已退出正式 examples 和 Pages。本文描述历史实验；旧 `examples/...` 命令需先按[独立恢复说明](../../experiments/bcore-comparison/README.md)还原实验快照。测量 JSON、原始压缩包和数值保持不变。

运行的是既有 QuickJS extras V8 v7 的八套、十个 JavaScript 测试项：Richards、DeltaBlue、Crypto/Encrypt、Crypto/Decrypt、RayTrace、EarleyBoyer/Earley、EarleyBoyer/Boyer、RegExp、Splay、NavierStokes。没有改 benchmark 算法、规模、预热或校验，也没有用之前的 C 微基准替代。

比较路径为 **ARM QuickJS → dynarmic.js Wasm** 和 **Blackfin QuickJS → bcore Wasm**；不包含直接编译为 Wasm 的 QuickJS。此次为 bcore 增加了执行现有 benchmark 的裸机运行适配，复用现有 ARM Worker 和官方 JS adapter。

**dynarmic.js 在全部十项中耗时更低。按八套基准等权汇总，bcore 耗时为 dynarmic.js 的 2.194×，三轮范围 2.192–2.197×。** 三轮共 60 个观测、30 个配对，全部通过，正式收集没有失败、超时、重试或浏览器重启恢复。

单位为 ms/次，时间为三轮中位数；倍率先逐轮配对再取中位数，因此不一定等于展示的两列时间直接相除。

| 测试项 | dynarmic.js | bcore Wasm | bcore/dynarmic 配对比 | 三轮倍率范围 |
| --- | ---: | ---: | ---: | ---: |
| Richards/Richards | 45.2 | 90.2 | 2.00× | 1.97–2.05× |
| DeltaBlue/DeltaBlue | 79.4 | 193.5 | 2.43× | 2.43–2.45× |
| Crypto/Encrypt | 91.9 | 182.9 | 1.99× | 1.92–2.16× |
| Crypto/Decrypt | 1697.1 | 3042.6 | 1.79× | 1.72–1.85× |
| RayTrace/RayTrace | 817.1 | 2306.5 | 2.82× | 2.73–2.85× |
| EarleyBoyer/Earley | 107.5 | 291.5 | 2.67× | 2.60–2.71× |
| EarleyBoyer/Boyer | 1547.9 | 2847.9 | 1.81× | 1.77–1.84× |
| RegExp/RegExp | 4226.0 | 8082.1 | 1.90× | 1.88–2.00× |
| Splay/Splay | 38.0 | 86.8 | 2.24× | 2.20–2.30× |
| NavierStokes/NavierStokes | 1387.2 | 3023.9 | 2.18× | 2.16–2.20× |

三轮八套等权比为 2.1966、2.1944、2.1916；十个子项直接等权的次要汇总为 2.161×。这次数据描述本机的完整执行路径，不能外推成所有浏览器、所有 JS 程序的固定倍率。

所有 bcore 观测在结束时均无晋升失败，提交/安装块数为 167–892，执行 Worker 的 deferred LLVM 加载次数均为 0。正式计时没有人为等待全部晋升完成；结束统计不代表每个块在预热结束时的 tier。

详细结果见 [comparison.md](comparison.md)、[comparison.json](comparison.json) 和 [comparison.csv](comparison.csv)。[validation.json](validation.json) 独立复算了全部倍率、验证配对处于同一浏览器进程，并确认输入哈希没有变化。

## 相同设置与实际差异

| 项目 | dynarmic.js + QuickJS | bcore Wasm + QuickJS |
| --- | --- | --- |
| QuickJS | 2025-09-13，同一源码 SHA-256、UTC 适配 | 相同 |
| 客体编译 | ARM GCC 10.3.1，O2，switch，无 LTO | ADI bfin GCC 4.5.3，O2，switch，无 LTO |
| 客体 ISA | ARMv7-A，ARM state，VFPv3/softfp ABI | BF532，软件 binary64 |
| C 运行库 | ARM newlib 3.3.0 | 工具链自带 newlib 1.19.0 |
| 客体 RAM / QuickJS 分配上限 | 256 MiB / 192 MiB | 相同 |
| 计时 | 宿主 performance.now 的微秒时钟，经 SVC | 同一时钟，经 MMIO |
| 模拟器 | 既有 Release arm_bench_jit，无 profile/诊断 | 固定 bcore Release，CP 起步、后台 LLVM 晋升 |
| 初始执行 Worker Wasm memory | 384 MiB | 384 MiB；另有编译 Worker |

bcore 使用已对齐 OP-1 网页的默认设置：copy-and-patch hints/split，cold threshold 10，batch 32/age 65536，remote tier 100000/16，LLVM O1/codegen2，light tier-1 关闭，8 ms 时间片，MessageChannel 让出，page-table RAM，chain cap 2^24，host cycles 开启，steady skipping 关闭。后端修订为 `df7aee68589424364e74d6cee4def5269721e3e6`，Emscripten 4.0.9，Wasm LLVM 23.1.2。

内存规模、地址映射和裸机 guest 为本测试适配；没有固件、设备、音频节流或设备期限。这是完整 QuickJS 执行路径的比较，不能单独归因为两种模拟器的指令吞吐，也不能忽略编译器、libc 与浮点实现的差异。

## 计时与统计

每个测试项保留上游一秒预热、一秒测量批次和至少 32 次调用。报告的是测量时间除以调用次数的 **ms/次**；自适应迭代数不同，整个 Worker 的运行时间不能当成同工作量的性能比。

完成 3 轮，每轮新 Chromium 进程，每项新 Worker/模拟器；轮换 case 顺序并交替两条路径的先后。每个配对必须处于同一浏览器进程。先在每轮计算 bcore/dynarmic，再汇总三轮中位数和最小/最大值。总倍率先对 Crypto 和 EarleyBoyer 各自的两个子项取几何平均，然后对八套基准等权取几何平均。范围不是置信区间。

环境为 Intel Core i7-12700、Linux、Chrome for Testing 149.0.7827.55，亲和性 `8,10,12,14`。没有锁频或独占整机，其他任务未被停止；本任务在正式收集期间不再构建或运行其他性能测试。程序加载/初始化在正式计时之外，后台晋升和调度若发生在测量窗口内则包含在计时中。结束时的晋升计数不能证明测量开始前所有热点已晋升。

原有断言继续执行。Encrypt 单独运行时的解密往返、Decrypt 的预先加密、Splay teardown 都保留。RegExp 和 NavierStokes 上游没有独立的数值结果断言，不将正常完成表述为独立数值验证。

## 适配验证与被排除的初次收集

- 原样 QuickJS 引擎源码与 ARM manifest 的哈希一致，计时和输出桥接之外复用现有 runner/syscalls。
- GCC 4.5 没有 C11 max_align_t，适配用符合 Blackfin ABI 的 union 保持 8 字节分配头。QuickJS 不使用 fenv API，兼容头仅满足其未使用的 include。
- JS 基础运算、双精度、正则、宿主时钟和 20 MiB 跨页读写验证通过。
- 原有 Full LCG/sieve/pi 得到 `428440788 / 6542 / 3141587`，全部匹配既有独立 reference；后台 236 个晋升任务全部安装，没有失败。
- 首次不完整收集发现由 C-lab 继承的 16 MiB marker 地址落入扩大的 RAM 范围。移除该地址拦截，重新验证并从头收集。旧观测、旧 host、manifest 和原因保存在 `superseded-before-mmio-fix`；它们不进入正式性能统计，也没有按快慢挑选结果。

## 复现

参见 [bcore QuickJS harness](../../experiments/bcore-comparison/README.md)。[raw-evidence.tar.gz](raw-evidence.tar.gz) 保留构建和收集脚本、生成宿主、构建 manifest、旧观测的排除原因、全部结果、工具链库哈希和验证记录。运行二进制保留在本地 build-bcore-quickjs/dist，证据包以 SHA-256 标识它们。正式分析器检查配对、上游计时协议、退出状态和后台编译失败；缺失或失败的 case 会阻止总分。

没有修改已有 JS benchmark、Pages 默认配置或上游 bcore，也没有发布或提交。
