# AGENTS

本文件记录当前 C++ 项目的目录与测试约定；通用协作规则由上级约定负责。

## 目录职责

- `app/`、`native/`：应用层与原生实现。
- `third_party/`、`vendor/`：第三方源码、头文件和运行时资源。
- `tests/probes/`：独立 probe、smoke 与质量验证源码。
- `tests/out/`：工程测试生成的可执行文件、对象文件、缓存和临时解包目录。
- `build/Release/`：正式构建产物；`dist/`：发布打包产物。
- `test-worlds/`：程序默认测试存档与用户测试数据，不作为临时构建目录清理。

## 测试文件规则

- 不要在仓库根目录生成 `.obj`、`.pdb`、`.ilk`、probe `.exe` 或 `release-smoke-*` 目录。
- 新工程测试源码统一放到 `tests/`，临时输出统一放到 `tests/out/`。
- 发布包解包验证应使用 `tests/out/release-smoke-<id>/`，验证完成后删除该临时目录。
- 不要把临时 CMake 构建树放在仓库根目录；测试专用构建树也放入 `tests/out/`。
- 清理测试产物时不得整体删除 `build/`、`dist/`、`vendor/` 或 `test-worlds/`。
