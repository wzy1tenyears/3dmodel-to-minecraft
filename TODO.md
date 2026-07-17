# TODO：3dmodel-to-minecraft 全项目审计

审计日期：2026-07-11

审计范围：`cpp/` 原生 C++ GUI/CLI、模型导入器、Minecraft 世界读写、配置、文档、构建与发布包，以及旧 Node 项目清理结果。

本文件区分“当前现状”和“改进建议”。2026-07-11 已删除根目录旧 Node 项目，并清理 C++ 中遗留的 Node 兼容代码。

## 当前定位（现状）

- 项目现在只有 `cpp/` 原生 C++ 产品线，提供独立 GUI 和 CLI，以及原生 OBJ/GLB、NBT/Anvil、预览、调色板、扫描和存档操作。
- 根目录旧 npm 元数据、JS 业务脚本、Node 启动器、旧 EXE、旧 README、旧发布目录和示例配置均已删除。
- C++ `main.cpp` 已移除 `nodeExe`、`--node`、`NODE_EXE`、Node 路径探测、旧后端目录探测及无效形参透传。
- 用户发布物仍位于 `cpp/dist`；GitHub 源码入口是 `cpp/README.md` 与 `cpp/README.zh-CN.md`。
- 本机旧世界备份、旧 atlas、历史脚本、GUI 截图和验证工作区已统一归档到 `local-data/`，不参与构建或发布。

## 已具备能力（现状）

- [x] C++ GUI 和 CLI 均能成功构建，当前 `help`、`doctor`、`self-test` 可运行。
- [x] GUI 支持模型/存档选择、拖放、自动预览、模型列表、视角控制、摆放工具、LOD、调色板编辑和世界方块统计。
- [x] 默认安全模式倾向复制测试存档；CLI 复制导入会在复制前要求确认。
- [x] GUI 的直接写入和重置操作有二次确认。
- [x] 重置世界会把 `region`、`poi`、`entities` 移入世界内的 `cpp-world-backups`，不会直接删除。
- [x] C++ 世界读取兼容标准 `<world>/region`，并保留旧 dimension-style 测试世界回退。
- [x] C++ README 已使用通用示例路径，没有把本机工作目录当公开示例。
- [x] 当前 release ZIP 没有包含 `config.local.json`；解压后生成的配置仍保留在用户本地。
- [x] 根目录旧 Node/JS 产品线已删除，C++ GUI/CLI 不再探测或依赖 Node 运行时。
- [x] `package.json`、`package-lock.json`、`node_modules` 和旧 Node 发布物已清除。

## P0：修复会造成误写或结果不一致的问题（建议）

- [x] 未知参数立即报错并返回退出码 2；已删除的 `--dry-run` 不再进入参数白名单，也不会回退为 `copy-world`。
- [x] 目录导入的 `--all` 与 GUI“处理目录内全部模型”会真实控制三个原生导入器的文件选择。
- [x] 目录输入未选择 `--all` 且未指定 `--only` 时会拒绝执行，不再默认全量导入。
- [x] OBJ Z 镜像的 GUI 预览和运行时默认值已统一为关闭，仅显式 `--flip-z` 时开启。
- [x] textured GLB 与 surface GLB 已读取并应用旋转、缩放和 X/Z 镜像，预览与导入共用同一参数语义。
- [x] 所有导入计划在写入前验证目标目录及 `level.dat`，普通目录不能作为存档写入。
- [x] 写世界前检测 `session.lock` 活跃状态，并要求先关闭 Minecraft 客户端或服务端。
- [x] region 写入已改为同目录临时文件、flush/大小校验和原子替换；覆盖既有 region 前保留备份。
- [x] GUI 导入前显示模型数量、模式、世界、安全方式、中心、旋转、缩放、镜像和覆盖语义并二次确认。
- [x] GUI 长任务使用受控子进程实时追加日志，提供取消按钮；取消或关闭窗口会终止 Job Object 中的子进程树。
- [x] 世界预览使用 `previewWorldMaxBlocks` 上限，默认最多保留 250000 个方块，并限制最大区块半径。
- [x] release ZIP 已包含 GUI/CLI、`vendor/ots/res`、双语 README、版本说明和第三方声明；在全新目录解压后 `doctor`、`help` 均通过。

## P1：让参数、界面和文档真实一致（建议）

- [x] `overwrite` 和 `batchBlockLimit` 已在三个原生导入器中生效；`workers` 保留为 OBJ/OBJ surface 的 1–64 并行构建设置，并贯通 CLI、配置、GUI 和原生 importer。
- [x] `overwrite` 已明确定义为覆盖本次坐标触及的非空气方块；README 明确说明不会清理模型旧位置遗留方块。
- [x] 导入数字参数使用统一严格校验并给出字段级错误；非法旋转、批量上限等输入不会进入导入器。
- [ ] 明确“目录是一组拼接 tile”还是“目录是多个独立模型”。当前即使只选一个文件，bounds/manifest 仍按目录全部文件计算；这适合 tile 集合，但会让独立模型的落点和尺寸难以理解。
- [x] `--yes --direct` 要求显式 `--world-dir`；交互和 GUI 会展示导入计划并要求确认目标世界。
- [x] 复制世界前估算并显示源大小，要求至少 110% 可用空间；失败副本会尽量标记为 `.incomplete`。
- [x] 写入前读取并校验世界 `DataVersion`；README 已说明写入器固定值 `4903`、`minY=-64`、`height=1536` 及 `--force-version` 风险。
- [ ] GUI 按导入模式隐藏/禁用无效设置。GLB 页面不应让用户误以为 OBJ 的找平、旋转、镜像、组件过滤都已作用于后端。
- [ ] 完成 GUI 国际化。CLI 支持中英文，但 GUI 的页签、字段、状态和对话框大多硬编码中文；配置为 `lang=en` 仍显示中文界面。
- [ ] 改善小屏和高 DPI。GUI 初始窗口固定为 `1500x920`，未发现 DPI awareness 或最小尺寸策略；在 1366x768、缩放显示器和窄窗口上需要实测。
- [ ] 优化默认页面的信息层级：首屏只保留模型、存档、安全模式和主要动作；模式专属参数留在对应页面，高级路径继续下沉。
- [x] README 已修正步骤编号，并补充失败副本、备份位置、取消语义、direct-write 和版本兼容风险。
- [x] GUI 的测试副本/直接写入已改为单选逻辑，载入旧配置时也会归一化为严格二选一。
- [x] GUI 与 CLI 已接入 `obj-surface`，复用原生 OBJ 几何流程并以指定单方块生成表面。
- [x] “实景半径（区块）”输入已允许 1–1024，失焦时按该范围校正并保存。

