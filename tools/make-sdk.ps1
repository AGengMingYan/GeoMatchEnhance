param(
    [string]$Version = '1.1.1',
    [switch]$SkipBuild
)

# 生成 32/64 位两套 SDK 目录（include + lib + bin + cmake 包配置 + SDK.md）
# 输出: release_GeoGUIDemo_x64/sdk/GeoMatch-<ver>-win64/ 与 ...-win32/
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outBase = Join-Path $root 'release_GeoGUIDemo_x64\sdk'
if (-not (Test-Path $outBase)) { New-Item -ItemType Directory -Path $outBase -Force | Out-Null }

# pick the first existing x64 OpenCV install; x86 uses the in-repo OpenCV455_MSVC32
$ocv64 = @('D:/develop2/OpenCV455/opencv/build', 'D:/Develop/OpenCV455/opencv/build') |
         Where-Object { Test-Path (Join-Path $_ 'x64/vc15/lib/opencv_world455.lib') } |
         Select-Object -First 1
$archs = @(
    @{ Name = 'win64'; Build = 'build'; BuildScript = 'tools\build-x64.bat';
       Dll = (Join-Path $ocv64 'x64/vc15/bin/opencv_world455.dll');
       Lib = (Join-Path $ocv64 'x64/vc15/lib/opencv_world455.lib') },
    @{ Name = 'win32'; Build = 'build_x86'; BuildScript = 'tools\build-x86.bat';
       Dll = (Join-Path $root 'OpenCV455_MSVC32\opencv_world455.dll');
       Lib = (Join-Path $root 'OpenCV455_MSVC32\opencv_world455.lib') }
)

foreach ($a in $archs) {
    if (-not (Test-Path $a.Lib)) { throw "OpenCV lib not found: $($a.Lib)" }
    Write-Host "=== $($a.Name) ==="
    if (-not $SkipBuild) {
        cmd /c "$($a.BuildScript) > `"$root\_ai_tmp\sdk_build_$($a.Name).log`" 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "build failed for $($a.Name)" }
    }

    $sdk = Join-Path $outBase ("GeoMatch-$Version-$($a.Name)")
    if (Test-Path $sdk) { Remove-Item -LiteralPath $sdk -Recurse -Force }

    Push-Location (Join-Path $root $a.Build)
    cmd /c "cmake --install . --prefix `"$sdk`" --config Release > `"$root\_ai_tmp\sdk_inst_$($a.Name).log`" 2>&1"
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { throw "install failed for $($a.Name)" }

    # 依赖：OpenCV 的 lib + dll（静态库需要 lib，运行时需要 dll）
    New-Item -ItemType Directory -Path (Join-Path $sdk 'lib') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $sdk 'bin') -Force | Out-Null
    Copy-Item -LiteralPath $a.Lib -Destination (Join-Path $sdk 'lib') -Force
    Copy-Item -LiteralPath $a.Dll -Destination (Join-Path $sdk 'bin') -Force

    # VC 运行时（部署 SDK 的示例程序时用）
    foreach ($d in 'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','concrt140.dll') {
        $cand = Join-Path $root "release_GeoGUIDemo_x64\$d"
        if (Test-Path $cand) { Copy-Item -LiteralPath $cand -Destination (Join-Path $sdk 'bin') -Force }
    }

    # 文档 + 示例（示例源码给用户当模板）
    if (Test-Path (Join-Path $root 'docs\SDK.md')) {
        Copy-Item -LiteralPath (Join-Path $root 'docs\SDK.md') -Destination $sdk -Force
    }
    $exDir = Join-Path $sdk 'examples'
    New-Item -ItemType Directory -Path $exDir -Force | Out-Null
    Copy-Item -Path (Join-Path $root 'examples\*.cpp') -Destination $exDir -Force -ErrorAction SilentlyContinue

    $files = Get-ChildItem -LiteralPath $sdk -Recurse -File
    $total = ($files | Measure-Object -Property Length -Sum).Sum
    Write-Host ("  -> {0}  ({1} files, {2:N1} MB)" -f $sdk, $files.Count, ($total / 1MB))
}
Write-Host "done"
