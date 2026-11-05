# 条件化 emission 的单项复查（2026-11-05）

基于 PR #9 的 `bddb4943fc3e730412f4423a72ddd8d82be54500`，重新检查 xhalf-hle
`a18df34bb1d450e1fae2f9fdcb038dfda0439f41` 的四个未保留思路。**此前整组验收失败不能
推出四项分别都慢，也不能把约 0.70 的协议吞吐比直接解释为某项优化导致执行慢约 30%。**
旧批次和失败保持原样，见 [前次报告](../2026-11-05-wasm-emission/README.md)。

本次只恢复 **立即数地址的 page-table 访存专门化**。原来的 RSB 单项继续保留。
普通/尾调用共享出口、i32 算术专门化和全局 local.tee 跟踪不进入生产代码。

## 先分开 guest 和编译器宿主

新增 native `emission_probe` 直接调用生产 emitter，产生地址固定的独立 Wasm 模块。
`probe.mjs` 使用同一个 JS 驱动、相同内存、相同 IR 和相同输入执行这些模块，绕开 JIT
编译器宿主的代码布局和宿主 Wasm tiering 干扰。测试程序为本项目自行编写的合成 IR，
没有复制固件或 xhalf 的私有产物。四个候选分别相对同一个基准，未相互叠加：

- `tail-only.patch`：只在 A32 board fallback region 中共享尾调用；普通返回仍原地提交和
  return。用单结果 block 传递目标索引，不为普通返回增加 result/target 变量或出口判断。
  deferred NZCV 在源出口物化，descriptor miss 和 callback 屏障仍走原路径。
- `i32.patch`：仅在需要 carry/NZCV、且 carry-in 为 ADD=0 / SUB=1 时改用 i32；沿用原有
  overflow 和 NZCV packing。无标志算术保持原生成代码，避免重复获取参数和额外 helper。
- `tee.patch`：仅折叠记录过指令边界的相邻 set/get；取得可写 Body 引用后永久禁用折叠，
  防止保留引用随后修改新指令。没有把追踪器的宿主开销算入 guest-only 结果。
- `constant.patch`（原实验见 `constant-experiment.patch`）：无 flat window、地址为立即数时，编译期计算页索引与 containment；
  页内访问保留 live page-pointer load、mask 和 null fallback，跨页直接调用 thunk。
  只为实际使用的路径分配临时 local。最终生产版本仅调整了此 patch 的空白；
  `selected-module-identity.json` 证明重新生成的所有模块逐字节一致。

每个配置 15 对独立进程、AB/BA 交替、固定 CPU 2；分别用 `--liftoff-only` 和
`--no-liftoff`。同一个 batch 函数先运行 1,000,000 次，再保留 9 个 200,000 次样本。
每进程取九次中位数，再对相邻进程耗时比取中位数；95% bootstrap 区间以进程对为单位。
全部 540 个进程和异常样本均保留，没有根据结果补跑挑选。结果、标志、store、多个
region 入口和返回值均有断言。这里的 tier 参数只作用于直接加载的 guest Wasm；
生产代码没有检测 Node、浏览器或 V8 tier。

Node 24.19.0 / V8 13.6.233.17，i7-12700，powersave、turbo 开启。native 生成器为
Release。合成计时期间未并行运行本任务的构建或其他 emulator 测试，同机仍有桌面负载。
`guest-ablation.json` 保存模块、驱动哈希、所有输出、命令、逐对比值及区间。

## 单项结果

下表为耗时变化；负值表示更快。括号为 95% 区间，适用范围仅为对应合成负载。

| 候选 / 负载 | Liftoff | TurboFan | 选择 |
| --- | ---: | ---: | --- |
| 仅共享 tail-call / 16 个可达尾调用出口 | +2.4%（+0.3%～+2.7%） | −0.7%（−1.4%～+0.1%） | 不恢复 |
| 固定 carry、需要标志的 i32 / 算术链 | −3.6%（−4.6%～−3.3%） | +5.7%（+3.6%～+6.5%） | 不恢复 |
| local.tee / 算术链 | +0.2%（−0.9%～+0.6%） | +0.3%（−0.2%～+1.4%） | 未确定执行收益 |
| 常量访存 / 64 次固定地址读写 | −69.7%（−70.3%～−69.4%） | +0.2%（−2.2%～+1.9%） | 按地址和内存模式条件恢复 |

