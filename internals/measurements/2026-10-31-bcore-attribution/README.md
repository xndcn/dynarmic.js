# bcore 与 bfin-gcc 的性能归因：整数、分支和内存

> 归档说明（2026-11-02）：bcore 已退出正式 examples 和 Pages。本文描述历史实验；旧 `examples/...` 命令需先按[独立恢复说明](../../experiments/bcore-comparison/README.md)还原实验快照。测量 JSON、原始压缩包和数值保持不变。

这份分析只讨论整数、分支和内存。结论是：**现有大差距的主要可消除部分在 bcore 的 copy-and-patch 执行路径；bfin-gcc 也有局部冗余，但不能把全部差距归为编译器生成了慢程序。分支项的剩余差距比整数项更难归因。**

这里做的是诊断对照，不是新的网页默认成绩。网页仍使用 copy-and-patch + 后台 LLVM 晋升。实验使用独立构建目录和诊断宿主，未修改页面默认设置或 bcore 上游源码。

## 1. 同一个 Blackfin 二进制，只改变执行后端

原始 Blackfin `.bin` 在构建时与页面分发的 `.bin` 做逐字节一致性断言。下表三个 Blackfin 后端使用同一份机器码、相同输入、种子和迭代次数；不是通过重编译客体来获得 LLVM 的改善。

主矩阵运行了 3 个独立 Chrome 149 进程。每个配置/用例在新 Worker 中执行 1 次首次调用、2 次预热、3 次测量，所有调用校验正确。表内耗时是三个会话中位数的中位数；倍率先在同一会话内配对，不能直接用表中两个汇总耗时相除。

| 用例 | bcore CP 停留 tier 0 | bcore 强制 LLVM O1/codegen2 | ARM/dynarmic | 同一 Blackfin 二进制的 CP/LLVM 配对比 |
| --- | ---: | ---: | ---: | ---: |
| 整数混合 | 47.3 ms | 9.0 ms | 7.0 ms | **5.10×**（4.46–5.56） |
| 数据依赖分支 | 48.2 ms | 30.8 ms | 11.2 ms | **1.56×**（1.50–1.57） |
| 顺序内存 | 99.6 ms | 33.6 ms | 13.8 ms | **2.96×**（2.92–3.07） |
| 指针追踪 | 36.7 ms | 16.2 ms | 7.3 ms | **2.20×**（2.12–2.50） |

客体指令包计数也一致：整数每次 **40,000,034**，分支 **20,000,080**，顺序内存 **104,000,041**，指针追踪 **32,000,036**。这些计数在原始二进制的各个后端模式、各次调用中相同。

因此，整数用例约六倍于 ARM 的默认路径差距，并不是这份 Blackfin 二进制不可避免的执行成本。同一程序换成 bcore 的 LLVM 路径就能接近 ARM；不能由此反推 bfin-gcc 的机器码已经最优，但能确认 tier-0 执行质量是主要因素。

这个 LLVM 对照是直接以 O1/codegen2 在首次调用中编译，再按相同流程预热两次，优化等级与网页晋升层相同。它不是声称网页的短测试已经完成后台晋升：之前的默认路径实验中，整数、分支和指针追踪都在晋升安装前结束。CP 对照保留 100000/16 热度设置，接收晋升请求但不交付结果，不启动竞争 CPU 的编译 Worker，用于单独测 tier-0 执行成本。

## 2. bcore 慢在什么地方

### 标量 stencil 保留了每条指令的状态操作与硬件循环判定

从实际运行中捕获了生成的 Wasm，而不仅是阅读 C++ 源码。整数热点为客体 PC `0x100ec`。

CP 的一条乘法先写回 `CpuState.R1`，随后读取 `LB0/LB1`，检查当前客体 PC 是否为硬件循环底部。接下来的加法、移位、异或等指令再次执行各自的状态处理。当前热点循环是普通条件跳转循环，不是硬件循环；输入初始化函数另有 `LSETUP`，不能因此全局禁用硬件循环语义。

捕获的 CP Wasm 中可直接看到这种形状：

```wat
(i32.store offset=4             ;; CpuState.R1
  (local.get $0)
  (i32.mul
    (i32.load offset=24 (local.get $0))
    (i32.load offset=4  (local.get $0))))
;; 紧接着比较 LB0/LB1 与本条指令 PC
(i32.eq (i32.load offset=160 (local.get $0)) (i32.const 65772))
(i32.ne (i32.load offset=164 (local.get $0)) (i32.const 65772))
```

这里要区分**每次都做的判定**与**条件命中才调用的 `bfin_hwloop_step`**：并不是每条指令都实际调用该 helper。

源码相互印证：

