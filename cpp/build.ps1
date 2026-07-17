Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sources = @(
    (Join-Path $scriptDir "main.cpp"),
    (Join-Path $scriptDir "app\cli_args.cpp"),
    (Join-Path $scriptDir "native\nbt.cpp"),
    (Join-Path $scriptDir "native\palette.cpp"),
    (Join-Path $scriptDir "native\world_ops.cpp"),
    (Join-Path $scriptDir "native\glb_surface_importer.cpp"),
    (Join-Path $scriptDir "native\glb_textured_importer.cpp"),
    (Join-Path $scriptDir "native\obj_textured_importer.cpp"),
    (Join-Path $scriptDir "native\preview_mesh.cpp"),
    (Join-Path $scriptDir "native\preview_cache.cpp"),
    (Join-Path $scriptDir "native\preview_directstorage.cpp"),
    (Join-Path $scriptDir "native\preview_gpu_lod.cpp"),
    (Join-Path $scriptDir "native\preview_gpu_lod_d3d12.cpp"),
    (Join-Path $scriptDir "native\preview_gpu_lod_dispatch.cpp"),
    (Join-Path $scriptDir "native\preview_d3d11_renderer.cpp"),
    (Join-Path $scriptDir "native\preview_d3d12_renderer.cpp"),
    (Join-Path $scriptDir "native\preview_vulkan_renderer.cpp"),
    (Join-Path $scriptDir "native\preview_renderer.cpp"),
    (Join-Path $scriptDir "third_party\miniz.c"),
    (Join-Path $scriptDir "third_party\miniz_tdef.c"),
    (Join-Path $scriptDir "third_party\miniz_tinfl.c")
)
$buildDir = Join-Path $scriptDir "build"
$outDir = Join-Path $buildDir "Release"
$guiExePath = Join-Path $outDir "3dmodel-to-minecraft-gui.exe"
$cliExePath = Join-Path $outDir "3dmodel-to-minecraft-cli.exe"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found: $vswhere"
}

$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw "Visual Studio Build Tools with C++ workload not found."
}

$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$sourceArgs = $sources | ForEach-Object { "`"$_`"" }

function Invoke-BuildTarget {
    param(
        [string]$DefineName,
        [string]$OutputPath,
        [string]$ObjectFolder
    )

    $objDir = Join-Path $buildDir ("obj\" + $ObjectFolder)
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $objDirForCl = ($objDir -replace '\\', '/') + '/'

    $clArgs = @(
        "/nologo",
        "/std:c++17",
        "/EHsc",
        "/O2",
        "/utf-8",
        "/DUNICODE",
        "/D_UNICODE",
        "/D_CRT_SECURE_NO_WARNINGS",
        "/D$DefineName",
        "/Fo`"$objDirForCl`"",
        "/Fd`"$($objDirForCl)compiler.pdb`"",
        "/I", "`"$scriptDir`"",
        "/I", "`"$(Join-Path $scriptDir 'native')`"",
        "/I", "`"$(Join-Path $scriptDir 'third_party')`"",
        "/I", "`"$(Join-Path $scriptDir 'third_party\directstorage\include')`"",
        "/I", "`"$(Join-Path $scriptDir 'third_party\vulkan_headers\include')`""
    )
    $clArgs += $sourceArgs
    $clArgs += @(
        "/link",
        "User32.lib",
        "Gdi32.lib",
        "Comctl32.lib",
        "Opengl32.lib",
        "Shell32.lib",
        "Comdlg32.lib",
        "Ole32.lib",
        "Winmm.lib",
        "d3d11.lib",
        "d3d12.lib",
        "d3dcompiler.lib",
        "dxgi.lib",
        "/OUT:`"$OutputPath`""
    )

    $command = "`"$vcvars`" >nul && cl " + ($clArgs -join " ")
    cmd /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
}

Invoke-BuildTarget -DefineName "CODEX_GUI_EXE" -OutputPath $guiExePath -ObjectFolder "gui"
Invoke-BuildTarget -DefineName "CODEX_CLI_EXE" -OutputPath $cliExePath -ObjectFolder "cli"

$directStorageBin = Join-Path $scriptDir "third_party\directstorage\bin\x64"
foreach ($dll in @("dstorage.dll", "dstoragecore.dll")) {
    $sourceDll = Join-Path $directStorageBin $dll
    if (-not (Test-Path $sourceDll)) {
        throw "Missing DirectStorage runtime: $sourceDll"
    }
    Copy-Item -LiteralPath $sourceDll -Destination (Join-Path $outDir $dll) -Force
}

Write-Host "Built GUI: $guiExePath"
Write-Host "Built CLI: $cliExePath"
