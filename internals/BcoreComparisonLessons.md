# bcore 对比：保留的经验与结论

截至 2026-11-02，bcore 相关工作仅保留为内部研究。正式 examples、浏览器入口和 Pages CI 不接入 bcore。实验源码、配置及验证工具已保存为[可在独立 checkout 恢复的快照](experiments/bcore-comparison/README.md)，各次结果仍保留在原测量目录。

## 已测得的结果及适用范围

倍率均表示 bcore 耗时 / Dynarmic 耗时；大于 1 表示 Dynarmic 更快。每行是自己的配对实验，不能跨行用毫秒数相除，也不能将它们合并为一个“模拟器性能分数”。

| 实验 | 关键结果 | 解释边界 |
| --- | --- | --- |
| [C/Wasm，直接 LLVM O2](measurements/2026-10-31-bcore-c-bench-performance/README.md) | 四项整数/内存等权 1.686× | LLVM-only、raw RAM；不是 OP-1 网页默认 |
| [C/Wasm，网页默认](measurements/2026-10-31-bcore-web-defaults-performance/README.md) | 四项整数/内存等权 5.782× | Quick 固定工作量较短，多数热点在晋升安装前结束 |
| [C/native，native 默认](measurements/2026-11-01-native-c-bench-performance/README.md) | 四项整数/内存等权 1.206× | Extended 工作量，x86-64 native JIT；与网页数据不是同一运行环境 |
| [QuickJS/Wasm，网页默认](measurements/2026-11-02-quickjs-bcore-wasm/README.md) | 八套基准等权 2.194×，三轮 2.192–2.197× | 既有十个 JS case、60 个观测全部通过；ARM/Blackfin 完整路径 |

QuickJS 对比中双方使用同一 QuickJS 2025-09-13 解释器源码、O2、switch dispatch、无 LTO、256 MiB guest RAM、192 MiB QuickJS 分配上限。最终每项安装 167–892 个晋升块，零晋升失败。ARM GCC 10.3/newlib 3.3/VFP 与 bfin GCC 4.5/newlib 1.19/软件 binary64 的差异仍包含在结果里。结束时的安装数不能代替测量开始时的 tier，也不能代替各 tier 的执行时间占比。

## 默认配置必须沿启动流程核对

不要只看 `op1_start()`：它最初关闭晋升，网页 Worker 随后改为 `100000/16` 并附加编译 Worker。完整[网页审计](experiments/bcore-comparison/OP1-CONFIG.md)还包括 cold threshold 10、batch 32/age 65536、8 ms 时间片、MessageChannel 让出、IR O1/codegen2、light tier-1 关闭、LLVM 按需加载和 split module。

native 默认不同：后台阈值 `30000/16`、light tier-1 开启、steady skipping=2。native CP 使用本次固定版本支持的 LLVM 15；Wasm 构建使用 Emscripten 4.0.9/Wasm LLVM 23.1.2。两个运行环境的配置和结论应分别记录。

**`set_copy_patch(true)` 不保证实际走 CP。** FlatMemory 的 raw inline 模式会使 CP 不适用；复现网页路径需要 page-table RAM。研究中用独立诊断观察 CP 模块/晋升事件确认了实际路径，正式测量保留未插桩 Release 宿主。

另一个观察者效应：bcore `Core::compile()` 的 CP 条件包含 `!want_ir`。请求 IR dump 会改变默认编译路径。默认模式的机器码捕获必须关闭 IR 请求；需要 IR 时单独运行明确标注的 LLVM-only 对照。最初受此影响的捕获保存在排除目录，没有混入默认结果。

## QuickJS 移植的具体经验

- 复用既有 JS source、adapter、runner 和分配跟踪，只替换 guest 的计时/输出/退出桥接。对解释器源码做与 ARM 一致的 UTC 适配，并校验完整源码哈希。
- bfin GCC 4.5 没有 C11 `max_align_t`，用符合 ABI 的 union 保持 8 字节分配头；该工具链没有 `fenv.h`，而固定 QuickJS 源码只 include、不使用任何 fenv API，因此提供空兼容头即可。不能将真正使用的函数静默 stub 掉。
- 使用 `-nostdlib` 后显式链接 `-lm -lc -lgcc`。仅 `-nostartfiles` 仍会引入工具链的 BF532 内存脚本，把大 QuickJS 镜像放入过小的 L1 区域。startup 汇编不要接收未由 `__ASSEMBLER__` 保护的 C typedef。
- 时钟来自宿主 monotonic clock，微秒低 32 位在 guest 中扩展为 64 位；不能用模拟器退休 packet/cycle 数代替真实耗时。
- **扩大 guest RAM 时重新审计全部 MMIO 地址。** C lab 原 marker 在 `0x01000000`；扩为 256 MiB 后该地址成为 RAM。QuickJS 适配必须移除旧拦截，将 clock/stdout/exit 放到 `0x10000000/04/08`。首次不完整收集因此被整体保留并排除，修正后从头运行。
- 先验证基础 JS、双精度、正则、时钟和 20 MiB 跨页读写，再跑已有 Full LCG/sieve/pi：输出应为 `428440788 / 6542 / 3141587`。之后运行原版官方十项，Splay 保留 8,000 节点和 teardown 验证。
- 记录编译器、libc/libm/libgcc、guest、宿主 Wasm、split 模块、Worker、JS source/adapter 的哈希。空兼容头、allocator header 和客体地址布局也属于构建证据。

