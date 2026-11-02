# bcore native 与 Dynarmic：从客体指令到 x86-64 热循环的归因

> 归档说明（2026-11-02）：bcore 已退出正式 examples 和 Pages。本文描述历史实验；旧 `examples/...` 命令需先按[独立恢复说明](../../experiments/bcore-comparison/README.md)还原实验快照。测量 JSON、原始压缩包和数值保持不变。

这次差距来自三个层面共同作用：**Blackfin 客体程序的指令与标志位语义、bcore 的跨块状态/内存访问方式，以及具体的 x86 指令选择**。不同基准的主因不同。独立跟踪确认，默认 native 配置下四项的主要热点都发生了 LLVM 晋升；热循环不能统一用“仍在 copy-and-patch”解释。

最有力的三个对照结果是：整数热点仅将一条等价 `SHLD` 换为 `ROL`，cycles 减少约 **20%**；分支项仅重编译 Blackfin 客体以省略叶函数栈帧，cycles 减少约 **16%**；指针追踪将页表路径换为有界连续 RAM 路径，cycles 减少约 **44%**。这些对照改变的层次不同，收益不能相加，也不代表已经对完整 OP-1 实现了这些优化。

本报告只覆盖整数、分支与内存。原始 native 性能结论仍以[五会话报告](../2026-11-01-native-c-bench-performance/README.md)为准：bcore 默认四项耗时等权几何平均为 Dynarmic 的 1.206×。这里新增 PMU、机器码和受控改动，回答“为什么”。

## 测量方法与可信边界

沿用原始 ARM/Blackfin 客体、相同 C 源码、Extended/full 输入及校验和。每次调用的 C 循环次数分别为 1600 万、400 万、3200 万、1600 万。双方使用 16 MiB RAM 与 4 KiB 页表；Dynarmic 未开启基于故障处理的 fastmem。

- Intel Core i7-12700，Linux 6.8.0-138，GCC 12.3，LLVM 15.0.7；亲和性为 `8,10,12,14`，都是 P-core 线程。未锁频、未独占机器。
- Dynarmic `a41c380246d3d9f9874f0f792d234dc0cc17c180`；bcore `df7aee68589424364e74d6cee4def5269721e3e6`。客体编译器分别为 Clang 22.1.8 O2 和 ADI bfin GCC 4.5.3 O2/BF532。
- bcore 默认仍是 OP-1 native 的 CP 起步、后台晋升 `30000/16`、light tier-1、普通 LLVM IR O1/codegen2、loop blocks、steadySkip=2、chain cap=`2^24`。网页的 remote Worker 设置不适用于此次 native 实验。
- 正式 PMU 宿主复用上一轮未启用 profiling 的 Release 静态库。`perf_event_open` 只计执行线程的用户态 cycles、retired instructions、branches、branch misses，`inherit=0`，不计后台编译线程。区间由客体 marker 包围，输入初始化在区间外。
- 3 个收集会话，每个 35 个配置/基准组合，旋转执行顺序。每个观测启动新进程与引擎，依次执行首次、两次预热、三次测量。共 **105 个观测、630 次校验全部通过**。所有 630 个 PMU 区间的 running/enabled 均为 **1.0**，没有事件复用。
- 每个观测先取三次 measured calls 的中位数，再汇总三个会话。倍率先在会话内相除，最后取中位数；因此倍率不必等于表中两列汇总数之比。范围只是三个会话的最小/最大值，不是置信区间。

记录的 host cycles 与 Blackfin 的客体 CYCLES 寄存器不是一个指标。PMU 包含 marker 附近少量宿主指令，但相对数百万轮工作量可忽略；它不衡量整个进程或编译的总 CPU 消耗。未采集 cache misses、uops、端口占用或 stall 分类，因此下文不把机器码推断写成这些事件的实测结论。

## 默认配置每轮实际执行了多少工作

以下均为每次 **C 循环迭代**的宿主计数，不是每条客体指令的计数。

