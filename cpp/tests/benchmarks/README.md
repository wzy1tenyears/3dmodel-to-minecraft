# Benchmarks

`meshoptimizer_eval.cpp` 用于比较 meshoptimizer 的 LOD 简化质量、覆盖面积与编码体积。它依赖 meshoptimizer v1.2；编译时把该版本的 `src/` 加入 include path，并链接其实现源码。

benchmark 产物和临时依赖检出目录统一放到 `tests/out/`，不要提交第三方源码副本或在仓库根目录生成对象文件。
