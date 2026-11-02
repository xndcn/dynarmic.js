# 根据 t0-gate 制定 bcore Wasm 的下一步优化路线

建议保留 LLVM 后端，以 **便宜的冷路径、精简的 CP、只对高收益热点做后台区域优化** 为主线。优先拆出能减少 OP-1 启动工作量的 LB-tail 和 piece 打包；暂不恢复默认全块 local 化，也不再靠增加一个同步 warm tier 寻找阈值。

这是对现有记录和补丁的只读分析，没有恢复分支、编译候选、运行新的性能实验或修改 OP-1。`audit.py` 重新检查了留存 lab 的 **96 个观测、576 个 checksum**，并重算启动日志。结果和输入哈希见 [audit.json](audit.json)。

## 现有结果证明了什么

来源：[t0-gate 原始说明](/home/xiong/.local/share/op1-perf-keep/t0-gate/README.md)、其 `lab/results/`、`node-boot.log`、`chrome.log`、`chrome-w.log`、`sweep3.log`。

| 实验 | 原配置 | 候选 | 解释 |
| --- | ---: | ---: | --- |
| lab chrome-s4 整数，ms | CP 45.2 | locals/区域 11.9；Dynarmic 6.9 | CP 热执行确实能大幅改善 |
| lab chrome-s4 分支，ms | CP 46.9 | locals/区域 18.0；Dynarmic 11.4 | 跨块区域有价值 |
| lab chrome-s4 顺序内存，ms | CP 94.2 | locals/区域 48.5；Dynarmic 13.9 | 只做 local 化还远不够 |
| lab chrome-s4 指针，ms | CP 38.2 | locals/区域 22.2；Dynarmic 7.0 | 内存/代码形态仍有大差距 |
| Node OP-1 启动，4 次中位数 | 6.305 s | 14.515 s | 全套路线约慢 2.30× |
| Chrome 149 OP-1 重访，4 次中位数 | 7.55 s | 16.40 s | 约慢 2.17×，远超计时分辨率 |
| Chrome 149 warm h4096 重访 | 7.55 s | 8.30 s | 仍回退约 10% |
| Chrome 149 warm h16384 重访 | 7.55 s | 8.05 s | 仍回退约 7%，计时较粗 |
| Node loop-only l4096，2 次中位数 | 6.39 s | 6.90 s | 只选择循环仍未回本 |
| Node loop-only l16384，2 次中位数 | 6.39 s | 6.915 s | 加大阈值没有解决根因 |

lab 前四行来自同一组 `chrome-s4` 三会话数据，避免把不同候选的最好数字拼成一个分数。最终计数器候选 `cq-cnt`/`cq-cnt2` 的摘要为 11.9/18.0/46.8/20.9 ms，但它们只测候选，没有同次 ARM/base 配对。两组共享相同 diag.wasm 哈希。`chrome-s5` 与 `cq-s5` 则是另一份哈希，不能仅按目录名字把它们当成最终四个提交的同一产物。

启动数据还有三点必须分清：

1. **`copy_patch_promote=false` 关闭的是 CP 的字段 local 化，不是后台 LLVM 晋升。** `sweep2.sh` 的 off 组仍传入 `--tier 100000,16 --compile worker`。off 恢复基线，不意味着应该禁用 LLVM。
2. Chrome h1000/h4096/h16384 的原始日志与 loop-only l1000/l4096/l16384 不能混为一组。留存 `sweep3.log` 只有 loop-only 的 Node 结果，不能声称已独立复算 loop-only 的 Chrome 分数。
3. Chrome 每 0.5 s 上报一次 stats；其数据是点击 Start 到首次上报 guestS≥2.88 的时间，包含启动阶段。大回退可信，小于一个采样间隔的收益需要更细的计时重测。首次访问中另有 405.5/278.7 s 异常值；本表明确只统计 visit 1/2，原值仍在 audit 中。

## 失败的是优化的成本和时机，不能由此否定寄存器 SSA

这条路线同时改变了执行代码和生成它的方式，至少增加四类成本：

- 每个块放置 stencil 后再解释 Wasm 字节码、识别 CpuState 访问、生成修改和 branch hints。第一个提交的 Node 启动就从约 6.3 s 增至 10.1 s。
- 区域发现反复 scan/place/Reader，失败的尝试也付成本。记录将 `FieldPromotion::exits` 归到约 5.6 s；先用缓存的出口定形状后有所改善，但仍未恢复基线。
- local 化和区域复制增加字段、路径及模块生命周期成本。Node 启动 RSS 中位数由约 873 MiB 增至 1310 MiB，不能只看最后热循环少了多少条指令。
- 新函数再次经过浏览器验证、实例化、Liftoff 和可能的 TurboFan 优化。对新字节/函数版本，不能假定旧函数的执行反馈与已优化机器码自动继承。