## P1：完善单一 C++ 产品入口和发布物（建议）

- [x] C++ 版已成为唯一产品；根目录旧 Node README、旧 EXE、npm 入口和 `dist-release` 已移除。
- [x] 遗留的 `cpp/3dmodel-to-minecraft-cpp.exe` 已移出源码目录并归档到 `local-data/legacy-build-artifacts`，当前产品入口只有新 GUI/CLI 双 EXE。
- [x] release ZIP 包含两个 exe、`vendor/ots/res`、双语 README、第三方声明和版本说明，并已在全新临时目录中通过 `doctor` 发布门。
- [x] GitHub source ZIP 已加入 `build.ps1`、`package-release.ps1`、`BUILDING.md`、第三方声明和版本说明。
- [ ] 为 exe 添加 FileVersion、ProductVersion、ProductName 和图标；正式公开分发时评估代码签名。当前两个 exe 没有版本资源且未签名。
- [x] 根目录 `.gitignore` 已覆盖本地数据、构建、发布、缓存、测试存档和二进制；MSVC `.obj`/PDB 已分别输出到 `cpp/build/obj/gui` 与 `cpp/build/obj/cli`。
- [x] 已停止双实现维护；旧 JS 辅助脚本和 Node 自检入口已删除。

## P2：可靠性和维护性（建议）

- [x] 将 CLI 参数结构、参数解析和导入数字校验拆到 `cpp/app/cli_args.h` 与 `cpp/app/cli_args.cpp`，`main.cpp` 不再承载这部分实现。
- [ ] 将导入模式识别、导入计划和安全门拆到独立模块。
- [ ] 将配置读写与默认值合并逻辑拆到独立模块。
- [ ] 将 GUI 页面创建、控件映射和布局拆到独立模块。
- [ ] 将 OBJ/GLB 与世界预览渲染、LOD 和交互拆到独立模块。
- [ ] 将 GUI 异步任务、子进程与 Job Object 生命周期拆到独立模块。
- [ ] 建立自动测试：参数解析、安全模式、未知参数、`--all/--only`、模式专属参数、预览/导入变换一致性、错误 world-dir、region 原子写入和 reset 恢复。
- [ ] 为 NBT/Anvil 写入增加小型 fixture 世界的往返测试，并用真实 Minecraft 目标版本打开验证。
- [ ] 为 OBJ/GLB 建立最小 fixture：单文件、tile 目录、混合目录、无 UV、外部/内嵌贴图、旋转/镜像/缩放和非法输入。
- [ ] 增加发布验收脚本：全新目录解压、`help`、`doctor`、无配置启动 GUI、dry-run、copy-world、scan，以及 ZIP 内容白名单检查。
- [ ] 给 GUI 异步任务增加受控生命周期，避免 detached thread 在窗口关闭后继续投递消息或泄漏 payload。

## 2026-07-11 实测证据

- C++ 当前 build 中 `3dmodel-to-minecraft-cli.exe help`、`doctor`、`self-test` 均通过。
- 将当前 `cpp/dist/release.zip` 解压到项目外并在解压目录运行 `doctor`，结果为 `resource root : (未找到)`，诊断失败。
- 2026-07-12 重新构建后，`--dry-run` 和任意未知参数均输出“未知参数”并返回退出码 2；源码中已无 `dryRun` 字段或分支。
- 三个原生导入器均要求 `allFiles=true` 或显式 `onlyFile`，GUI/CLI 的选择状态会传入该执行门。
- `overwrite` 与 `batchBlockLimit` 已进入三个原生导入器；`workers` 使用 `std::async` 分批并行构建 OBJ，随后按原顺序串行写入世界。
- GLB 两个 options 结构已包含并应用旋转、缩放与镜像；OBJ 后端与 GUI 的 Z 镜像默认值已统一。
- 世界预览默认 `previewWorldMaxBlocks=250000`，加载时会应用该上限。
- 根目录已不存在 `.js`、`package.json` 或 `package-lock.json`；C++ 公开源码中也不存在 Node 运行依赖。
- 删除旧实现后，C++ GUI/CLI 已重新构建，CLI `help` 正常，发布 ZIP 已重新生成。
- C++ reset 和 import 均有备份；region 写入使用临时文件和 `MoveFileExW(...MOVEFILE_WRITE_THROUGH)` 原子替换。
- 文件整理后，`cpp/` 根目录只保留源码、依赖、工具、构建/发布目录、测试存档和用户文档；旧备份及验证材料均位于被忽略的 `local-data/`。
- 整理后的 `build.ps1` 已成功生成 GUI/CLI，源码根目录不再产生 `.obj`、PDB 或旧单 EXE。
- 2026-07-12 的 `release.zip` 在全新 `build/release-smoke` 目录中通过 `doctor` 和 `help`，资源根解析为解压目录自身。
