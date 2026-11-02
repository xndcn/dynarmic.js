# dynarmic 与 bcore native：相同 C 工作量的性能对比

> 归档说明（2026-11-02）：bcore 已退出正式 examples 和 Pages。本文描述历史实验；旧 `examples/...` 命令需先按[独立恢复说明](../../experiments/bcore-comparison/README.md)还原实验快照。测量 JSON、原始压缩包和数值保持不变。

本次运行的是两者真正的 **x86-64 native JIT**，不经过 JavaScript、Wasm 或浏览器。仅测整数、分支与内存；没有纳入浮点。

**bcore native 默认配置的耗时约为 dynarmic 的 1.206×**，五个会话的等权几何平均比分布在 **1.166–1.207×**。这里的“默认”指 OP-1 native 的后端设置：copy-and-patch 起步，后台 LLVM 晋升 `30000/16`，light tier-1 开启。它不是 bcore standalone `emu` 不带参数的设置，也不是网页的 `100000/16` remote Worker 配置。

五组独立收集会话，每组 3 轮，每个观测启动新进程和新引擎；共 **240 个观测、1,440 次校验全部通过**，没有丢弃或重试失败会话。每个观测中的六次调用依次为首次、两次预热、三次测量。720 个测量调用不是 720 次独立实验。

## 正式对比：native 默认配置

单位 ms，越低越好。耗时为五个会话中位数的中位数。倍率先在同一轮内配对，再汇总到会话、五会话，因此不一定等于表中汇总耗时相除。范围是五个会话倍率的最小/最大值，不是置信区间。

| 基准 | 每次调用迭代数 | dynarmic native | bcore native 默认 | bcore/dynarmic 配对比 | 会话范围 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 整数混合 | 16,000,000 | 26.45 | 38.60 | 1.458× | 1.324–1.466× |
| 数据依赖分支 | 4,000,000 | 38.89 | 48.54 | 1.246× | 1.206–1.324× |
| 顺序内存 | 32,000,000 | 83.23 | 95.14 | 1.122× | 1.070–1.170× |
| 指针追踪 | 16,000,000 | 50.74 | 54.83 | 1.063× | 1.060–1.083× |

在这组工作负载与配置下，dynarmic 四项都更快，指针追踪最接近，整数差距最大。结论不等于完整 OP-1 固件的性能，也不是相同 ISA/机器码的后端比较。

## 诊断对照：不改变 Blackfin 二进制

额外测了禁用晋升的 CP 组，以及禁用 CP、直接使用 LLVM IR O1 / codegen O2 的组。两组仍保留相同页表访问、输入、计时协议和客体二进制；它们不代表 native 默认设置。CP 模式仍允许 bcore 对没有 stencil 的指令使用其回退路径。

| 基准 | bcore 默认 | bcore 禁用晋升 CP | bcore 直接 LLVM | CP/LLVM 配对比 |
| --- | ---: | ---: | ---: | ---: |
| 整数混合 | 38.60 | 158.34 | 38.47 | 4.123× |
| 数据依赖分支 | 48.54 | 78.76 | 47.92 | 1.612× |
| 顺序内存 | 95.14 | 341.65 | 91.06 | 3.737× |
| 指针追踪 | 54.83 | 116.16 | 53.76 | 2.154× |

四项等权汇总，禁用晋升 CP 是 dynarmic 的 **3.258×**，直接 LLVM 是 **1.207×**，CP/LLVM 为 **2.723×**。默认组与直接 LLVM 组接近，符合后台晋升能够改善执行的预期；但此次 release 构建没有开启 profiler 或逐块 tier 记录，**不能仅凭接近的耗时断言每个块都已晋升**。固定两次预热也不是完成晋升的保证。

## 首次调用与准备时间

首次调用包括在其计时标记内发生的翻译/编译开销，输入初始化在标记外。准备时间包括加载二进制、RAM/页表和引擎初始化，另外报告。后台编译线程的析构等待在最后标记之外；原始报告另保留整个子进程墙钟时间。

| 基准 | dynarmic 首次 | bcore 默认首次 | bcore CP 首次 | bcore LLVM 首次 | dynarmic 准备 | bcore 默认准备 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 整数混合 | 25.86 | 82.16 | 159.29 | 103.31 | 10.87 | 12.11 |
| 数据依赖分支 | 39.36 | 67.96 | 79.05 | 119.19 | 10.93 | 12.05 |
| 顺序内存 | 84.96 | 134.68 | 334.19 | 163.94 | 11.03 | 12.08 |
| 指针追踪 | 51.70 | 80.69 | 115.99 | 119.49 | 10.91 | 11.92 |

## 环境与构建