`FieldPromotion::apply()` 还有一个具体可改点：先取 `used = read | written`，为所有 used 字段分配 local，并在函数入口加载它们。它覆盖整个函数的可达路径，而不是只覆盖热循环真正活跃的字段。一个只在冷退出/helper 路径使用的字段，也可能扩大入口加载、循环 phi 和寄存器压力。当前补丁又在运行时用 `State{28 个 Value + vector}` 分析这些信息，既增加翻译成本，也不能保证最终 V8 不 spill。

因此应把“减少客体状态往返”保留为目标，把“每个 CP 块在线通用反编译再重写”替换成更便宜、保留语义的实现。

## 优先级一：从冷路径移走无用工作

### 独立实现受保护的 scalar LB-tail 省略

[web-cold6 的相关原型记录](/home/xiong/.local/share/op1-perf-keep/web-cold6/notes.md) 给出：

- LB 检查和慢路径占采集的 tier-0 **静态字节数约 40.9%**，不是 40.9% CPU 时间。
- 受入口 guard 保护地省略不可能命中的 LB 尾部后，Node 6.29→5.42 s，生成字节 41.3→30.5 MB。
- Chrome 重访从约 7.3–7.9 s 改善到 6.3–6.8 s。

这个方向同时减少热执行指令和浏览器需要验证/编译的字节，是比全块 local 化更好的第一步。

生产实现建议在 stencilgen 阶段生成 `careful` / `no-LB-tail` 等有明确语义的变体或切片元数据；运行时沿用已有 scan、hint 和入口 guard 选择。不要直接发布原型的 byte-pattern `strip_lb()`，也不要依赖通用 Reader 才能识别一段已知的尾部。

适用条件必须显式包含：本包不是实际 loop bottom；此前没有修改硬件循环状态；memory callback 的寄存器影响满足约定；guard miss 回退 careful；异常、terminator 和 parallel packet 维持各自语义。在 Wasm 使用点隔离这个优化，避免再次改变 native 的 guard/重编行为。

此方向有 OP-1 原型收益证据；**它独立于 Reader 后对 QuickJS/现有 CP 的收益仍须实测**。

### 将共享 pieces 做成通用分组，减少零散模块

同一份 notes 中，在 no-LB 原型上合并启动使用的 13,058 个 piece 函数后：模块数 5,836→709，Node 5.40→4.95 s，RSS 少约 95 MiB；Chrome 133 重访约 6.3→5.8 s。相对最初 7.8 s 的约 26% 改善是两个原型的组合收益，不能归给 piece 打包单项，也不能与前一项相加。

原型列表来自一次固件启动轨迹，不能把这个列表当成通用解法。建议构建期按指令家族/常用度产生有上限的共享包，未覆盖 piece 保留按需路径；包的内容和地址 ABI 使用版本键。在首次访问、重访和其他固件/工作负载上比较包下载、验证、首次调用和峰值内存，避免把数千小模块变成一个同样昂贵的同步大模块。

更少的 module 不等于所有函数已预编译。浏览器首次调用的 Liftoff 成本需单独核对，不能仅以 `WebAssembly.compile()` 完成作为预热完成。

## 优先级二：让语义信息在构建期留下来

LLVM stencilgen 已经知道寄存器字段、helper 和控制流。建议输出紧凑的 side table：

- 客体寄存器/flags 的读写集合、位宽、读写顺序和依赖参数洞的字段编号；
- helper 的读集合、写集合、是否可能异常、是否能改变循环/内存映射；
- fallthrough/branch/call/return 的出口描述及安全回退；
- hardware-loop 尾部、安全点和状态恢复需要哪些值。

运行时以参数洞绑定、位集运算和模板拼接为主，常见路径不再重新解释全部 Wasm 指令。间接字段地址、部分宽度访问、别名及未知 helper 必须保守处理，不能为省成本猜测副作用。静态 metadata 与 stencil 格式/生成器哈希绑定。

先只在廉价可识别的块内做有限优化：复用相邻指令的寄存器值，删除必然被覆盖且没有可观察边界的写回；只加载 live-in、只维持必要的 loop-carried 值。冷异常路径通过精确状态快照物化，而非把所有控制字段都拖过回边。是否改善 Liftoff 和 TurboFan 要分别验证，locals 本身不是寄存器分配成功的保证。

