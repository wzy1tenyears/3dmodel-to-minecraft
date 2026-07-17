# Tests

工程测试与诊断源码集中放在这里，避免污染项目根目录。程序运行时使用的测试存档仍由 `test-worlds/` 管理，不把用户存档当成临时编译物清理。

- `probes/`：渲染后端、GPU LOD、缓存流水线等独立验证程序源码。
- `benchmarks/`：独立性能与质量评估源码。
- `out/`：所有测试编译物、缓存、日志和 release smoke 解包目录。

编译 probe 时应把项目根目录加入 include path，并把输出显式指定到 `tests/out/`。`out/` 中的内容不进入源码包。