| 基准 | Dyn cycles | bcore cycles | bcore/Dyn cycles 比 | Dyn x86 指令 | bcore x86 指令 | Dyn 分支 | bcore 分支 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 整数混合 | 7.01 | 10.44 | 1.487× | 29.00 | 33.00 | 3.00 | 3.00 |
| 数据依赖分支 | 41.74 | 51.82 | 1.242× | 129.50 | 228.49 | 14.50 | 26.00 |
| 顺序内存 | 11.03 | 12.23 | 1.109× | 59.00 | 73.00 | 5.00 | 7.00 |
| 指针追踪 | 13.92 | 14.52 | 1.036× | 36.00 | 54.03 | 4.00 | 5.01 |

整数项仅多约 14% 的指令，却多约 49% cycles，提示关键依赖链比指令数量更重要。分支项则多约 76% 的指令，其中大量工作可以并行或被准确预测，所以 cycles 的增长没有同比例放大。

**分支项的 branch misses 每轮几乎一致：Dynarmic 0.5000，bcore 0.5007。** bcore 的主要问题是执行更多指令和可预测分支，不能归因成“多了一倍预测失败”。不应只比较 miss rate 百分比，因为两边分支总数不同。

整数 cycles 比的会话范围为 1.476–1.493×，分支为 1.241–1.288×，证据较稳定；顺序内存为 1.009–1.138×；指针追踪为 **0.857–1.059×**，其中一个会话反向。因此此次 PMU 不支持把指针项的小差距描述成稳定的大劣势。后面的 page/raw 对照则在三个会话都稳定。

## 默认热点确实完成了晋升

机器码捕获使用单独的 profiling 构建，不用于上表计时。有效默认捕获目录是 `capture/bcore-default-no-ir-*`：

| 基准 | 主热点 Blackfin PC | CP 编译块数 | 晋升数 | 主热点安装时 marker | loop blocks |
| --- | --- | ---: | ---: | ---: | ---: |
| 整数 | `0x100ec` | 13 | 1 | 0 | 1 |
| 分支 | `0x1017c`，以及调用/返回相关块 | 18 | 6 | 0 | 0 |
| 顺序内存 | `0x101ba` | 16 | 2 | 0 | 2 |
| 指针追踪 | `0x1021e` | 18 | 4 | 0 | 4 |

marker 0 表示首次调用期间。四组均无 CP fallback。顺序内存和指针项的部分初始化块到 marker 6/8 才安装晋升代码，所以不能说“所有代码在预热后都是 LLVM”。默认与直接 LLVM 的 PMU 指令数非常接近，与主热点已晋升相符；指针默认的 54.0286 对直接 LLVM 的 54.0002，也保留了少量额外执行。

这是独立跟踪的代表性证据，不是对每一个未插桩 PMU 进程的晋升时刻证明。捕获事件、块地址和字节保存在 [code-evidence.json](code-evidence.json) 及原始包中。

捕获过程中还发现一个会改变被观测对象的设置：`Core::compile()` 的 CP 条件包含 `!want_ir`。最早同时请求 IR 的“default”捕获实际禁用了 CP，**不能作为默认路径证据**。这些记录完整保留在 `rejected-default-capture-ir-forced-llvm/`，本报告排除它们；有效默认捕获不请求 IR，只有明确的 LLVM-only 捕获才启用 `NATIVE_IR`。

## 整数项：一条指令在循环依赖链上造成明显差距

客体 C 核心是无符号 32 位运算：

```c
x = value * 1664525u + 1013904223u;
y = x ^ (x >> 13);
value = (y << 7) | (y >> 25);  // rotate-left 7
```

ARM 程序用乘加、异或及旋转表达计算。Dynarmic 在热点 `0x101c8` 生成 `imul → lea → shr → xor → rorx ...,25`。`RORX` 不改宿主标志位。

这个客体循环在 ARM 中是 5 条指令，在 Blackfin 中是 10 个标量 packet。ARM 的 MLA、带移位操作数的 EOR 和 ROR 表达更紧凑；Blackfin 将乘/加和左移/右移/OR 分开。这是 ISA 表达能力与客体编译结果的差异，不能直接换算成两倍宿主耗时，也不能全部记为 bfin GCC 的缺陷。

Blackfin GCC 将旋转拆成左移、右移、OR。bcore 的 LLVM lifted IR 仍然包含同一值的两个移位，但优化后右移变成了 `x >> 25`。这个改写在数值上成立：`(x ^ (x >> 13)) >> 25 == x >> 25`。随后 x86 后端选择了双输入 `SHLD`。默认晋升后的热点片段为：