## 测量时保留什么，避免推断什么

正式对比应串行执行，轮换 case 顺序和路径顺序，每次新 Worker/引擎，同一配对位于同一浏览器进程。固定亲和性和工作量，记录机器共享/未锁频条件，测量期间不再构建其他候选。

C lab 的首次＋两次预热＋三次测量，与 QuickJS 官方的一秒预热、一秒批次、至少 32 次调用，是不同协议。保留原始协议，不因一条路径更慢而缩小工作量。QuickJS 比较 `elapsed / runs`，不能直接比较自适应迭代后的 Worker 总时长；加载、首次编译和总运行时间另列。Crypto/EarleyBoyer 各有两个子项，八套等权与十子项等权是不同汇总。

先在同一轮内计算倍率，再对轮次取中位数。内层调用不是独立实验，三轮最小/最大值不是置信区间。失败、超时和受错误配置影响的观测必须保留，重新开始时说明原因。RegExp 和 NavierStokes 上游没有独立数值断言，不能把“完成”写成独立数值校验。

native 的 PMU 诊断只计执行线程用户态，`inherit=0`，不包含后台编译线程；每个区间正确差分 enabled/running 时间。该实验全部区间无事件复用，但未采集 cache miss/uops/端口占用，不能把源码推断写成这些硬件事件的实测归因。

## 从架构和机器码得到的经验

完整证据见 [native 指令级分析](measurements/2026-11-01-native-instruction-analysis/README.md)与 [Wasm 归因](measurements/2026-10-31-bcore-attribution/README.md)。

| 观察/对照 | 已验证的含义 | 不应外推为 |
| --- | --- | --- |
| native 整数 `SHLD → ROL`，cycles 约 −20% | 同一热点关键依赖链上的指令选择很重要；补位 NOP 甚至增加了一条退休指令。Quick/Full 最终 292 字节客体架构状态一致 | 可直接发布的通用机器码补丁，或所有 Wasm 热点都有同一问题 |
| bfin 省略叶栈帧，native 分支 cycles 约 −16% | 两条 guest LINK/UNLINK 可被页表、异常与状态处理放大为约 52.5 条宿主指令/轮 | 无需重编固件即可获得的 bcore 优化 |
| native 指针 raw RAM 对照提升约 1.78×，顺序访问约 1.02× | 串行地址依赖和可重叠访问的收益不同 | 把 1.78×推广到全部访存或忽略 MMIO/映射语义 |
| native 分支每轮预测失败均约 0.5 次 | bcore 多出的成本主要是更多工作，不能用 miss rate 百分比直接判断 | “bcore 多一倍分支，所以多一倍预测失败” |
| 关闭 loop blocks，native 顺序项慢约 1.64× | 现有 loop shadow/SSA 已能保留跨迭代寄存器；扩大优化范围值得研究 | 所有 native 热点仍逐指令读写整个 CpuState |

Blackfin sticky VS 是可观察架构状态。C 不读取 overflow 不等于模拟器能删除它；跨块 flags 活跃性、异常恢复、helper 副作用和预算退出都需要显式表示。分支多块循环未覆盖现有自循环优化，仍保存状态并查 chain table；已知边直接链接和热 region 是方向，尚无本实验中单独实现后的收益数字。

保留 LLVM 时，优先研究 guest registers/flags/memory 语义在进入通用内存 lowering 前的区域表示，再验证最终代码生成。仅 O1→O2 没有普遍收益；这些实验不能证明整体抛弃 LLVM 更好。

同时必须看启动和资源成本。[后续优化路线记录](measurements/2026-11-02-bcore-optimization-roadmap/README.md)引用的 t0-gate 实验显示：在线通用 Reader/local 化改善微基准，却让 OP-1 启动回退。该记录区分了原日志、重新审计结果和未证实假说；不能将 CP-only 微基准收益套到正常后台 LLVM 的 QuickJS。

下一次实验应先量化 QuickJS 测量窗口里 CP、LLVM、分发、guest allocator/libc 和编译调度的占比。构建期 stencil 语义 metadata、廉价 cold/CP 路径和已有后台 LLVM 中的小型热 region 可作为候选；同时验证 OP-1 启动、持续运行、内存和响应延迟，避免用一个热循环分数抵消实际应用的回退。
