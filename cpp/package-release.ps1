param(
    [string]$BuildRoot = "build\Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$resolvedBuildRoot = if ([System.IO.Path]::IsPathRooted($BuildRoot)) { $BuildRoot } else { Join-Path $scriptDir $BuildRoot }
$buildGuiExe = Join-Path $resolvedBuildRoot "3dmodel-to-minecraft-gui.exe"
$buildCliExe = Join-Path $resolvedBuildRoot "3dmodel-to-minecraft-cli.exe"
$distDir = Join-Path $scriptDir "dist"
$sourceRoot = Join-Path $distDir "github-source"
$sourceZip = Join-Path $distDir "github-source.zip"
$releaseRoot = Join-Path $distDir "release"
$releaseZip = Join-Path $distDir "release.zip"
$legacyReleaseRoot = Join-Path $distDir "3dmodel-to-minecraft-cpp-release"
$legacyReleaseZip = Join-Path $distDir "3dmodel-to-minecraft-cpp-release.zip"

if (-not (Test-Path $buildGuiExe)) {
    throw "Missing built GUI exe: $buildGuiExe`nRun .\build.ps1 first."
}
if (-not (Test-Path $buildCliExe)) {
    throw "Missing built CLI exe: $buildCliExe`nRun .\build.ps1 first."
}

$sourceInputs = @(
    (Join-Path $scriptDir "main.cpp"),
    (Join-Path $scriptDir "app"),
    (Join-Path $scriptDir "native"),
    (Join-Path $scriptDir "third_party"),
    (Join-Path $scriptDir "build.ps1"),
    (Join-Path $scriptDir "CMakeLists.txt")
)
$sourceFiles = foreach ($sourceInput in $sourceInputs) {
    if (Test-Path -LiteralPath $sourceInput -PathType Container) {
        Get-ChildItem -LiteralPath $sourceInput -File -Recurse
    } elseif (Test-Path -LiteralPath $sourceInput -PathType Leaf) {
        Get-Item -LiteralPath $sourceInput
    }
}
$newestSource = $sourceFiles | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$oldestExecutableTime = @(
    (Get-Item -LiteralPath $buildGuiExe).LastWriteTimeUtc,
    (Get-Item -LiteralPath $buildCliExe).LastWriteTimeUtc
) | Sort-Object | Select-Object -First 1
if ($newestSource -and $newestSource.LastWriteTimeUtc -gt $oldestExecutableTime) {
    throw "Built executables are older than source file: $($newestSource.FullName)`nRebuild before packaging."
}

foreach ($path in @($sourceRoot, $releaseRoot)) {
    if (Test-Path $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $path | Out-Null
}

$projectLicense = Join-Path $repoRoot "LICENSE"
if (-not (Test-Path -LiteralPath $projectLicense -PathType Leaf)) {
    throw "Missing project license: $projectLicense"
}
Copy-Item -LiteralPath $projectLicense -Destination (Join-Path $sourceRoot "LICENSE")
Copy-Item -LiteralPath $projectLicense -Destination (Join-Path $releaseRoot "LICENSE")

foreach ($legacyPath in @($legacyReleaseRoot, $legacyReleaseZip)) {
    if (Test-Path $legacyPath) {
        Remove-Item -LiteralPath $legacyPath -Recurse -Force
    }
}

foreach ($item in @("main.cpp", "app", "native", "third_party", "AGENTS.md", "CMakeLists.txt", "build.ps1", "package-release.ps1", "BUILDING.md", "VERSION.txt", "THIRD_PARTY_NOTICES.md")) {
    $src = Join-Path $scriptDir $item
    if (Test-Path $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $sourceRoot $item) -Recurse -Force
    }
}

$sourceTestsRoot = Join-Path $sourceRoot "tests"
New-Item -ItemType Directory -Force -Path $sourceTestsRoot | Out-Null
foreach ($item in @("README.md", "probes", "benchmarks")) {
    $src = Join-Path (Join-Path $scriptDir "tests") $item
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $sourceTestsRoot $item) -Recurse -Force
    }
}
$sourceTestsOut = Join-Path $sourceTestsRoot "out"
New-Item -ItemType Directory -Force -Path $sourceTestsOut | Out-Null
Copy-Item -LiteralPath (Join-Path $scriptDir "tests\out\.gitignore") -Destination (Join-Path $sourceTestsOut ".gitignore")