```asm
imul r14d, r10d
lea  ebx, [r14+r9]       ; x
; ……计算加法 overflow / 累积 VS……
mov  r14d, ebx
shr  r14d, 13
xor  r14d, ebx           ; y
; ……保留其他客体寄存器、标志位语义……
shld r14d, ebx, 7        ; (y << 7) | (x >> 25)
cmp  r13d, eax
```

下一轮乘法依赖这条 `SHLD` 的结果。LLVM 的代数简化在这里没有产生最合适的宿主指令形态。

诊断实验在同一份 LLVM-only JIT 机器码中，**只把这条 `SHLD r14d,ebx,7` 改成 `ROL r14d,7` 加一个补位 NOP**；不改地址布局、客体指令、VS 计算、预算检查或循环结构。紧随其后的 CMP 覆盖宿主标志位，因此这两个指令对宿主 flags 的差异在此处不可观察。

| 整数对照 | cycles/轮 | x86 指令/轮 |
| --- | ---: | ---: |
| LLVM O1 原样 | 10.55 | 33.00 |
| 同一机器码，仅替换旋转 | 8.44 | 34.00 |

配对提速 **1.252×**，三个会话为 **1.248–1.257×**，cycles 减少约 20.1%。补位 NOP 让 retired instructions 反而增加，性能仍显著改善，直接说明这里不能只数指令条数。

替换只在 `core.run()` 返回后、首次调用完成与测量前进行；没有后台编译线程；校验精确字节及前置 SHR/XOR 形态，并要求恰好替换一次。另用独立宿主比较 Quick/Full 的最终 **292 字节客体架构状态**，包含 ASTAT 与 CYCLES，原样/替换完全一致。状态字节和检查记录在原始包 `build/rotation-state-check.json`。

这证明该指令选择在本机、本循环中有实质成本，但没有测量 SHLD 的具体微操作延迟或端口分配，也不是对所有程序的等价替换证明。生产实现应在 IR 层保留/恢复 rotate 语义，并检查最终 codegen；诊断用的字节补丁不能作为通用优化发布。单独加 `llvm.fshl` 也需要检查后续 pass 是否再次拆散它。

旋转改完后 bcore 仍约 8.44 cycles，对照 Dynarmic 约 7.01，剩余差距不能继续归到这一条指令。

## 标志位：Blackfin 有更多真实语义需要保留

Blackfin 的数据寄存器 ADD、循环计数递增会更新 ASTAT，特别是 sticky overflow：

```text
overflow = ((a >> 31) XOR (sum >> 31))
         AND ((b >> 31) XOR (sum >> 31))
VS_next = VS_previous OR overflow
```

`lift_visitor.cpp` 的 `emit_flags_arith()` 和 `decode_COMPI2opD_ADD()` 将其展开为移位、XOR、AND、OR。整数热点里可以实际看到这些 x86 指令；VS 保存在宿主寄存器中，并不是每条都写内存。

相同 C 的 ARM 热点大量使用不更新 CPSR 的 MLA/EOR/ROR，只需对循环 SUBS/条件判断保存相应标志。**相同 C 计算不等于两种 ISA 有相同的可观察状态义务。** C 不读取 overflow，并不允许模拟器随意丢弃 Blackfin 的 sticky VS：后续客体代码或异常处理仍可观察它。

可尝试用 LLVM overflow intrinsic 保留“算术结果+overflow”的关系，或在确认 VS 已为 1 且区域内不会清零时做受保护的专用版本。前者需要核对 Blackfin 精确语义；后者需要 guard、异常状态恢复及失效处理。此次没有单独测得“删除标志位计算”的安全收益，不能把剩余差额全算在 flags 上。

## 分支项：客体栈帧被内存语义和块边界放大

分支基准每轮调用 `branch_left` 或 `branch_right`。原始 bfin GCC 在这两个叶函数内都生成 `LINK 0`/`UNLINK`；它们虽然没有局部栈空间，仍要保存/恢复客体返回地址与 FP，并进行客体栈读写。

只为 Blackfin 增加 `-fomit-frame-pointer -momit-leaf-frame-pointer`，保留相同 C、O2、分支与非内联调用，可删除这两条客体 packet/轮。其效果远大于“宿主少执行两条指令”：

