# 两项默认关闭的 emission 选项（2026-11-05）

最终只提供 `sharedExit` 和 `constantMemory` 两个显式开关，C++ 对应
`OptimizationFlag::WasmSharedExit` / `WasmConstantMemory`。
`UserConfig` 默认使用的 `default_optimizations` 和 JavaScript 工厂默认都不启用它们。
显式选择 `all_safe_optimizations` 会包含这两项，与现有 region sharing 的分类方式相同。
没有加入 i32 算术或 local.tee 的优化实现/选项。

| 选项 | 适用条件 |
| --- | --- |
| `sharedExit` | A32/Thumb 多入口 fallback region，需同时启用 `regionMultiEntry`；C++ 还需 cycle counting 和有效 `BoundaryState` |
| `constantMemory` | A32 立即数地址的 page-table 访存，保留 live pointer、mask、null/MMIO/protection 和跨页 thunk；flat-window 路径不变 |

JavaScript 示例：

```js
const cpu = await createEmulator({
  architecture: 'a32',
  optimizations: {
    regionMultiEntry: true,
    sharedExit: true,
    constantMemory: true,
  },
});
```

两个开关独立，省略或显式 `false` 都关闭；策略在实例创建时固定。
不满足适用条件时不改变相应生成路径。A64 忽略这两个 A32 emission 选项，现有
`regionMultiEntry` 的 A64 支持保持原样。JS 工厂在第一个 await 之前读取所有开关，
未知名称和非布尔值拒绝初始化。私有 `dj_create` 参数改为 bit mask，原有 0/1 含义不变。

完整共享出口通过 emitter 的返回 helper 汇合，不增加 `FuncBody` 的全局 return override
回调。普通 return 的结果、tail-call 参数、源块 deferred NZCV、公共 GPR/cycle 提交、
selector miss 的早退与 callback coherence 都保留原契约。

## 验证

- native CTest **25/25**，包括新增配置映射测试、安装后 public-header 消费者、整数 oracle
  和 192 项常量访存 byte oracle。
- Wasm CTest **12/12**，包括全部开启两项的新 A32 运行时回归：寄存器/标志、预算、
  callbacks、ARM/Thumb、IT/endian、descriptor miss、alias invalidation、嵌套调用和 Step。
- checked-in fuzz corpus **6/6**。
- 直接生成四种配置（关闭 / shared / constant / shared+constant），single 与 region 两类，
  六种负载、两个 tier，共 **96 个功能进程**，全部通过独立结果/NZCV/store/入口检查。
- **48 份文件逐字节匹配**前两次研究的冻结模块：默认关闭匹配专门化前基准；单项及组合
  开启匹配相应已测实现。见 `options-summary.json` / 原始 `options-checks.json`。
  默认指令流未混入可选优化，不把这份代码身份检查当作新的吞吐资格。
- 实际打包 tarball：Node **45** checks、Chromium **42**、Firefox **43**，以及
  TypeScript NodeNext/Bundler、publint/ATTW、Vite Worker **6** 项工具链检查均通过。
  API 用例包括默认/显式 false 的生成字节一致、四种组合、参数快照、标志和 cache clear。
- `docs/Embedding.md` 和 `emit_wasm.cpp` 中任务开始前已有的改动完整保留、不提交。

本次是 API 和功能验证，没有重跑默认吞吐资格或真实固件 First Play。
此前性能证据及失败不改写；新配置是否有收益应按实际引擎和负载选择。
性能动机见 [条件专门化](../2026-11-05-conditional-emission/README.md) 和
[共享出口组合](../2026-11-05-shared-exit-interactions/README.md)。

## 复现

构建 native 和 Emscripten，然后执行 CTest、corpus 和 npm installed-package 检查。
新 native 测试验证两种架构的 public flag/default/all-safe 行为以及 A32 配置传递；
`a32_region_alias_test.js emission` 对两个选项全开重跑运行时合约。

`check-options.py` 使用当前 `emission_probe` 和已归档冻结模块验证所有组合：

```sh
python3 internals/measurements/2026-11-05-emission-options/check-options.py
```

脚本拒绝覆盖输出；复测使用新的输出目录或证据副本。完整生成模块、原始检查输出、
编译/测试和 package 日志在 `evidence.tar.gz`；摘要和采集脚本直接保留。
归档内每个文件与原文件逐字节核对。在本目录解包后验证：

```sh
tar -xzf evidence.tar.gz
sha256sum -c evidence.sha256
```
