# Review a18df34 与性能回归（2026-11-05）

审阅 `~/sandbox/xhalf/xhalf-re/xhalf-hle` 的
`a18df34bb1d450e1fae2f9fdcb038dfda0439f41`。五项优化在原应用的调用契约下语义基本合理，
但当前仓库的测量不支持整组移植。最终只保留 **board 模式下省略无用 RSB 写入及内部返回 pop**；
共享出口、i32 专门化、local.tee 和常量访存专门化均已回退。没有修改原应用仓库。

保留项的 board call 配对耗时下降 **12.0%**（95% 区间下降 **5.9%–23.6%**），生成模块
减少 **8.1%**。默认套件 **10/12** 通过，`memory-thunk` / `thumb-memory-page-table`
的协议吞吐比为 **0.7986 / 0.7100**，未过门槛。因此当前只是有 board 收益的候选，
**没有取得默认配置无回退的性能资格**。完整数值见 [results.md](results.md) / [summary.json](summary.json)。

## Review 与选择

| 改动 | 审阅结论 | 最终处理 |
| --- | --- | --- |
| 共享 region 出口 | 统一提交寄存器和 clock 的思路成立；deferred NZCV 必须在原出口物化，callback 屏障仍需独立保留，descriptor miss 必须在 cache load 前退出 | 本轮未保留 |
| board RSB | board 返回始终使用完整 descriptor 分派及 budget/halt/hook guards，不读取 speculative RSB；该 policy 在 JIT 构造时选择，不能在同一实例中切换为普通 RSB 路径 | 保留 |
| 固定 carry 的 i32 加减 | ADD cin=0 的 C 为 result<a，SUB cin=1 的 C 为 a>=b；动态 ADC/SBC 及同时消费 C/V/NZCV 必须分别覆盖 | 本轮未保留 |
| local.tee | 相邻完整 set/get 可合并，不能搜索原始字节模式。原 patch 的可写 Body() 只清空一次记录，无法防止保留的引用随后改写新指令；生产 emitter 没有这种调用，所以是防御性缺口 | 候选中增强过保护；优化整体未保留 |
| 常量访存 | 可折叠页索引和 containment，不能缓存 live page pointer；mask、absolute offset、MMIO、权限和跨页 fallback 都必须保留 | 本轮未保留 |

保留的改动只影响两个 backend 文件。PushRSB IR 仍参与 region continuation 发现，
没有删掉 LR 更新、guest 指令计数、返回 descriptor 检查、时间/中断/钩子守卫或 callback
同步。A32/A64 board 的直接、间接、内部、外部返回继续使用原来的分派路径；普通模式
保留原 RSB 推测逻辑。没有修改 region limits、默认入口共享开关或 module packing。

## 为什么没有保留整组优化

整组候选确实减小了生成代码：A32 共享负载约少 16.7%–29.2%，A64 约少 4.1%–7.1%。
但最后的固定 CPU 2 试验中，普通 Thumb page-table 的协议吞吐比为 **0.7017**，未过 0.95
门槛；共享 Thumb 强制边界的配对耗时增加 **9.4%**，95% bootstrap 区间 **+3.7%～+17.1%**。
ARM 共享 flat-memory 同时快了约 10.7%。这些是收益与代价并存，不足以认定整组普遍更快。
原始比较和完整表格保留在 [full-candidate](full-candidate)。

中间还尝试了将 peephole 放在 .cpp、恢复普通路径的内联 local 访问，以及限定 board policy。
所有旧样本及失败都保留于 `initial/`、`unscoped-out-of-line/`、`partly-scoped/`、
`scoped-dual/`、`scoped-out-of-line/` 和 `full-candidate/`；没有混入保留项的结果。
这不是每项独立性能影响的消融证明，不能把整组回退都归因于某一个补丁。

有些短负载受调度、频率、布局或引擎 tiering 影响明显：早期双核 affinity `2,4` 中，
ordinary flat-memory 出现约 11% 差异，但反汇编只有宿主地址重定位 +592 bytes；
CPU 2 / CPU 4 各 30 对定向检查又得到 1.0066 / 0.9985 的配对时间比。
证据在 `default-code-check/`、`flat-cpu2.json`、`flat-cpu4.json`。因此不能据此宣称唯一根因，
也不能只选有利样本。最终的 RSB 单项采用重新构建的匹配控制组和固定单核采集。

原 commit 的约 10% First Play 和 40.8% 编译输入缩减，来自特定固件、16-block/512-IR
及三个 Chromium pairs。本仓库仍为 64-block/2000-IR，不能照搬该数字，也没有把 emitted
Wasm bytes 当作 resident native-code / RSS。

## 保留项的实验身份

基准为 `0939fae` 加任务开始时已有的 pthread / 文档改动，见 [pre-existing.patch](pre-existing.patch)。
先把全部 backend 恢复到 [baseline-source.json](baseline-source.json) 的文件哈希，
只扩展 call benchmark 的可选 `board` 模式，再构建控制组；然后只加入两个 RSB 改动。
库源码身份在 `rsb-baseline-source.json` / `rsb-candidate-source.json`。