| 分支对照 | cycles/轮 | x86 指令/轮 | 分支/轮 |
| --- | ---: | ---: | ---: |
| bcore 直接 LLVM，原始客体 | 52.43 | 231.99 | 26.00 |
| bcore 直接 LLVM，省略叶栈帧 | 44.24 | 179.49 | 20.00 |
| bcore 默认，省略叶栈帧 | 42.98 | 175.99 | 20.00 |
| Dynarmic 默认，原始 ARM 客体 | 41.74 | 129.50 | 14.50 |

固定直接 LLVM 对照的配对提速是 **1.195×**，范围 **1.164–1.198×**，cycles 减少约 16.3%；每轮减少约 **52.5 条宿主指令、6 次分支**。默认 omit 也接近 Dynarmic，但其墙钟时间有一个较噪的会话，因此用直接 LLVM 对照量化收益。

这部分首先说明客体编译选项有影响；同时解释了 bcore 的放大因素：一次 guest stack load/store 会展开为对齐检查、页表读取、缺页/设备慢路径、精确异常状态准备。整个 omit 编译也改变代码布局和少量序言/尾声，所以该对照量化的是“省略叶栈帧的完整结果”，不是每个检查的独立成本。

分支热点还有两个明确的 bcore 结构成本：

1. **跨块时状态物化。** 捕获的 `0x1017c` 会将 AZ、AN、V、VS、AC 等分别存到 CpuState；一次标志更新可能成为多个 32 位 store。下一块重新读取值。普通 LLVM 局部优化不能跨独立 JIT 函数消除这些 store/load。
2. **已知目标仍走 chain table。** `emit_chain_exit()` 即使知道下一 guest PC，仍检查预算、比较槽位 PC、加载函数地址、间接跳转。该热点的实际尾部可见：

```asm
cmp ecx, edx                         ; chain budget
jae leave
cmp dword ptr [rax+0x80b80], 0x10170  ; slot PC tag
jne leave
mov dword ptr [r13+0x15c], ecx       ; chain packets
mov rax, qword ptr [rax+0x80b88]     ; target function
jmp rax
```

Dynarmic 的 `EmitTerminalImpl(LinkBlock)` 为已知目标修补直接 Jg/Jz，`LinkBlockFast` 修补直接 Jmp，返回还可使用 RSB 提示。它也有 cycle/状态保存开销，并非零成本；但已知边不需要反复执行 bcore 这套槽位验证。

该分支基准捕获到 **0 个 loop block**。含调用和多条回边的循环没有落入现有自循环优化，因此跨块状态开销更突出。这里最相关的架构方向是扩大 region/trace 的优化范围，以及针对稳定直接边做可失效的链接。此次没有单独替换 chain table 或 flags lowering，所以不给这两项虚构独立百分比。

native 块 ABI 已是 LLVM 15 的 **SwiftTail + no_callee_saved_registers**，CPU/Memory 参数用 R13/R14；并非旧的 GHC ABI，也不是每块必然走一轮普通 C 调用保存全部寄存器。LLVM ≥19 的 preserve_none 是另一套支持，当前 CP stencil 构建固定在 LLVM 15；不能把简单升级 LLVM 当成已验证修复。

## 内存项：页表在指针依赖链上，比在顺序访问中更贵

指针追踪的下一轮地址取决于本轮读到的值：

```text
index → guest address → page index → load page pointer
      → alignment/null checks → load data[index] → next index
```

捕获的热点 `0x1021e` 有对应的 `shr ...,12`、`mov page,[table+index*8]`、`test page,page`，然后才是 `mov value,[page+offset]`。即使页表和数据都在缓存中，后一次 load 仍依赖前一次 load 的结果。两边都采用页表，因此仅凭这条链的存在不能解释全部相对差距；它更直接揭示 bcore 自身的优化空间。

保持 LLVM O1、相同客体、同一内存大小，只将 bcore 换为 **有界 raw RAM 路径**：

| LLVM 内存对照 | 页表 cycles/轮 | raw cycles/轮 | 配对提速 | 页表→raw x86 指令/轮 |
| --- | ---: | ---: | ---: | ---: |
| 指针追踪 | 14.41 | 8.09 | **1.779×** | 54 → 48 |
| 顺序内存 | 12.14 | 12.02 | 1.020× | 73 → 69 |

