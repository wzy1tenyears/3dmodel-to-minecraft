# 3dmodel-to-minecraft

English version: [README.md](README.md)

## Features

- 提供两个原生 Windows 程序：
  `3dmodel-to-minecraft-gui.exe` 用于可视化编辑，`3dmodel-to-minecraft-cli.exe` 用于终端工作流
- 实时 3D 编辑器：
  支持旋转、平移、缩放、顶视/前视/侧视、模型切换、网格/坐标轴、摆放 gizmo、吸附、键盘微调、撤销/重做和摆放预设；网格与坐标轴开关位于“通用设置”
- 拖放识别：
  可把 OBJ/GLB 文件、模型目录或 Minecraft 存档目录直接拖到 GUI，程序会自动填写对应路径
- 自定义实景范围：
  可在“实景与扫描”页输入 1–1024 区块的任意预览半径，工具栏仍保留常用范围快捷切换
- 更安全的存档操作：
  测试副本与直接写入互斥；直接写入和重置存档前必须再次确认
- 完整模型预览与自适应 LOD：
  始终保留完整 OBJ/GLB 网格，交互和远景只在渲染时切换 LOD，不截断载入的源模型；LOD 生成优先使用 D3D12 Compute，并自动回退 D3D11 或 CPU；工具栏也可以强制查看完整、LOD 1、LOD 2 或 LOD 3
- 全部模型 LOD 总览：
  可在“模型列表”页用 Ctrl/Shift 多选任意模型并合并显示，也可显示目录内全部 OBJ/GLB。大模型会弹出进度小窗；全部模型总览约限制在 200 万三角形，单模型预览仍保留完整网格
- 以模型估算找平面：
  在“模型列表”页选中一个 OBJ，点击“以所选 OBJ 作为找平面”，程序会自动估算并填写找平法线
- OBJ/GLB 材质预览：
  将 GLB 基础色贴图和 OBJ `MTL` / `map_Kd` 贴图采样为 GPU 顶点颜色，并支持材质透明度
- GPU 加速预览：
  一个 EXE 同时包含 Direct3D 11、Direct3D 12 和 Vulkan 真实绘制后端，可在“通用设置”运行时切换并自动刷新预览画布；下方显示实际 API/显卡，失败时自动回退 OpenGL。全局刷新上限可选 30/60/120/144/180/200 Hz 或无上限
- 对应存档实景：
  可以把所选 Minecraft 存档采样显示为点、面或体素；同时兼容标准 `<世界>\region` 和旧测试存档的 dimension-style 布局
- 更快的方块统计：
  使用受限数量的工作线程并行扫描 region 文件，而且方块统计始终扫描用户明确选择的存档，不再自动切换到最新测试副本
- 内置方块调色板编辑器：
  可搜索、加入和排除方块，不需要手动编辑配置文件
- 原生 C++ 实现的核心命令：
  `doctor`、`copy-world`、`reset`、`scan`、`glb`、`glb-copy`、`glb-surface`、`obj`、`obj-copy`、`obj-rotate`
- `--input` 同时支持单个文件和目录：
  可以直接传一个 `OBJ` / `GLB` 文件，也可以传一个包含多个文件的目录
- 世界定位更泛用：
  支持直接传 `--world-dir`
  也支持 `--world` + `--mc-root` / `--mc-version`
- 默认更安全：
  第一次使用更适合走 `copy-world`
  先复制测试存档，再实际写入
- 支持 textured GLB 导入：
  会读取嵌入贴图、UV、基础色，并映射到 Minecraft 方块调色板
- 支持 GLB surface 导入：
  适合先用单一方块快速看体量、位置和边界
- 支持 textured OBJ 导入：
  支持 `MTL`、`map_Kd` 贴图、材质回退色、组件过滤、旋转、镜像、可选 clip-plane
- 支持常用导入参数：
  `--center`
  `--base-y`
  `--rotate-y-deg`
  `--block-types`
  `--blocks`
  `--exclude-block-types`
  `--exclude-blocks`
  `--no-glass`
- 支持世界扫描：
  可以按调色板模式统计导入后的世界方块数量和边界
- 支持测试存档复制和重置：
  方便反复试导入，不直接污染原始存档
- 支持 OBJ bounds 缓存：
  同一目录重复 import 时，不需要每次都重新扫描全部 OBJ 边界

## How To Use

可视化使用时，直接打开 `3dmodel-to-minecraft-gui.exe`。“模型与存档”页只保留两个输入：OBJ/GLB 模型文件或目录，以及包含 `level.dat` 的 Minecraft 存档。选择或粘贴这两个路径后会自动加载模型和对应存档实景，不需要再点击“加载预览”。兼容目录仍可在“高级路径”页修改。

