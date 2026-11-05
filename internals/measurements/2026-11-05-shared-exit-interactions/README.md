# 开启共享出口后的优化组合（2026-11-05）

此前单项实验的 arithmetic/memory probe 是单基本块，**没有启用 region 共享出口**。
它们不能直接回答“共享出口已经开启时，其他优化是否值得加回”。本次保持相同 IR 和
JS 驱动，比较出口策略与各项优化的组合。生产 backend 没有新增共享出口开关；所有
实验补丁在生成产物后已恢复，PR 的生产选择保持不变。

## 控制组和组合

基准 backend 为 `bddb494`（RSB 单项），条件优化沿用前次已审阅的三个候选：
固定 carry + live flags 的 i32、相邻 set/get 的 tee、立即数 page-table 访存。
三个出口策略分别为：

- `inline`：当前的原地 flush + return/tail-call。
- `shared`：xhalf-hle 原始 `jit-shared-exit.patch`，普通返回和尾调用均进入公共出口。
  两个额外 i32 locals 传递 result/target，公共出口根据 target 判断是否 tail-call。
- `tail`：前次的收窄实验，仅尾调用共享，普通返回保持原地提交。

每种出口策略分别生成 base/i32/constant/tee，另加 `shared-all`（共享 + 三项优化），
共 13 个源码组合。`build-matrix.py` 按顺序构建、冻结模块、记录源文件和生成器哈希，
finally 恢复原生产文件。三个单项补丁来自
[前次实验](../2026-11-05-conditional-emission/README.md)，完整共享补丁保留于本目录。
组合中的 i32 是收窄版本，未改原无标志算术路径，不是原应用 helper 的逐字移植。

新增 `emission_probe <out> region`：算术/访存放入**两个块的 A32 board 多入口 region**，
第一个块执行 64 次操作链后 LinkBlockFast 到第二个块，第二个块返回宿主。
配置启用 cycle counting、table import、完整 descriptor 入口选择和原有边界守卫。
因此完整共享出口确实被生成，所有策略面对同一 register cache 和控制流。
算术链是 ADD(carry-in=0)、消费其 C 的 SBC、NZCV 写入的依赖链，不能代表所有
carry-only / subtract-only 情形。访存为每步 read32 + add + write32。

另外保留两个 16 块负载，分别检查频繁普通返回和多个真实 tail-call 出口。
两个块的算术/访存 region 本身没有真正执行 tail-call，`tail` 在这里主要提供对照；
它的实际尾调用成本由 `tail-exits` 单独测量。不能把少量出口的计算负载推广为大型
region 的冷编译结果。

`emission_probe` 不传 region 参数时仍生成原模块；生产源码恢复后的默认模块逐字节
匹配前次冻结产物，新 region 模块逐字节匹配 `inline-constant`，见
`restored-identity.json`。已有 benchmark/test 入口没有改变。

## 方法

- 每个比较、每个 tier 15 对独立进程，AB/BA 交替，每轮轮换比较顺序，固定 CPU 2。
- 直接运行冻结 guest 模块，使用相同 JS 驱动；`--liftoff-only` / `--no-liftoff` 作用于
  guest Wasm，没有编译器宿主 Wasm 的布局或 tiering 差异。它们不是生产检测条件。
- 同一个 batch 函数先 warm 1,000,000 次，然后完整保留 9 个 200,000 次样本。
  每进程取样本中位数，每对取耗时比，最后取配对比值中位数；95% bootstrap 区间
  以进程对为单位（10,000 重采样，固定种子）。多项比较未做区间校正。
- 26 个比较、两个 tier，共 1,560 个计时进程。另有 156 个功能检查进程，验证结果、
  NZCV、内存写入、多入口及 tail-call。全部命令和输出保存，无删样、无结果筛选重跑。
- Node 24.19.0 / V8 13.6.233.17、i7-12700、powersave、turbo 开启。采集时未并行
  运行本任务的构建或其他 emulator 测试；同机仍有桌面负载。
