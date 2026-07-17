# 3dmodel-to-minecraft

Chinese version: [README.zh-CN.md](README.zh-CN.md)

## Features

- Two native Windows applications:
  `3dmodel-to-minecraft-gui.exe` for visual editing and `3dmodel-to-minecraft-cli.exe` for terminal workflows
- Real-time 3D editor:
  Orbit, pan, zoom, top/front/side views, model switching, grid/axes toggles in General Settings, placement gizmo, snapping, keyboard nudging, undo/redo, and placement presets
- Drag and drop:
  Drop an OBJ/GLB file, a model folder, or a Minecraft save folder anywhere on the GUI to configure it automatically
- Flexible world-preview range:
  Enter any radius from 1 to 1024 chunks on the World Preview & Scan page; toolbar presets remain available for quick changes
- Safer world operations:
  Test-copy and direct-write modes are mutually exclusive; direct imports and world reset require explicit confirmation
- Complete model preview with adaptive LOD:
  The full OBJ/GLB mesh is retained; interaction and distant views use runtime LOD without truncating the loaded source. LOD generation uses D3D12 Compute with automatic D3D11 and CPU fallback. The toolbar can force Full, LOD 1, LOD 2, or LOD 3 for inspection
- Multi-model overview with bounded LOD:
  Use Ctrl/Shift on the Model List page to combine any selection, or display every OBJ/GLB in the directory. Large inputs show progress; the combined overview is capped near two million triangles while single-model preview remains complete
- Level from a reference model:
  Select an OBJ on the Model List page and click `Use Selected OBJ as Level Plane` to estimate and fill the leveling normal automatically
- Material-aware OBJ/GLB preview:
  Samples GLB base-color textures and OBJ `MTL` / `map_Kd` textures into GPU vertex colors, including material alpha
- GPU-accelerated preview:
  One executable includes real Direct3D 11, Direct3D 12, and Vulkan renderers. Switch APIs at runtime from General Settings; the preview canvas is recreated automatically, the active API/GPU is shown below it, and OpenGL remains the compatibility fallback. The global cap is selectable at 30/60/120/144/180/200 Hz or unlimited
- Minecraft world context:
  Preview sampled blocks from the selected save as points, faces, or voxels. Standard `<world>\region` saves and older dimension-style test saves are both supported
- Faster block breakdown:
  Region files are scanned in parallel with bounded worker count, while the breakdown window always uses the save path selected by the user
- Built-in palette editor:
  Search, include, and exclude blocks without editing a configuration file manually
- Native C++ commands:
  `doctor`, `copy-world`, `reset`, `scan`, `glb`, `glb-copy`, `glb-surface`, `obj`, `obj-copy`, `obj-rotate`
- `--input` accepts either a single file or a directory:
  You can point directly to one `OBJ` / `GLB` file, or to a folder that contains multiple files
- Flexible world selection:
  Supports `--world-dir`
  Also supports `--world` with `--mc-root` / `--mc-version`
- Safer default workflow:
  `copy-world` is the preferred first step before writing to a real save
- Native textured GLB import:
  Reads embedded textures, UVs, and base color factors, then maps them to Minecraft blocks
- Native GLB surface import:
  Useful for quickly checking size, placement, and bounds with a single block type
- Native textured OBJ import:
  Supports `MTL`, `map_Kd`, material fallback colors, component filtering, rotation, mirroring, and optional clip-plane filtering
- Common import controls:
  `--center`
  `--base-y`
  `--rotate-y-deg`
  `--block-types`
  `--blocks`
  `--exclude-block-types`
  `--exclude-blocks`
  `--no-glass`
- World scan support:
  Can report block counts and bounds after import
- Test-world copy and reset support:
  Useful for repeated import tests without touching the original save
- OBJ bounds cache:
  Repeated imports against the same OBJ folder do not need to rescan every OBJ file

## How To Use

For visual editing, open `3dmodel-to-minecraft-gui.exe`. The Model & World page contains only two inputs: an OBJ/GLB file or directory, and a Minecraft save folder containing `level.dat`. Selecting or pasting these paths automatically loads the model and matching world preview; no extra Load Preview click is required. Compatibility paths remain available on the Advanced Paths page.

Open the Model List page to switch models, display all models together, or choose an OBJ as the leveling reference. When a model or a multi-model overview is large, confirm the LOD prompt and wait for the small progress window to finish.