其余控制和 tee 的访存、普通返回结果全部在原始 JSON 中。i32 的无标志控制和常量访存的
动态地址控制分别与基准模块逐字节相同；仍可看到约 0.5% 的进程统计偏移，不能将窄区间
本身当作代码差异的因果证明。这些区间也没有覆盖跨机器、跨引擎或多重比较的不确定性。

`machine-code.json` 和 `disassembly/` 解释了输入缩减为何不一定带来执行收益：

| 负载 | 基准 Wasm | 候选 Wasm | Liftoff 指令字节（前→后） | TurboFan 指令字节（前→后） |
| --- | ---: | ---: | ---: | ---: |
| tail exits | 5,793 | 5,517 | 12,200→10,088 | 7,664→6,448 |
| i32 flags | 11,791 | 11,021 | 15,036→14,324 | 10,720→10,904 |
| tee flags | 11,791 | 11,535 | 15,036→15,036 | 10,720→10,720 |
| constant memory | 9,454 | 5,102 | 25,084→15,840 | 11,096→11,096 |

只共享 tail-call 已去掉普通返回的额外路由，普通返回控制的机器指令字节也完全不变；
但尾调用本身仍增加跨 block 传值和分支，缩小代码没有抵消本负载的执行成本。
i32 的比较和标志依赖在两种编译器下有不同结果；不能按 i64 指令数估算 x86-64 成本。
tee 在这些负载中只缩小 Wasm 输入。常量访存消除了 Liftoff 仍会执行的计算，TurboFan
已经能消除它们，因此其最终指令大小保持一致。
这些是函数静态指令字节，不是 RSS。反汇编进程只用于代码诊断，其中打印的 timing
不参与性能汇总。该算术链不能排除其他 carry-only / subtract-only 或其他架构有收益；
目前没有据此添加架构/tier 开关的依据。

## 完整宿主回归

固定 15 对独立进程，AB/BA 交替、CPU 2。真实 A32 literal 负载通过 frontend/address-space/JIT 执行，
每次运行验证 `r0`、写入内存、精确 1,572,865 ticks 和零 warm guest compilation。
同时保留原默认 12 项协议及门槛，并补充 board call、Thumb 强制边界、board page-table
循环。对照为本次重新构建的 bddb494 + 相同测试工具，候选仅额外加入常量访存；
冻结路径 `/tmp/dynarmic-conditional-{before,after}-20261105`。

| 完整宿主负载 | 配对耗时变化 | 95% 区间 | emitted bytes（前→后） |
| --- | ---: | ---: | ---: |
| literal-0 | +0.75% | -1.28%～+65.09% | 10,382→10,382 |
| literal-1 | +1.25% | -2.06%～+3.03% | 10,495→10,429 |
| literal-2 | +0.61% | -0.08%～+1.75% | 10,431→10,431 |
| literal-1-liftoff | -18.98% | -20.96%～-18.02% | 10,495→10,429 |
| literal-1-turbofan | +0.21% | -1.22%～+1.96% | 10,495→10,429 |
| thumb-boundaries | +3.24% | -1.76%～+5.99% | 14,259→14,259 |
| board-call | -0.39% | -2.33%～+2.54% | 2,714→2,714 |
| board-memory-1 | +29.97% | -1.26%～+73.90% | 12,259→12,125 |

真实 literal page-table 的固定 Liftoff 耗时下降 19.0%，直接 TurboFan 无明确变化；
默认 tiering 下也没有检测到稳定收益，不能把合成负载的 −69.7% 推广为整个应用收益。
三个真实 literal 模式（thunk/page-table/flat）都检查结果和精确 ticks。

**默认正式门槛仍失败：8/12。** 失败项和协议吞吐比分别为 memory-thunk **0.5891**、
a64-memory-thunk **0.7663**、thumb-memory-page-table **0.5768**、
thumb-memory-flat-window **0.8041**。所有 emitted-byte 和 cold-compile 门槛通过。
对应配对耗时变化为 +23.9%、+2.3%、+4.9%、+0.9%，区间均包含 0；不能用该统计量
将正式失败改成通过。board-memory-1 的默认结果也十分分散，见表。
这些结果不足以宣称默认无回退，候选需要进一步应用验证。

对四个正式失败项及不确定的 board-memory-1 另做固定 tier 诊断，每项每 tier 10 对。
这是针对本批次未解决问题的后续诊断，没有重跑默认门槛，也没有覆盖其失败。
结果见 `host-tier-summary.json`。完整宿主的 tier 参数同时影响宿主和 guest Wasm，
不能仅凭该组诊断拆分两者的影响；前面的独立 guest probe 才排除了宿主 Wasm 差异。

