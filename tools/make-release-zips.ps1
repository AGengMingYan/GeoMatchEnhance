param(
    [string]$Version = '1.1.1',
    [switch]$SkipGui          # 只打 SDK 包，不重打 GUI 包
)

# 打包发布用的 zip（本机 .zip 会被加密落盘，所以输出名一律用 .zip1）
#   GeoGUIDemo-<ver>-win64.zip1   GUI 便携版（exe + dll + 示例数据 + 图片说明 + README）
#   GeoMatch-SDK-<ver>-win64.zip1  SDK（include + lib + bin + cmake + examples + SDK.md）
#   GeoMatch-SDK-<ver>-win32.zip1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$rel  = Join-Path $root 'release_GeoGUIDemo_x64'

function New-Zip1 {
    param([string]$SrcDir, [string]$ZipPath, [string]$TopName)
    # 本机 .zip 落盘即密文，所以输出名一律用 .zip1（下载方自己改回 .zip）。
    # 必须用 python 打：PowerShell / .NET 的 ZipFile::CreateFromDirectory 在本机
    # 读到的是源码的"加密态视图"，打出来的包里 .h/.cpp 是密文
    # （1.1.0 / 1.2.0 的 SDK 都中招过）。tools\make-zip1.py 读明文并自校验。
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { throw 'python not found: packaging needs tools\make-zip1.py' }
    & $py.Source (Join-Path $PSScriptRoot 'make-zip1.py') $SrcDir $ZipPath
    if ($LASTEXITCODE -ne 0) { throw "zip failed: $ZipPath" }
}

# ---------------- SDK 包 ----------------
foreach ($arch in 'win64','win32') {
    $src = Join-Path $rel "sdk\GeoMatch-$Version-$arch"
    if (-not (Test-Path $src)) { Write-Host "skip $arch (no $src)"; continue }
    Write-Host "=== SDK $arch ==="
    New-Zip1 -SrcDir $src -ZipPath (Join-Path $rel "GeoMatch-SDK-$Version-$arch.zip1") -TopName "GeoMatch-$Version-$arch"
}

# ---------------- GUI 包 ----------------
if (-not $SkipGui) {
    Write-Host "=== GUI win64 ==="
    $stage = Join-Path $rel "pkg_$Version"
    $dist  = Join-Path $stage "GeoGUIDemo-$Version"
    if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Path (Join-Path $dist 'case1') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dist '图片说明') -Force | Out-Null

    Copy-Item -LiteralPath (Join-Path $rel 'GeoGUIDemo.exe') -Destination $dist -Force
    foreach ($d in 'opencv_world455.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll',
                   'msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','vcruntime140.dll',
                   'vcruntime140_1.dll','concrt140.dll','vccorlib140.dll') {
        Copy-Item -LiteralPath (Join-Path $rel $d) -Destination $dist -Force -ErrorAction SilentlyContinue
    }
    Copy-Item -LiteralPath (Join-Path $rel 'README.txt') -Destination $dist -Force
    Copy-Item -LiteralPath (Join-Path $root 'case1\temp.jpg') -Destination (Join-Path $dist 'case1') -Force
    Copy-Item -LiteralPath (Join-Path $root 'case1\tmp_grab_1762994178.jpg') -Destination (Join-Path $dist 'case1') -Force
    Copy-Item -Path (Join-Path $root 'docs\img\*.png') -Destination (Join-Path $dist '图片说明') -Force

    # 快速上手：md -> txt，图片路径改指向 图片说明/
    $md = Join-Path $root 'docs\快速上手-三步测试Demo.md'
    if (Test-Path $md) {
        $t = [System.IO.File]::ReadAllText($md, [System.Text.Encoding]::UTF8)
        $t = $t -replace 'img/', '图片说明/'
        $enc = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText((Join-Path $dist '快速上手-三步测试Demo.txt'), $t, $enc)
    }

    # SDK 手册也放一份进 GUI 包，方便只想用库的人发现
    if (Test-Path (Join-Path $root 'docs\SDK.md')) {
        Copy-Item -LiteralPath (Join-Path $root 'docs\SDK.md') -Destination $dist -Force
    }

    New-Zip1 -SrcDir $dist -ZipPath (Join-Path $rel "GeoGUIDemo-$Version-win64.zip1") -TopName "GeoGUIDemo-$Version"
    Remove-Item -LiteralPath $stage -Recurse -Force
}
Write-Host "done"