- 这是合成 guest 组合诊断，不测 emitter 的宿主开销、真实固件、First Play 或默认
  JIT 验收。不能用它覆盖前次默认门槛 8/12 的失败或直接修改生产选择。

## 结果

以 **shared-base** 为对照，保持完整共享出口开启，只改变其他优化：

| 比较 | Liftoff 耗时变化（95% 区间） | TurboFan 耗时变化（95% 区间） |
| --- | ---: | ---: |
| 加 i32 / flags | +3.39%（+0.76%～+5.96%） | +23.72%（+22.98%～+26.15%） |
| 加 tee / flags | +0.43%（-1.11%～+1.30%） | -0.26%（-1.01%～+1.37%） |
| 加 tee / memory | -0.75%（-3.81%～+0.76%） | +0.40%（-0.20%～+16.34%） |
| 加常量访存 / memory | -68.30%（-69.99%～-66.95%） | +0.30%（-8.52%～+3.11%） |
| 加三项组合 / flags | +3.90%（+2.39%～+5.47%） | +25.28%（+22.36%～+26.61%） |
| 加三项组合 / memory | -68.00%（-71.08%～-65.84%） | +2.64%（-9.46%～+11.21%） |

在这些负载中，共享出口没有消除 i32 的执行代价，tee 也没有可靠的收益；
常量访存在 Liftoff 下仍然明显有益，TurboFan 下未检测到明确方向。
三项全开没有修复算术链的回退。

不能把上表相对 shared-base 的收益当作相对当前 inline-base 的总收益；另做直接比较：

| 比较 | Liftoff 耗时变化（95% 区间） | TurboFan 耗时变化（95% 区间） |
| --- | ---: | ---: |
| shared-all 对 inline-base / flags | +4.84%（+3.33%～+7.00%） | +25.81%（+24.88%～+26.24%） |
| shared-all 对 inline-base / memory | -67.25%（-72.20%～-66.15%） | +2.65%（-3.82%～+7.25%） |

同一批、同一多块 IR 下，i32 的不同出口上下文为：

| 比较 | Liftoff 耗时变化（95% 区间） | TurboFan 耗时变化（95% 区间） |
| --- | ---: | ---: |
| inline 下加 i32 / flags | +3.13%（+1.86%～+4.81%） | +25.40%（+23.44%～+26.70%） |
| shared 下加 i32 / flags | +3.39%（+0.76%～+5.96%） | +23.72%（+22.98%～+26.15%） |
| tail 下加 i32 / flags | +5.25%（+4.37%～+26.02%） | +25.12%（+24.35%～+26.71%） |

因此这次 TurboFan 的约 24%～25% 算术链回退并非共享出口特有：inline 和 tail
也有近似结果。此前单块 probe 的 −3.6% / +5.7% 不是本组的匹配对照。
多块 region 使 GPR/NZCV 缓存在 locals，TurboFan 能消除大量中间 NZCV 写入，
剩余 carry 依赖链的权重变大；不能只把两组百分比之差归因于出口。
这些数值仅适用于此算术 microbenchmark，不是整个 JIT 或应用慢约 25%。

出口策略本身（其余优化全关）还做了独立比较：

| 比较 | Liftoff 耗时变化（95% 区间） | TurboFan 耗时变化（95% 区间） |
| --- | ---: | ---: |
| shared 对 inline / flags | -0.41%（-15.82%～+2.30%） | -0.36%（-1.03%～+1.12%） |
| shared 对 inline / memory | -0.20%（-9.97%～+6.26%） | -0.09%（-4.61%～+5.61%） |
| shared 对 inline / 普通返回密集 | +1.82%（+0.51%～+6.14%） | +4.87%（+3.62%～+9.24%） |
| shared 对 inline / tail 出口 | +3.12%（-5.15%～+6.23%） | +1.75%（+0.28%～+6.43%） |

