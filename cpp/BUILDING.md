# Building from source

## Requirements

- Windows 10 or later
- Visual Studio 2022 Build Tools with the Desktop development with C++ workload
- PowerShell 5.1 or later
- CMake 3.21 or later (optional; Visual Studio's bundled CMake is supported)
- A current graphics driver for D3D11/D3D12; Vulkan is loaded dynamically when the system Vulkan runtime is available

## Build

From this directory, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

The build produces:

- `build\Release\3dmodel-to-minecraft-gui.exe`
- `build\Release\3dmodel-to-minecraft-cli.exe`
- `build\Release\dstorage.dll` and `dstoragecore.dll`

The GUI contains D3D11, D3D12, and Vulkan preview renderers in one executable. LOD generation uses a separate hardware D3D12 Compute path with D3D11 fallback. Vulkan headers are vendored for compilation; no Vulkan SDK or `vulkan-1.lib` is required.

You can also configure and build with CMake:

```powershell
cmake -S . -B build-cmake -G "Visual Studio 17 2022" -A x64
cmake --build build-cmake --config Release --parallel
```

Verify the command-line executable before packaging:

```powershell
.\build\Release\3dmodel-to-minecraft-cli.exe help
.\build\Release\3dmodel-to-minecraft-cli.exe doctor
```

## Tests and diagnostics

Standalone renderer, GPU LOD, and cache probes live in `tests\probes`; quality benchmarks live in `tests\benchmarks`. They are not part of the default GUI/CLI build. Put every test object, executable, cache, log, and unpacked smoke package under `tests\out`.

To create the user release and source archives, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\package-release.ps1
```

The packaging script uses `build\Release` by default and refuses to package executables older than the source. To package a CMake build instead, pass its Release directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\package-release.ps1 -BuildRoot .\build-cmake\Release
```