For terminal use, open PowerShell in the executable folder and use `3dmodel-to-minecraft-cli.exe`.

Recommended first-use flow:

1. Read help and see which commands are available:

```powershell
.\3dmodel-to-minecraft-cli.exe help
```

2. Run diagnostics and confirm the world path and resource path are detected correctly:

```powershell
.\3dmodel-to-minecraft-cli.exe doctor
```

3. Copy a test world before writing:

```powershell
.\3dmodel-to-minecraft-cli.exe copy-world `
  --world-dir "C:\path\to\.minecraft\saves\WorldName"
```

4. Import into a copied test world:

Textured GLB import:

```powershell
.\3dmodel-to-minecraft-cli.exe glb `
  --yes `
  --direct `
  --input "C:\path\to\model.glb" `
  --world-dir "C:\path\to\copied-test-world" `
  --overwrite
```

GLB surface import:

```powershell
.\3dmodel-to-minecraft-cli.exe glb-surface `
  --yes `
  --direct `
  --input "C:\path\to\model.glb" `
  --world-dir "C:\path\to\copied-test-world"
```

OBJ import:

```powershell
.\3dmodel-to-minecraft-cli.exe obj `
  --obj-dir "C:\path\to\obj-tiles" `
  --only Tile_001.obj `
  --world-dir "C:\path\to\copied-test-world" `
  --yes `
  --direct `
  --overwrite
```

5. Scan the world after import:

```powershell
.\3dmodel-to-minecraft-cli.exe scan `
  --world-dir "C:\path\to\copied-world"
```

If you want palette-filtered scan results, add `--palette`:

```powershell
.\3dmodel-to-minecraft-cli.exe scan `
  --palette scene `
  --min-y -64 `
  --world-dir "C:\path\to\copied-world"
```

If you want to clear the copied world and retry:

```powershell
.\3dmodel-to-minecraft-cli.exe reset `
  --yes `
  --world-dir "C:\path\to\copied-world"
```

Common parameter notes:

- `--input`
  Pass a single file or a directory
  Best for the GLB flow
- `--obj-dir`
  Directory containing OBJ files
  Best for the OBJ flow
- `--only`
  Import just one file
  Useful for smoke tests
- `--world-dir`
  The safest world selection method
  Points to one exact world directory
- `--copy-world`
  Automatically create a copied test world before import
- `--direct`
  Write directly to the selected target world
  Better when you already trust the coordinates and output
- `--center`
  Set the import center manually
  Accepts `x,z` or `x,y,z`
- `--base-y`
  Set the default Y baseline
- `--rotate-y-deg`
  Rotate the import around the Y axis
- `--block-types` / `--blocks`
  Control which blocks are allowed
- `--exclude-block-types` / `--exclude-blocks`
  Control which blocks are excluded
- `--no-glass`
  Quickly remove glass-like blocks from the palette
- `--fallback-block`
  Fallback block when a textured GLB has no sampled texture color
- `--overwrite`
  Replace non-air blocks touched by this import; it does not track or remove blocks left at an older model position

Safety and compatibility:

- The writer currently requires world `DataVersion 4903`. A different or unreadable version is rejected before writing unless you explicitly pass `--force-version` and accept the compatibility risk.
- Generated chunks use a minimum Y of `-64` and a height of `1536`. Use a copied test world first when the world's dimension settings may differ.
- `--yes --direct` requires an explicit `--world-dir`. Direct writes also refuse an active `session.lock`; close Minecraft or the server before importing.
- Existing region files are backed up under `cpp-region-backups` before atomic replacement. World reset backups are stored under `cpp-world-backups`.
- A failed world copy is renamed with an `.incomplete` suffix when possible. Inspect or remove that copy before retrying.
- Cancelling a GUI task terminates its import process tree. A cancellation during writing can leave completed region replacements in place, so inspect the backup and run `scan` before continuing.

Recommended flows:

- First GLB import:
  `doctor -> copy-world -> glb -> scan`
- First OBJ import:
  `doctor -> copy-world -> obj -> scan`
- Quick shape / placement preview:
  Use `glb-surface` first
- Repeated OBJ testing:
  Keep using the same `objDir`
  The bounds cache under `cpp\cache\` will help

If you launch the CLI executable by double-clicking:

- The program opens a menu first
- Help and doctor do not immediately disappear
- Press Enter to return to the menu

If you run it inside a terminal:

- Commands exit normally after they finish
- This is better for repeated parameter tuning from PowerShell history