普通返回密集时，完整共享仍有可观察执行成本；计算和访存为主的两个块 region
则没有检测到稳定出口策略差异。`matrix-summary.json` 同时保存所有 tail-only 对照、
tee 与 i32 的组合比较，不用不同批次的百分比相减来推导组合收益。

## 生成代码解释

`machine-code.json` / `disassembly/` 是独立代码诊断，打印时未采集性能结果。

| shared 策略 / 负载 | Wasm 字节 | Liftoff 指令字节 | TurboFan 指令字节 |
| --- | ---: | ---: | ---: |
| base / flags | 11,703 | 14,532 | 1,876 |
| i32 / flags | 10,933 | 13,840 | 2,068 |
| tee / flags | 11,185 | 14,532 | 1,876 |
| all / flags | 10,415 | 13,840 | 2,068 |
| base / constant-memory | 13,064 | 32,736 | 13,780 |
| constant / constant-memory | 8,712 | 23,540 | 13,780 |
| tee / constant-memory | 12,546 | 32,736 | 13,780 |
| all / constant-memory | 8,706 | 23,540 | 13,780 |

完整共享下，三项组合将算术 Wasm 从 11,703 缩至 10,415（−11.0%），访存从
13,064 缩至 8,706（−33.4%）。但算术的 TurboFan 指令反而从 1,876 增至 2,068。
反汇编中原加法 carry 是 64-bit LEA/ADD 后取高位 SHR；i32 路径使用
`cmp → setc → movzx`，结果继续被下一次 SBC 消费，形成更长的依赖链。
这解释了为什么少写 i64 Wasm 操作仍可能更慢，也说明共享 epilogue 不能修复
算术内核的指令选择。这里未做硬件计数器采集，未把它声称为唯一的微架构原因。

tee 在这些完整共享负载中未减少最终指令字节；三项组合的 flags 指令大小与
shared-i32 相同，memory 与 shared-constant 相同。常量访存消除的计算在 Liftoff
中仍有执行成本，TurboFan 则已经能简化它们。静态指令字节不是 RSS，也不是
执行时间的直接预测。

本次结果支持继续保留常量访存的条件专门化，不支持因为开启共享出口就恢复另两项。
共享出口是否值得用于真实固件冷路径，仍须使用对应固件、region 大小和引擎另行验收；
本次没有改变生产 backend、没有宣称默认门槛已通过。

## 复现

在相同工具链配置下构建 native `emission_probe`。工作区保持 backend 无任务外改动，
生成脚本会暂时重建六个 backend 文件并在 finally 恢复；不要同时运行别的构建/测试。
`bddb494` 为基准，三个优化补丁取自前次已提交的实验目录：

```sh
python3 internals/measurements/2026-11-05-shared-exit-interactions/build-matrix.py
python3 internals/measurements/2026-11-05-shared-exit-interactions/build-matrix.py bundle
cmake --build build-native-region-sharing --target emission_probe -j6
python3 internals/measurements/2026-11-05-shared-exit-interactions/check-matrix.py
python3 internals/measurements/2026-11-05-shared-exit-interactions/collect-matrix.py
python3 internals/measurements/2026-11-05-shared-exit-interactions/disassemble-matrix.py
```

脚本拒绝覆盖生成模块和结果。复测需使用证据副本或新的输出目录；不需要重新生成就
可以直接用归档内冻结模块运行采集。归档包含全部源码组合身份、模块、逐进程输出、
反汇编及构建日志；宿主生成器的时间不在 guest 吞吐统计内。


## 证据归档

完整矩阵逐进程数据、156 个功能检查输出、13 组模块、52 份反汇编和构建配置/日志在
`evidence.tar.gz`。摘要、源码组合身份、补丁和脚本保持直接可读。每个归档文件已经
与原文件逐字节核对；`archive-index.json` 保存其哈希。在本目录解包并验证：

```sh
tar -xzf evidence.tar.gz
sha256sum -c evidence.sha256
```