现有 ASTAT 删除消融对整数约 0%、分支约 8%，且删除状态并非正确实现；它只能用于判断该候选的瓶颈。不要把“全面延迟 flags”排在已经证实的翻译/字节/模块成本之前。native SHLD→ROL 的收益也不能直接套到这些 Wasm 捕获：这里已经能看到 `rorl`，还要检查回边 spill。

## 优先级三：区域优化放到已有的后台热点路径

建议沿用 `共享冷 pieces → 精简 CP → 后台 LLVM 热区`，先不引入 `CP → warm CP → LLVM` 这个额外的重编阶段。不是说 warm tier 永远无效，而是现有所有阈值都没有证明它在 OP-1 能回本。

对 QuickJS，值得尝试的是在已有远程编译任务中构造小的热 region，让 guest registers/flags 在区域内部保持 SSA，已知内部边成为分支；只在 helper、异常、预算耗尽和区域出口提交 CpuState。LLVM 继续负责机器码生成。现有 `BBTranslator` 的自循环 shadow state 是可扩展的基础；大 switch、间接目标或调用图不能靠将任意几个块拼起来自动解决。

区域发现先读缓存的出口/热边摘要，先判断值得编译，再取记录过的客体代码并 emit。需要上限：候选数量、字节、块数、调用路径复制次数、失败重试次数及每秒编译预算。优先选高占比、自循环或稳定的小型多块循环，避免在每个离开 cold tier 的块上生长一棵调用树。

异步发布必须校验 guest code/映射版本、guard 假设和成员依赖；构建期间保持原 CP 可执行。失效或不支持的出口回退原路径，不在实时执行线程触发成串重编。直接边若绕过原 chain table 的更新/失效机制，必须补上等价的有效性保证。

后台并不是免费：现有 profile 中 OP-1 约 7.3 s 墙钟消耗约 25 CPU-s，编译 Worker 与 V8 后台优化已有竞争。区域任务要有去重、取消过期任务、队列背压和资源预算；不要仅将原同步工作原封不动搬进更多 Workers。

## 先验证双重分层，再决定循环边界

bcore 的 CP/LLVM 和 V8 的 Liftoff/TurboFan 是两套分层。`t0-gate` 怀疑新 Wasm loop 一次调用耗尽很长链预算，导致已在后台完成的 TurboFan 无法接管当前 Liftoff frame。这是**尚未由该实验确定的因果解释**，值得做小型消融，不应直接据此全局缩短链预算。

建议固定同一个候选，只对它的内部循环每 K 个 packet 退出到可重新进入/可发布的边界，扫描少量 K。分离：bcore compile/emit、浏览器验证实例化、首次调用、首次 TurboFan 可用与实际进入、LLVM job ready/install、steady 执行和事件延迟。packet 预算仍按真实工作扣除，不把实现层的让出伪装成额外客体指令，也不能改变设备期限和中断状态。

只在确认“小 K 帮助新函数跨过浏览器分层，且收益大于边界成本”后考虑自适应策略。不能依赖 Pages 无法控制的 V8 flags；编译 Worker 中调用一次导出只证明走过首次编译路径，不保证整个区域已达到 TurboFan 稳态。

还需修正“启动基本停在 Liftoff”的宽泛理解：相关 notes 报告约 20,143 个 Liftoff 函数、1,623 个 TurboFan 函数，说明大量冷函数没有升层；但 Chrome 的 tier-0 采样时间约为 **TurboFan 2.66 s、Liftoff 1.19 s**。按函数数量和按执行时间得到的结论不同，热代码和冷代码需要同时优化。

## 接近 Dynarmic 的目标，应以 QuickJS 实际热点占比决定

`t0-gate/lab/host.cpp` 的 mode 0 只接收远程任务、不交付结果；mode 8 另关闭 local promotion。因此这些表是 CP 路径诊断。我们实际的 [QuickJS 对比](../2026-11-02-quickjs-bcore-wasm/README.md) 则开启正常远程 LLVM，最终每项安装 167–892 个块，bcore/dynarmic 为 2.194×。

**没有证据证明 t0-gate 的这四个提交会把 QuickJS 提速同样倍数。** 下一份性能 profile 应先回答 QuickJS 的测量窗口究竟花在 CP、LLVM guest blocks、分发、guest allocator/libc/softfloat，还是编译及调度。至少选择控制流/对象类与计算类的不同案例，给热点 PC 同时标注 guest symbol 和当前 tier；记录时间线，不用最终安装块数替代时间占比。