指针项提速范围 **1.766–1.824×**，cycles 减少约 43.8%。只少了约 11% 指令，却少了约 44% cycles，与串行地址依赖被缩短相符。raw 对照也改变了地址检查与寄存器分配，不能将整个收益精确归给单条 page-pointer load。

顺序访问能重叠更多工作，移除页表路径只改善约 2%，不能把指针项的 1.78×推广到所有内存工作负载。顺序热点已经复用 load/store 的 page pointer，但仍有对齐/空页检查、store 后 CEC pending 检查、budget 检查，以及 flags 活跃值引起的寄存器压力和 spill。

运行时数据工作集为 32 KiB；没有采集 cache-miss 事件，因此不声称瓶颈是 DRAM 或 cache miss。raw 结果也不是同等设置下“bcore 已超过 Dynarmic”的证明：该对照里 Dynarmic 仍是页表，真实 OP-1 还包含设备与非连续映射。生产方向应是受保护的连续 RAM 快路径、区域范围证明/版本化，并为 MMIO、异常和映射改变保留正确回退。

## LLVM 已经做对了什么，以及当前优化边界

native 热点已经不是“每条 guest 指令立即读写整个 CpuState”。`BBTranslator::expand_loop_syncs()` 对 loop block 建立 shadow state，在必要的 helper、退出与跨块调用处同步；LLVM 可将循环中的客体寄存器提升为 SSA/宿主寄存器。整数热点能直接看到值跨迭代保存在寄存器中。

关闭 loop blocks 的对照能量化这个现有收益：

| 基准 | loop blocks 开 cycles/轮 | 关 cycles/轮 | 关/开的配对比 |
| --- | ---: | ---: | ---: |
| 整数 | 10.55 | 12.86 | 1.218×，范围 0.991–1.473 |
| 分支 | 52.43 | 52.45 | 1.001× |
| 顺序内存 | 12.14 | 19.88 | **1.638×**，范围 1.582–1.652 |
| 指针追踪 | 14.41 | 17.86 | 1.231×，范围 1.088–1.292 |

顺序项的稳定收益证明，**让 LLVM 看见更长的状态存活区间是有效的**。分支项没有匹配到 loop block，所以关闭它几乎没有变化；整数对照波动较大，不应把 1.218×当作精确通用收益。

其他排除性对照：

- LLVM IR O1→O2：分支提速约 1.039×，其他项没有一致收益，不能靠统一提高优化等级消除差距。
- 关闭 steady skipping：宿主指令数基本相同，未显示可推广的收益；顺序项关闭后反而更慢。它不是这组不断改变状态的循环跑得快的原因。
- 关闭 Dynarmic cycle counting：整数/顺序/指针每轮少约 2 条指令，分支少约 6.5 条。分支 cycles 改善约 5.7%，其他项波动，不能把当前差距解释成 Dynarmic 没做周期统计；正式默认本来就开启了它。

## 保留 LLVM 时，应优先改哪一层

| 优先方向 | 本次支持它的证据 | 落地时需要保留的语义 |
| --- | --- | --- |
| 修复 rotate 的识别/晚期规范化 | 同一 JIT 机器码替换，整数 cycles −20% | 中间 guest 寄存器、ASTAT、每个退出点状态；避免优化后又退回 SHLD |
| 客体省略不必要的叶栈帧 | 同 C/O2，分支 cycles −16%，减少 52.5 条 x86/轮 | C ABI、调试/回溯需求；这是客体构建优化 |
| 将自循环 SSA 扩展到多块热 region | 已有 loop blocks 对顺序项提升 1.64×；分支完全未覆盖 | 每条异常边/中断边的状态重建、代码失效、guest 内存副作用 |
| 已知块边直接链接 | 热码仍执行 slot tag/load/indirect jump | 晋升替换、SMC、失效、安全点与预算检查；收益尚未单独测量 |
| flags 语义延迟降低 | 分支中多字段 ASTAT store；循环内仍有 sticky VS 算术 | 标志活跃性、sticky 状态、精确异常；不能按“C 不读”删除 |
| 对连续 RAM 做区域证明与快路径 | 指针 raw 对照稳定提升 1.78× | 对齐、越界、MMIO、映射变化、异常与回退；不能推广顺序项收益 |