进入“模型列表”页可以切换单个模型、同时显示全部模型，或把所选 OBJ 用作找平面参考。模型较大时确认 LOD 提示，然后等待进度小窗处理完成即可。

终端使用时，在 exe 所在目录打开 PowerShell，运行 `3dmodel-to-minecraft-cli.exe`。

第一次使用推荐顺序：

1. 先看帮助，确认当前版本支持哪些命令

```powershell
.\3dmodel-to-minecraft-cli.exe help
```

2. 跑一次环境诊断，确认世界目录和资源目录能正确识别

```powershell
.\3dmodel-to-minecraft-cli.exe doctor
```

3. 先复制一个测试存档，不直接写原始世界

```powershell
.\3dmodel-to-minecraft-cli.exe copy-world `
  --world-dir "C:\path\to\.minecraft\saves\WorldName"
```

4. 写入复制出来的测试存档

GLB textured 导入：

```powershell
.\3dmodel-to-minecraft-cli.exe glb `
  --yes `
  --direct `
  --input "C:\path\to\model.glb" `
  --world-dir "C:\path\to\copied-test-world" `
  --overwrite
```

GLB surface 导入：

```powershell
.\3dmodel-to-minecraft-cli.exe glb-surface `
  --yes `
  --direct `
  --input "C:\path\to\model.glb" `
  --world-dir "C:\path\to\copied-test-world"
```

OBJ 导入：

```powershell
.\3dmodel-to-minecraft-cli.exe obj `
  --obj-dir "C:\path\to\obj-tiles" `
  --only Tile_001.obj `
  --world-dir "C:\path\to\copied-test-world" `
  --yes `
  --direct `
  --overwrite
```

5. 导入完成后，用 `scan` 回读世界，确认方块数量和边界

```powershell
.\3dmodel-to-minecraft-cli.exe scan `
  --world-dir "C:\path\to\copied-world"
```

如果你想只统计某类调色板结果，可以加 `--palette`：

```powershell
.\3dmodel-to-minecraft-cli.exe scan `
  --palette scene `
  --min-y -64 `
  --world-dir "C:\path\to\copied-world"
```

如果你要清空测试存档再重来，可以用：

```powershell
.\3dmodel-to-minecraft-cli.exe reset `
  --yes `
  --world-dir "C:\path\to\copied-world"
```

常见参数说明：

- `--input`
  传单个文件或目录
  适合 GLB 主流程
- `--obj-dir`
  OBJ 文件所在目录
  适合 OBJ 主流程
- `--only`
  只导入一个指定文件
  适合 smoke test
- `--world-dir`
  最稳妥的世界定位方式
  直接指向一个具体世界目录
- `--copy-world`
  导入前自动复制测试存档
- `--direct`
  直接写当前目标世界
  更适合你已经确认过坐标和结果的时候
- `--center`
  手动指定导入中心
  可以用 `x,z` 或 `x,y,z`
- `--base-y`
  手动控制默认高度基线
- `--rotate-y-deg`
  对导入结果绕 Y 轴旋转
- `--block-types` / `--blocks`
  控制允许使用哪些方块
- `--exclude-block-types` / `--exclude-blocks`
  控制排除哪些方块
- `--no-glass`
  快速排除玻璃类方块
- `--fallback-block`
  textured GLB 没有可采样颜色时的回退方块
- `--overwrite`
  允许覆盖本次导入坐标触及的非空气方块；不会追踪或清理模型旧位置遗留的方块

安全与兼容性：

- 当前写入器要求世界 `DataVersion` 为 `4903`。版本不同或无法读取时会在写入前拒绝；只有明确接受兼容风险时才应使用 `--force-version`。
- 新生成的区块使用最低 Y `-64`、总高度 `1536`。世界维度设置可能不同时，必须先使用测试副本验证。
- `--yes --direct` 必须显式提供 `--world-dir`。直接写入还会拒绝活跃的 `session.lock`；导入前请关闭 Minecraft 或服务端。
- 覆盖既有 region 前会备份到 `cpp-region-backups`，重置世界的备份位于 `cpp-world-backups`。
- 世界复制失败时会尽量把残留副本改名为 `.incomplete`；重试前应检查或删除该副本。
- 在 GUI 取消任务会终止整个导入进程树。若取消发生在写入期间，已经完成原子替换的 region 仍会保留；继续前应检查备份并运行 `scan`。

推荐流程：

- 第一次导入 GLB：
  `doctor -> copy-world -> glb -> scan`
- 第一次导入 OBJ：
  `doctor -> copy-world -> obj -> scan`
- 只想快速看模型落点和体量：
  先用 `glb-surface`
- 需要反复试 OBJ：
  保持同一个 `objDir`
  这样 `cpp\cache\` 里的 bounds 缓存会生效