- [`bb_stencil.cpp`](https://github.com/op1emu/bcore/blob/df7aee68589424364e74d6cee4def5269721e3e6/src/bb_stencil.cpp) 的 `translate_stencil()` 设置 `visitor.set_careful(true)`，一个标量指令独立生成一个 stencil。
- [`lift_visitor.cpp`](https://github.com/op1emu/bcore/blob/df7aee68589424364e74d6cee4def5269721e3e6/src/lift_visitor.cpp) 的 `lb_absent()` / `pin_bottom()` 在 `careful_` 模式不做相关假设；`emit_epilog_clear()` / `emit_epilog_call()` 处理循环底部判断。
- [`cp_translator.cpp`](https://github.com/op1emu/bcore/blob/df7aee68589424364e74d6cee4def5269721e3e6/src/cp_translator.cpp) 已在 `compose_parallel()` 对并行 packet 使用 hint/guard，普通标量路径尚无等价的整块消除。
- [`cp_emit_wasm.cpp`](https://github.com/op1emu/bcore/blob/df7aee68589424364e74d6cee4def5269721e3e6/src/cp_emit_wasm.cpp) 拼接已编译的 pieces，不再经过一次整块 LLVM 优化。

这些操作在 V8 中仍可能被优化，不能把 WAT 的静态 load/store 数直接视为机器指令数或耗时占比。但它们明确指出了可优化的方向。

### 循环内保留寄存器值有实测收益

LLVM 热点能把客体寄存器值保留在 Wasm locals 中，形成原生 Wasm loop；捕获的整数 CP 热块则通过 `return_call $0` 回到自身。关闭 LLVM 的 `loop_blocks`，保留其余配置和客体二进制，得到：

| 用例 | LLVM loop blocks 开 | 关 | 关闭/开启配对比 |
| --- | ---: | ---: | ---: |
| 整数 | 9.0 ms | 14.2 ms | **1.59×** |
| 分支 | 30.8 ms | 31.8 ms | 0.96×，无稳定收益 |
| 顺序内存 | 33.6 ms | 49.1 ms | **1.46×** |
| 指针追踪 | 16.2 ms | 20.3 ms | **1.25×** |

这验证了循环级优化的影响，但没有把整个 CP/LLVM 差距都解释完。标量 stencil 的跨指令优化、标志位维护、循环判定等还混合在剩余差值中；本实验不能给它们分别分摊精确百分比。

### 页表对依赖加载有明显影响，计数器和事件循环不是主要原因

保持相同客体二进制与 LLVM 优化设置，仅将页表访问换成有边界检查的连续 RAM 路径：

| 用例 | LLVM + 页表 | LLVM + 连续 RAM | 页表/连续 RAM 配对比 |
| --- | ---: | ---: | ---: |
| 整数 | 9.0 ms | 8.8 ms | 1.02× |
| 分支 | 30.8 ms | 25.9 ms | 1.19× |
| 顺序内存 | 33.6 ms | 30.7 ms | 1.08× |
| 指针追踪 | 16.2 ms | 10.5 ms | **1.53×** |

指针追踪每次加载依赖上一次结果，额外页表加载/检查更难隐藏。这个对照只能说明内存路径的成本：网页和 copy-and-patch 仍需要页表，OP-1 还有非连续地址空间与设备映射，不能直接据此换成裸指针模式。

另外：

- 将 CP 热度阈值设成 `UINT32_MAX`、使用不计数的 prologue，没有数量级改善；不同用例的配对差异约在 -6% 到 +5% 的中位水平。热度计数不是大头。
- 记录每个同步 `bcore_step()` 调用的墙钟区间，与 guest 的计时标记求交。整数/分支/内存的事件循环让出空隙中位数约 **0–0.1 ms**，远小于其几十毫秒总耗时。这个指标不是 CPU profiler，也不扣除同步调用内部的 OS 抢占；它排除的是 MessageChannel 等待作为主要解释。

## 3. bfin-gcc 确实有局部冗余，但不是全面慢的证据

只添加 `-fomit-frame-pointer -momit-leaf-frame-pointer`，不改 C 算法，分支叶子函数中的 `LINK 0` / `UNLINK` 消失。

分支基准每次调用的指令包数从 **20,000,080 降至 18,000,078**。其收益在两种后端都能观察到：

| 用例/后端 | 原编译参数 | 省略叶子栈帧 | 原版/新版配对比 |
| --- | ---: | ---: | ---: |
| 分支 / CP | 48.2 ms | 43.3 ms | **1.15×** |
| 分支 / LLVM | 30.8 ms | 23.9 ms | **1.32×** |

整数、顺序内存的热点循环没有因为这个选项变短，收益接近噪声；指针追踪只有很小的变化。进一步把 O2 改为 O3，在保留相同 scalar/no-unroll 约束和省略栈帧选项时，`kernels.s` **逐字节相同**。所以“先换 O3”不能解释或解决这组大差距。

也不能把 Blackfin 比 ARM 多的客体指令全部算成 GCC 缺陷。例如整数循环是 Blackfin 10 个标量 packet、ARM 5 条指令；ARM 有 MLA、移位操作数和 ROR，Blackfin 使用乘加分离、移位/或组合，并且两种 ISA 的标志位语义不同。真实硬件的发射、流水线和标志位成本也不同，指令数不是硬件周期数。

## 4. 组合收益没有简单相乘

又用独立的 3 个浏览器会话做了整数范围内的组合对照。下表均是 LLVM 后端、同样 Quick 输入，倍率在这个新 cohort 内配对；不要把它与上面不同 cohort 的耗时拼接计算：

| 用例 | 原客体 + 页表 | 省略栈帧 + 连续 RAM | 组合配对收益 |
| --- | ---: | ---: | ---: |
| 分支 | 31.8 ms | 27.7 ms | **1.12×**（1.05–1.20） |
| 顺序内存 | 36.1 ms | 33.7 ms | 1.03×（0.96–1.13），无稳定大收益 |
| 指针追踪 | 16.8 ms | 10.5 ms | **1.65×**（1.44–1.72） |

分支的栈帧优化与内存路径优化涉及部分相同的栈访问；收益不独立，加上代码布局/JIT/运行波动，不能把单项倍率相乘。组合后分支和顺序内存仍约为同会话 ARM 的 2.25×、2.34×。这部分仍同时包含 ISA、调用/分支翻译、访存与代码生成差异，本实验没有把它们完全分开。

## 优化优先级

1. **先处理 CP 标量热点的执行成本。** 将已有 parallel-packet hint/guard 思路扩展到标量块，安全地消除已知不可能命中的硬件循环判定；研究跨 stencil 的状态读写和标志位消冗余。这里是源码与生成代码支持的方向，尚未实现，不能把预期收益当作测量结果。
2. **让长期热点更早获得 LLVM 代码，并分别测启动与稳态。** 原页面短项目在后台晋升完成前结束；当前固定两次预热测的是默认启动/晋升过程，不能当作长期 LLVM 稳态。页面默认不在本次分析中改动。
3. **针对依赖加载优化页表路径。** 指针追踪的隔离实验给出了明确收益；需要保留设备、故障和地址空间语义。
4. **编译器侧先处理已验证的叶子栈帧问题。** 分支项有实际收益，其他项不能据此泛化。O3 在这个约束下没有改变内核汇编。

没有真实 Blackfin 硬件或第二个等价 Blackfin 编译器对照，因此不能给出“GCC 编译质量占总差距多少”的完整百分比。能确定的是：原二进制的大部分整数/内存差距可通过后端改变消除，分支存在额外的、可测量的客体代码冗余。

## 方法与复现

- i7-12700、Linux、Chrome for Testing 149.0.7827.55、Node 24；计时进程限制在 CPU `8,10,12,14`。未锁频、未独占整机；只报告大而一致的效应，小差异不作性能结论。
- 主矩阵中本报告统计 108 个整数/分支/内存观测，组合实验 45 个，共 **153 个观测、918 次匹配校验**。原始全矩阵的其他条目没有进入本报告汇总。
- 每个 cohort 3 个新浏览器进程，任务顺序旋转，每个观测独立 Worker；六次调用保持同一输入。倍率按会话配对，再取三个会话中位数与范围；不是 153 次独立浏览器重复。
- 诊断宿主复用页面构建的、关闭 profiler 的 bcore 静态库，C++ O3 / Wasm link O1；为简化实验宿主使用未分割 Wasm，加载不计入热测量。LLVM 模式为 O1/codegen2，页表、chain cap、8 ms slices 等其余关键条件保持一致；只按模式改变明确列出的控制项。
- CP 模式保留热度请求但不交付晋升，也没有后台编译 Worker 的 CPU 竞争；它隔离的是 tier-0 执行成本，不是新的网页默认分数。所有原客体后端的 packet 数一致。
- Wasm 捕获另外运行，未用于计时结果。WAT 包含异常/冷路径，静态 opcode 数不是执行次数。Binaryen 对 LLVM object 的 linking 自定义段会提示警告；这里只读取代码，不用它重写或执行测量二进制。

文件：

- [summary.json](summary.json)：主矩阵，仅整数/分支/内存。
- [combined-summary.json](combined-summary.json)：组合对照。
- [raw-evidence.tar.gz](raw-evidence.tar.gz)：完整观测、输入哈希、构建命令、宿主源码、客体汇编、实际生成的 Wasm/WAT，以及当时使用的采集脚本。
- [code-evidence.json](code-evidence.json)：整数/内存热点模块索引与哈希。
- [build.py](build.py)、[worker.js](worker.js)、[measure.mjs](measure.mjs)、[analyze.py](analyze.py)：复现脚本。

先按 C lab 文档构建正式 bcore 产物，随后在仓库根目录：

```sh
python3 internals/measurements/2026-10-31-bcore-attribution/build.py
cp internals/measurements/2026-10-31-bcore-attribution/worker.js build-bcore-analysis/dist/analysis-worker.js
printf '<!doctype html><title>Attribution</title>' > build-bcore-analysis/dist/index.html
taskset -c 8,10,12,14 node internals/measurements/2026-10-31-bcore-attribution/measure.mjs \
  build-bcore-analysis/dist NEW_RESULTS 3 integer
python3 internals/measurements/2026-10-31-bcore-attribution/analyze.py NEW_RESULTS NEW_SUMMARY
```

`combined` 运行组合对照，`capture` 在独立非计时实验中保存生成的 Wasm。旧原始全矩阵可用 `matrix` 复现；当前默认选择为 `integer`。分析保留先前页面默认与 LLVM-only 成绩，各自配置互不混用。