架构上应让 guest registers、flags、memory access 的语义先存在于可优化的区域表示中：区域内部用 SSA 值，普通 flags 按使用点求值，sticky flags 显式累计，只有在可观察的退出/helper/异常边才物化 CpuState。然后再降低成 LLVM IR，交给 LLVM 做调度、寄存器分配与机器码生成。已有 loop shadow 机制可以作为扩展基础，重点是覆盖多块控制流和保留语义信息。

因此，**目前证据支持改进 LLVM 前的表示和边界，再修具体的 lowering/codegen；没有证据支持把 LLVM 整体抛弃作为首要方案。** bcore 有可改的架构限制，但也存在 guest 编译选择与一条具体宿主指令这样局部、可验证的原因，不能将所有差额归为某一个“架构原罪”。

## 原始证据与复现

- [summary.json](summary.json)：3 会话中位数、范围和逐会话数值。
- [code-evidence.json](code-evidence.json)：有效默认捕获的热点、晋升事件和块字节索引。
- [raw-evidence.tar.gz](raw-evidence.tar.gz)：原始 PMU 会话、生成宿主、构建清单、guest 汇编、加载后的 x86 字节/反汇编、LLVM IR、最终状态校验，以及明确排除的早期捕获。
- [build.py](build.py)、[measure.py](measure.py)、[analyze.py](analyze.py)、[pmu.h](pmu.h)、[capture.h](capture.h)、[rotate.h](rotate.h)：实际实验工具。`rotate.h` 仅适用于本报告固定输入程序。

先按 [native 构建说明](../../experiments/bcore-comparison/README.md)生成原始 release 宿主与 guest。`build.py` 还使用上一轮 [attribution 构建](../2026-10-31-bcore-attribution/build.py)生成的 `build-bcore-analysis/dist/omit.bin`。只需复跑本次 native 实验时，也可使用本证据包中的精确 omit 客体，其 SHA-256 记录在 `build/build.json`。

```sh
# 在仓库根目录，已有原始 native 构建；结果目录必须尚不存在。
mkdir -p build-native-instruction-analysis/replay build-bcore-analysis/dist
tar -xzf internals/measurements/2026-11-01-native-instruction-analysis/raw-evidence.tar.gz \
  -C build-native-instruction-analysis/replay
cp build-native-instruction-analysis/replay/guest/bfin-omit.bin build-bcore-analysis/dist/omit.bin
python3 internals/measurements/2026-11-01-native-instruction-analysis/build.py
# 完成所有构建后再收集；按本机 P-core 编号调整 affinity，需要可访问 PMU。
taskset -c 8,10,12,14 python3 internals/measurements/2026-11-01-native-instruction-analysis/measure.py
python3 internals/measurements/2026-11-01-native-instruction-analysis/analyze.py
```

只重新统计原始结果时，无需运行基准：将包内 `results/` 解到仓库的 `build-native-instruction-analysis/results/`，再执行 `analyze.py`；它会校验调用次数、checksum 和 PMU 覆盖率，并重新生成 summary。

可选机器码捕获：包内 `build/profile-project/CMakeLists.txt` 配合固定 bcore snapshot、Release、LLVM 15 单独构建 `capture-bcore`，生成源是 `build/bcore.cpp`，包含 `capture.h`。设置 `NATIVE_CAPTURE` 指定目录；**默认配置不要设置 NATIVE_IR**。CLI 参数与普通诊断宿主相同：`guest case_index iterations checksum mode`。原生 Dynarmic 的捕获由诊断宿主调用 `DumpDisassembly()`，结合 `PERF_BUILDID_DIR` 的 perf map 识别 guest PC。捕获构建的时间不用于 PMU 表。

源码定位（均为上述固定修订）：bcore `src/lift_visitor.cpp` 的 flags helpers（约 1395）、alignment/page/memory lowering（1536–1759）、ADD overflow（2014）；`src/bb_translator.cpp` 的 `expand_loop_syncs`（337）、`emit_chain_exit`（715）；`src/core.cpp` 的 `want_ir`/CP 条件（202–229）；`src/block_abi.h`。Dynarmic `src/dynarmic/backend/x64/a32_emit_x64.cpp` 的 `EmitTerminalImpl(LinkBlock)`（1157）。

本次仅新增诊断工具与证据，不修改 bcore 上游、正式 native 库、页面默认配置或发布产物。