量化预算：从 2.194× 达到 1.2×，bcore 总耗时需减少约 **45.3%**；达到相当水平需减少约 **54.4%**。若某优化只覆盖 20% 的执行时间，即使该部分完全消失也不够。用 `新/旧时间 = (1-p) + p/s` 评估热点占比 p 和该处加速 s，不把多个存在重叠的收益相乘。

本文不以浮点作为下一步主要优化对象。可以先针对 QuickJS 的解释器分发、跨块状态和对象访存定位；仍应承认 ISA、编译器和 libc 的差异。省略叶栈帧是客体构建对照，不是 bcore 修复，更不能作为不重编 OP-1 固件也能获得的收益。

## “OP-1 不回退”应成为每一步的验收条件

建议每个独立改动从第一步就执行以下矩阵，比较候选与同次 baseline；不以 QuickJS 或平均总分抵消某项 OP-1 回退。

| 维度 | 必须验证 | 不能用什么替代 |
| --- | --- | --- |
| native 隔离 | Wasm 专属改动下 op1prof gate、boot/synth 配对、生成代码指纹保持预期 | native 仅编译成功 |
| Wasm 精确语义 | no-chain、chain、真实网页 tier/Worker 配置的 marks/digests；packet、ASTAT、循环状态、异常/MMIO 边界 | 只看画面或 checksum |
| 多块 region | 调用/多返回点、guard miss、循环寄存器变更、代码失效、预算耗尽处恢复状态 | 现有约 400 个 ISA 用例；记录显示它们几乎不覆盖 region |
| 网页启动 | 全新浏览器缓存与重访分开；Start→首帧/主界面，worker 内精确 milestone，主线程最坏阻塞 | 每 0.5 s 的 stats 用来判断 1–2% 变化 |
| OP-1 持续运行 | 固定按键/合成器/音序器或磁带回放脚本；音频 underrun、slice p95/p99、输入响应与稳定性 | 启动 digest 一致 |
| 资源 | 执行 Worker、编译 Worker 和 V8 后台 CPU，峰值内存、代码/函数/模块数 | 单看 guest 热循环耗时 |
| QuickJS 和微基准 | 相同 guest 与设置的配对测试，首次/晋升/持续运行分开；保留全部失败 | 将不同配置或最佳单轮拼成总分 |

可在实验前约定非劣界限，例如精确计时下启动中位数回退不超过 2%，但这只是建议的验收容忍度，不是当前 0.5 s 粗日志能验证的结论。需要足够的交替配对，使回退上界也落在约定范围内；高波动时判“证据不足”，不判“无回退”。音频 deadline、额外 underrun 和正确性错误应单列硬约束。

`SetHostOnly` 的 chained gate 补丁值得保留到测试工具：未触发的 mark PC 必须回到宿主。但它会改变被测控制流的边界，所以用它做语义 gate，用没有人为 mark 切断的真实页面做性能验收，两者不能互相代替。

本目录留存的 native/digest 通过结论来自原 README，`native.sh`/`web.sh` 保留了执行方法；没有找到对应 `/tmp/og` 的完整 gate 输出与 profile 原始样本，不能声称此次重新验证了它们。前述 web-cold6 性能数字同样是关联 notes 中的原型记录，新实现必须重新过 gate。

## 推荐实施顺序

1. **先补精确网页 milestone 和 chained 正确性 gate**，冻结 OP-1、QuickJS 双基线；低扰动采集 QuickJS 的 tier/热点占比。
2. **单独实现构建期 no-LB-tail 变体**，不带 Reader、local promotion 或 region；通过双基线后再继续。
3. **单独实现通用共享 piece 分组**，验证冷缓存和重访，避免固件专用清单。
4. **把 stencil 语义 metadata 前移到生成阶段**，再试有限字段活跃性和块内数据流优化，验证 Liftoff 与 TurboFan 两个阶段。
5. **通过已有 LLVM Worker 做小型热 region**，采用编译预算/收益筛选、版本校验与安全回退；用实际 QuickJS 热点决定先做哪类区域。
6. **最后才调整晋升阈值和循环让出策略**，只接受 Chrome 和真实 OP-1 证据支持的设置。

当前最值得拿来做下一次独立改动的是 **scalar LB-tail 的低成本省略**。它已有改善 OP-1 的原型证据，且能为随后更强的热区优化留出 CPU 和内存余量。接近 Dynarmic 的稳态差距则需要真实热点的区域/内存优化，不能只靠这项启动改进或承诺一个尚未测得的倍率。