foreach ($item in @("README.md", "ots\LICENSE.txt", "ots\res\palettes\colourful.ts")) {
    $src = Join-Path (Join-Path $scriptDir "vendor") $item
    if (Test-Path -LiteralPath $src -PathType Leaf) {
        $dst = Join-Path (Join-Path $sourceRoot "vendor") $item
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst
    }
}

$sourceAtlasToolRoot = Join-Path $sourceRoot "tools\atlas-26.2"
New-Item -ItemType Directory -Force -Path $sourceAtlasToolRoot | Out-Null
foreach ($item in @("README.md", "debug_faces.py", "gen_cpp_vendor_atlas.py")) {
    $src = Join-Path $scriptDir "tools\atlas-26.2\$item"
    if (Test-Path -LiteralPath $src -PathType Leaf) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $sourceAtlasToolRoot $item)
    }
}

Copy-Item -LiteralPath (Join-Path $scriptDir "README.md") -Destination (Join-Path $sourceRoot "README.md")
Copy-Item -LiteralPath (Join-Path $scriptDir "README.zh-CN.md") -Destination (Join-Path $sourceRoot "README.zh-CN.md")

Copy-Item -LiteralPath $buildGuiExe -Destination (Join-Path $releaseRoot "3dmodel-to-minecraft-gui.exe")
Copy-Item -LiteralPath $buildCliExe -Destination (Join-Path $releaseRoot "3dmodel-to-minecraft-cli.exe")
foreach ($dll in @("dstorage.dll", "dstoragecore.dll")) {
    $runtimeDll = Join-Path $scriptDir "third_party\directstorage\bin\x64\$dll"
    if (-not (Test-Path $runtimeDll)) {
        throw "Missing DirectStorage runtime: $runtimeDll"
    }
    Copy-Item -LiteralPath $runtimeDll -Destination (Join-Path $releaseRoot $dll)
}
Copy-Item -LiteralPath (Join-Path $scriptDir "vendor") -Destination (Join-Path $releaseRoot "vendor") -Recurse -Force
foreach ($item in @("README.md", "README.zh-CN.md", "VERSION.txt", "THIRD_PARTY_NOTICES.md")) {
    Copy-Item -LiteralPath (Join-Path $scriptDir $item) -Destination (Join-Path $releaseRoot $item)
}
Copy-Item -LiteralPath (Join-Path $scriptDir "third_party\directstorage\LICENSE.txt") -Destination (Join-Path $releaseRoot "DIRECTSTORAGE_LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $scriptDir "third_party\directstorage\LICENSE-CODE.txt") -Destination (Join-Path $releaseRoot "DIRECTSTORAGE_HEADERS_LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $scriptDir "third_party\directstorage\NOTICES.txt") -Destination (Join-Path $releaseRoot "DIRECTSTORAGE_NOTICES.txt")
Copy-Item -LiteralPath (Join-Path $scriptDir "third_party\vulkan_headers\LICENSE.md") -Destination (Join-Path $releaseRoot "VULKAN_HEADERS_LICENSE.md")

foreach ($zip in @($sourceZip, $releaseZip)) {
    if (Test-Path $zip) {
        Remove-Item -LiteralPath $zip -Force
    }
}
Compress-Archive -Path (Join-Path $sourceRoot "*") -DestinationPath $sourceZip -Force
Compress-Archive -Path (Join-Path $releaseRoot "*") -DestinationPath $releaseZip -Force

Write-Host "GitHub source folder: $sourceRoot"
Write-Host "GitHub source zip   : $sourceZip"
Write-Host "Release folder      : $releaseRoot"
Write-Host "Release zip         : $releaseZip"