| 诊断项 | Liftoff 耗时变化（95% 区间） | TurboFan 耗时变化（95% 区间） |
| --- | ---: | ---: |
| memory-thunk | +0.08%（-0.24%～+1.55%） | +0.73%（-2.01%～+2.41%） |
| a64-memory-thunk | +0.84%（-1.55%～+3.30%） | -2.56%（-9.58%～+0.75%） |
| thumb-memory-page-table | -1.40%（-2.20%～+4.88%） | -0.06%（-0.88%～+1.75%） |
| thumb-memory-flat-window | +0.98%（-3.20%～+5.97%） | -1.20%（-13.61%～+5.99%） |
| board-memory-1 | +0.44%（-0.15%～+1.31%） | +2.22%（+0.19%～+3.77%） |

四个默认失败项在固定 tier 后均未检测到稳定方向，但部分区间仍较宽。这支持默认结果受
tiering 影响，不能据此证明默认路径绝无回退。另一个 board-memory-1 在 TurboFan 下
仍有 **+2.2%（+0.2%～+3.8%）** 的小幅成本，本候选保留此代价；不能宣称全面更快。
正式默认门槛仍为失败。

## 功能和身份

- 最终常量访存版本：native CTest 24/24、Wasm CTest 11/11、corpus 6/6、Chromium Worker 33 项。
- A32 常量访存 192 个 byte-reference cases：1/2/4/8-byte、跨页非连续映射、live remap、
  pointer tags、absolute-offset、null、observers、protection、缺页和异常。
- 独立 i32 实验通过 1,201,020 次整数 oracle；该候选没有进入最终生产代码。
- 最终生产变更仅 `emit_wasm_memory.cpp`；新增工具及 literal benchmark 不改变已有负载。
  `docs/Embedding.md` 和 `emit_wasm.cpp` 中任务开始前已有的 pthread 改动保持原样、不提交。
- 基准/候选源码和冻结宿主哈希在 `*-source.json` / `*-artifacts.json`。两边均 Emscripten
  6.0.13 Release，profiling / region diagnostics 关闭；保留完整 CMake 和编译日志。
  测量使用非 pthread 分支，原有 pthread 修改不会影响其生成结果。
- 本次没有运行真实固件、首次播放、Firefox 或跨架构性能验收。前次 RSB 相对 main 的
  默认资格问题仍属于历史批次，不能用本次相对 bddb494 的结果覆盖。

## 复现

在基准源码下构建 native `emission_probe`，运行 `emission_probe <dir>`，再分别单独应用
四个 patch、重建并导出。四个目录和 baseline 放在 `modules/` 中。`constant-experiment.patch` 保留原始空白布局，`constant.patch` 为整理后的生产补丁。运行：

```sh
python3 internals/measurements/2026-11-05-conditional-emission/collect.py
python3 internals/measurements/2026-11-05-conditional-emission/disassemble.py
```

脚本拒绝覆盖已有 JSON；需要使用证据副本或新的输出目录。完整宿主先在相同测试源码下
构建基准，再仅应用生产 memory patch 构建候选，用前次 `freeze.py` 冻结两组。串行运行：

```sh
taskset -c 2 node internals/measurements/2026-11-05-conditional-emission/collect-host.mjs \
  /tmp/dynarmic-conditional-before-20261105/tests \
  /tmp/dynarmic-conditional-after-20261105/tests /tmp/conditional-host.json
```

计时前停止本任务的构建、测试和浏览器检查。不要用诊断 tier 参数改写默认协议。

## 原始证据归档

完整逐进程报告、生成模块、反汇编、CMake 配置、构建和测试日志在 `evidence.tar.gz`。
摘要 JSON、候选补丁、身份哈希和采集脚本保持直接可读。归档内每个文件已经与原文件
逐字节核对；`archive-index.json` 记录其 SHA-256。在本目录解包后验证全部文件：

```sh
tar -xzf evidence.tar.gz
sha256sum -c evidence.sha256
```

`collector-syntax-error.log` / `tier-preflight-error.log` 是开始采集之前的工具错误，未产生
计时样本；实际固定轮数采集均完成。冻结的完整宿主仍由 `*-artifacts.json` 标识并保留
在上述 `/tmp` 目录；归档包含其完整输出和配置，未重复打包大型编译器宿主二进制。