冻结目录为 `/tmp/dynarmic-rsb-before-20261105` / `/tmp/dynarmic-rsb-after-20261105`，
产物 SHA256 在 `rsb-baseline-artifacts.json` / `rsb-candidate-artifacts.json`。
`jit_callbench.js board` 选择 cycle counting、board boundary 和入口共享；省略参数仍执行
原普通模式。guest words 和 RunOnce 计时函数与原版本一致，见 `rsb-workload-identity.json`。
benchmark 的 native 版本通过 upstream headers 的语法检查；条件编译没有改变 Wasm 的
预处理结果，且重新构建后产物逐字节一致，见 `callbench-preprocess-identity.json`。

两边均为 Release Emscripten 6.0.13、Node 24.19.0、默认 V8 参数，profiling / region
诊断关闭，固定 CPU 2，powersave、turbo 开启。普通套件每项 25 对新进程，轮换 case 顺序；
board call 每项 25 对，按轮交替实现顺序。每进程保留 cold 和全部八次 warm，warm 无新增编译。
八次后续运行包含 tiering，未声称稳态认证。计时期间没有运行本任务的构建或其他 emulator
测试；同机仍有桌面任务。这是本地回归检查，不是跨 CPU、跨浏览器的性能保证。

独立代码检查 `rsb-code-check/comparison.json` 证明普通 flat-memory 生成模块逐字节一致，
包括地址；board call 冷态均编译一个模块，bytes **2953→2714（−8.1%）**。
最终的耗时比较另列在结果文件中。

## 功能验证

- 最终 RSB 单项：原生 CTest **24/24**，Wasm CTest **11/11**。
- Chromium Worker **33 项**、Firefox Worker smoke、checked-in fuzz corpus **6/6**。
- 新增/保留 192 个 A32 常量地址字节 oracle，覆盖 1/2/4/8-byte、非连续跨页、live remap、
  pointer tags、absolute-offset、null / observer / protection 和缺页异常。
- 扩展整数 oracle：固定和动态 carry、同时消费 C/V/NZCV，**1,201,020 次通过**。
- A32 预算退出与逐指令执行比较全部 16 个寄存器和 CPSR；嵌套 BL/BLX 验证 LR、栈和
  普通 RSB；A32/A64 board canary 验证 RSB top、descriptor 和 index 全部不被写入。
- 原有 region corpus 仍逐字节匹配，没有保留候选的 fixture 更新。

对应 `rsb-native-tests.log`、`rsb-wasm-tests.log`、`rsb-chromium-worker.json`、
`rsb-firefox-worker.json`、`rsb-fuzz-corpus.log`。最初整组候选还通过完整严格回归：
21 个随机 campaign，24,300 个 oracle-accepted cases、22,459 次实际比较、1,841 个
unsupported、零 mismatch，各原有比例门槛通过；见 `regression.log` / `fuzz-summary.json`。
这次完整 gate 不是最终单项新跑的一轮；最终单项重新跑了上述 CTest、corpus、双浏览器。
未运行外部 CoreMark 或 xhalf 私有固件，浏览器检查只作为兼容性证据。

## 原始证据

完整的原始报告、逐进程输出、编译与测试日志、失败批次和反汇编在
[evidence.tar.gz](evidence.tar.gz)，小结、保留项 patch 及源码/产物身份仍直接可读。
在本目录执行 `tar -xzf evidence.tar.gz` 恢复原路径，再运行
`sha256sum -c evidence.sha256` 检查完整性。`rsb-only.patch` 只含 RSB 修改；
`candidate.patch` 以仓库 HEAD 为基底，包含测试、文档以及任务开始时已有的改动。
比较器本轮的退出码 1 表示已保存数据但门槛未通过，仍可独立运行 board collector。

## 复现

先构建并冻结匹配控制组，再应用 RSB 单项并构建候选。`freeze.py` 复制完整 host 及 CMake
配置身份；保持两边 benchmark 源码相同，输出路径必须不存在。采集应串行执行：

```sh
taskset -c 2 node tests/bench/run.mjs /tmp/dynarmic-rsb-before-20261105/tests \
  --runs 5 --output /tmp/rsb-before-template.json
taskset -c 2 node tests/bench/run.mjs /tmp/dynarmic-rsb-after-20261105/tests \
  --runs 5 --output /tmp/rsb-after-template.json
taskset -c 2 node tests/bench/paired_run.mjs fixed \
  /tmp/dynarmic-rsb-before-20261105/tests /tmp/dynarmic-rsb-after-20261105/tests \
  /tmp/rsb-before-template.json /tmp/rsb-after-template.json /tmp/rsb-paired --runs 25
taskset -c 2 node internals/measurements/2026-11-05-wasm-emission/collect-rsb.mjs \
  /tmp/dynarmic-rsb-before-20261105/tests /tmp/dynarmic-rsb-after-20261105/tests \
  /tmp/rsb-calls.json 25
```