- Intel Core i7-12700，Linux 6.8.0-138，GCC/G++ 12.3.0，LLVM 15.0.7。
- 所有被测进程及其后台线程继承相同 CPU 亲和性 `8,10,12,14`（四个物理 P-core 的一个线程）。未锁频、未独占整机，其他任务未停止；每组记录前后 load average。测量期间本任务没有执行构建或其他性能测试。
- 两个宿主均为 Release `-O3 -DNDEBUG`，x86-64 ELF。Dynarmic 使用其 native emitter 和安全默认优化、块链接、cycle counting，不经 LLVM；bcore native 使用 LLVM 15 支持的 x86-64 CP stencils 和 LLVM JIT。
- Dynarmic 上游修订：`a41c380246d3d9f9874f0f792d234dc0cc17c180`，干净的仓库固定子模块，没有使用根 CMake 中的 Wasm 后端。
- bcore 修订：`df7aee68589424364e74d6cee4def5269721e3e6`，从 tracked-clean Git 树导出独立 snapshot 构建，没有复用上游目录中来源不明/开启 profile 的现成库。
- OP-1 native 默认设置审计：`a2ff78e3ec866384225545caa0e77343d754f199` 的 `gui/main.cpp`、`src/cpu/cpu.cpp`、`src/cpu/cpu.h`，对应文件哈希与副本保存在证据包中。
- bcore 默认启用 CP hints/split、chaining、lazy state、loop blocks、host cycles、IDLE waits、steady skipping；`SYSCFG=0x30`，chain cap `2^24`；后台晋升 `30000/16`、light tier-1、普通 LLVM pipeline O1/codegen2。编译线程 nice 10，无额外单独绑核，编译完成即发布。CPU 执行线程和后台编译可以并发，并非双方固定相同线程数。
- bcore profiling、perf JIT events、chained-block 诊断计数、LLVM verifier 均关闭。native 没有浏览器的 cold tier、8 ms 时间片或 MessageChannel。
- 两边统一 **16 MiB RAM、4 KiB 页表**，没有给 dynarmic 开启基于故障处理的 4 GiB fastmem。这个结果不声称是所有配置下的极限吞吐。

## 工作量与统计边界

ARM 和 Blackfin **沿用页面中的原始二进制**，构建脚本验证它们的 artifact SHA-256 及共同 `kernels.c` / `kernels.h` / `guest.c` 来源。ARM 仍由 Zig 0.17.0 / Clang 22.1.8 O2 scalar 编译；Blackfin 仍由 ADI GCC 4.5.3 O2 BF532 编译。宿主 C++ 编译器相同不意味着客体编译器或指令集相同。

使用 C lab **Extended/full** 工作量、种子 `0x12345678`、独立 reference 校验和。四项分别是 1600 万、400 万、3200 万、1600 万次循环。此前网页默认报告使用 Quick，不能直接拿本表的毫秒数与历史网页毫秒数相除；本次只在同一轮 native 运行之间计算性能比。

ARM 的 SVC 3 与 Blackfin 的 MMIO phase marker 调用同一个 `steady_clock` 实现计时。程序初始化/输入重置不在区间内；host cycle accounting 不是报告耗时来源。只有裸机计算，没有固件、NAND、显示、设备中断/计时器期限和音频节流。

每个收集会话包含 3 轮，4 配置串行执行并旋转 case/runtime 顺序。由于配置数为 4、轮数为 3，一个会话内不是完整 Latin square；顺序继续在五会话之间旋转，原始报告保留每项位置。分析先取三个 measured calls 的中位数，再配对同轮倍率，再对轮/会话汇总，等权几何平均只覆盖四个非浮点 case。

## 验证和复现

- Quick 冒烟：16 个观测、96 个匹配校验。
- Full 正式收集：5 会话 × 3 轮 × 4 基准 × 4 配置 = 240 个观测、1,440 个匹配校验。
- 两个宿主都用错误预期值验证了 checksum mismatch 会导致非零退出。
- 每组前后验证被测可执行文件、guest、reference 和 manifest 哈希未变；原始结果保留 stdout/stderr、命令、进程退出码与总耗时。

文件：

- [summary.json](summary.json)：完整会话统计、热/首次/准备时间与倍率。
- [samples.csv](samples.csv)：240 个观测的全部六次调用时间。
- [raw-reports.tar.gz](raw-reports.tar.gz)：完整会话、构建 manifest、源码、构建 flags、配置审计与校验记录。
- [构建及收集工具](../../experiments/bcore-comparison/README.md)。

```sh
python3 examples/c-bench/native/build.py --bcore-source /path/to/pinned/bcore
# 构建结束后再运行；按机器选择有效 CPU 编号。
taskset -c 8,10,12,14 python3 examples/c-bench/native/measure.py \
  --build build-c-bench-native --profile full --sessions 5 --rounds 3 --output NEW_RESULTS
python3 examples/c-bench/native/analyze.py NEW_RESULTS NEW_SUMMARY
```

审计本次结果时，解压原始包，将其中 `performance/` 目录交给分析器。此次只增加 native 基准工具和结果，没有修改先前页面的后端设置，也没有提交或部署。
