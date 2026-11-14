# compare-test2-four.ps1 -- 四器对比采集（本项目 shape/ncc × Halcon17 shape/ncc）
#
# 用法（在仓库根目录执行）：
#   $env:PATH       = "$PWD\HalconDemo\Halcon17\bin;$env:PATH"
#   $env:HALCONARCH = 'x86sse2-win32'
#   $env:HALCONROOT = "$PWD\HalconDemo\Halcon17"
#
#   powershell -ExecutionPolicy Bypass -File tools\compare-test2-four.ps1 -Days 2024-05-07,2024-05-15 -Reset
#   powershell -ExecutionPolicy Bypass -File tools\compare-test2-four.ps1 -Days 2024-05-16,2024-05-20
#   powershell -ExecutionPolicy Bypass -File tools\compare-test2-four.ps1 -Days 2024-05-21
#
# 产物：_ai_tmp\cmp200.csv（每次追加；-Reset 先清空）
# 说明与结论见 docs\test2四器对比-进度-2026-11-14.md

param(
    [string[]]$Days = @('2024-05-07', '2024-05-15', '2024-05-16', '2024-05-20', '2024-05-21'),
    [string]$Csv = '',
    [switch]$Reset
)

$ErrorActionPreference = 'Continue'

# powershell -File 会把 "a,b" 当成单个字符串，这里统一拆开
$Days = @($Days | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })

$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$data  = Join-Path $root 'dataset\test2\imgLog'
$tpl   = Join-Path $root 'dataset\test2\template_diamond.jpg'
if ($Csv -eq '') { $Csv = Join-Path $root '_ai_tmp\cmp200.csv' }

$env:PATH       = (Join-Path $root 'HalconDemo\Halcon17\bin') + ';' + $env:PATH
$env:HALCONARCH = 'x86sse2-win32'
$env:HALCONROOT = Join-Path $root 'HalconDemo\Halcon17'

$shapeExe = Join-Path $build 'GeoMatchDemo.exe'
$nccExe   = Join-Path $build 'NccDemo.exe'
$halShape = Join-Path $root 'HalconDemo\halcon_shape_compare.exe'
$halNcc   = Join-Path $root 'HalconDemo\halcon_ncc_compare.exe'
$shm      = Join-Path $build '_diamond.shm'
$ncm      = Join-Path $build '_diamond.ncm'
$t2out    = Join-Path $root '_ai_tmp\t2out'

# 每日抽样数（40+20+45+45+50 = 200）；05-15 是唯一含目标的一天，20 张全部入选
$perDay = @{
    '2024-05-07' = 40
    '2024-05-15' = 20
    '2024-05-16' = 45
    '2024-05-20' = 45
    '2024-05-21' = 50
}

New-Item -ItemType Directory -Force $t2out | Out-Null
New-Item -ItemType Directory -Force (Split-Path $Csv -Parent) | Out-Null
if ($Reset -and (Test-Path $Csv)) { Remove-Item $Csv -Force }

function ToD($s) {
    if ($null -eq $s) { return -1.0 }
    $v = 0.0
    if ([double]::TryParse($s, [ref]$v)) { return $v }
    return -1.0
}

$rows = New-Object System.Collections.Generic.List[object]
$total = 0

foreach ($day in $Days) {
    $dayDir = Join-Path $data $day
    if (-not (Test-Path $dayDir)) { Write-Host "skip missing $day"; continue }
    $take = $perDay[$day]
    if (-not $take) { $take = 40 }
    $imgs = Get-ChildItem $dayDir -Filter *.jpg | Sort-Object Name | Select-Object -First $take
    Write-Host "=== $day  take $($imgs.Count) ==="

    foreach ($img in $imgs) {
        $total++
        $p = $img.FullName
        $nm = $img.Name

        # ---------------- 1) 本项目 shape (x64) - t2 模式 ----------------
        # 输出：[t2] matches : N / [t2] match : preprocess X ms search Y ms / #0 score=.. pos=(x;y) angle=..deg
        $gOut = (& $shapeExe -t2 -load $shm -s $p -noshow -o $t2out `
                     -minscore 0.3 -max 12 -max-overlap 1.0 2>&1) -join "`n"
        $gN = -1; $gMs = -1.0
        $gS = -1.0; $gX = -1.0; $gY = -1.0; $gA = 0.0
        $m = [regex]::Match($gOut, '\[t2\] matches\s*:\s*(\d+)')
        if ($m.Success) { $gN = [int]$m.Groups[1].Value }
        $m = [regex]::Match($gOut, '\[t2\] match\s*:\s*preprocess\s+([\d.]+)\s*ms\s+search\s+([\d.]+)\s*ms')
        if ($m.Success) { $gMs = (ToD $m.Groups[1].Value) + (ToD $m.Groups[2].Value) }
        $m = [regex]::Match($gOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) {
            $gS = ToD $m.Groups[1].Value; $gX = ToD $m.Groups[2].Value
            $gY = ToD $m.Groups[3].Value; $gA = ToD $m.Groups[4].Value
        }

        # ---------------- 2) 本项目 ncc (x64) ----------------
        # 输出：<name> <count> <score> <x> <y> <angle> ... ; avg X ms/image
        $nOut = (& $nccExe -load $ncm -s $p -nodraw `
                     -minscore 0.3 -max 12 -overlap 1.0 2>&1) -join "`n"
        $nN = -1; $nMs = -1.0
        $nS = -1.0; $nX = -1.0; $nY = -1.0; $nA = 0.0
        $nLine = ($nOut -split "`n" | Where-Object { $_ -match [regex]::Escape($nm) } | Select-Object -First 1)
        if ($nLine) {
            $m = [regex]::Match($nLine, '^\s*\S+\s+(\d+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)')
            if ($m.Success) {
                $nN = [int]$m.Groups[1].Value; $nS = ToD $m.Groups[2].Value
                $nX = ToD $m.Groups[3].Value; $nY = ToD $m.Groups[4].Value; $nA = ToD $m.Groups[5].Value
            } else {
                $m = [regex]::Match($nLine, '^\s*\S+\s+(\d+)\s')
                if ($m.Success) { $nN = [int]$m.Groups[1].Value }
            }
        }
        $m = [regex]::Match($nOut, 'avg\s+([\d.]+)\s*ms/image')
        if ($m.Success) { $nMs = ToD $m.Groups[1].Value }

        # ---------------- 3) Halcon shape (x86 sse2) ----------------
        # 输出：<file>  matches=N  search=X ms / #0 score=.. pos=(x;y) angle=..deg
        $hOut = (& $halShape $tpl $p -angle-start -10 -angle-extent 20 `
                     -minscore 0.3 -max 12 -max-overlap 1.0 2>&1) -join "`n"
        $hN = -1; $hMs = -1.0
        $hS = -1.0; $hX = -1.0; $hY = -1.0; $hA = 0.0
        $m = [regex]::Match($hOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $hN = [int]$m.Groups[1].Value; $hMs = ToD $m.Groups[2].Value }
        $m = [regex]::Match($hOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) {
            $hS = ToD $m.Groups[1].Value; $hX = ToD $m.Groups[2].Value
            $hY = ToD $m.Groups[3].Value; $hA = ToD $m.Groups[4].Value
        }

        # ---------------- 4) Halcon ncc (x86 sse2) ----------------
        $kOut = (& $halNcc $tpl $p -angle-start -10 -angle-extent 20 `
                     -minscore 0.3 -max 12 -max-overlap 1.0 2>&1) -join "`n"
        $kN = -1; $kMs = -1.0
        $kS = -1.0; $kX = -1.0; $kY = -1.0; $kA = 0.0
        $m = [regex]::Match($kOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $kN = [int]$m.Groups[1].Value; $kMs = ToD $m.Groups[2].Value }
        $m = [regex]::Match($kOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) {
            $kS = ToD $m.Groups[1].Value; $kX = ToD $m.Groups[2].Value
            $kY = ToD $m.Groups[3].Value; $kA = ToD $m.Groups[4].Value
        }

        $rows.Add([pscustomobject]@{
            day = $day; img = $nm
            gN = $gN; gScore = $gS; gX = $gX; gY = $gY; gA = $gA; gMs = $gMs
            nN = $nN; nScore = $nS; nX = $nX; nY = $nY; nA = $nA; nMs = $nMs
            hN = $hN; hScore = $hS; hX = $hX; hY = $hY; hA = $hA; hMs = $hMs
            kN = $kN; kScore = $kS; kX = $kX; kY = $kY; kA = $kA; kMs = $kMs
        })

        Write-Host ("  [{0}] {1}  gS={2} hS={3} nS={4} kS={5}" -f $total, $nm, $gS, $hS, $nS, $kS)
    }
}

if ($rows.Count -gt 0) {
    $append = (-not $Reset) -and (Test-Path $Csv)
    $rows | Export-Csv -Path $Csv -NoTypeInformation -Encoding UTF8 -Append:$append
}
Write-Host "rows written: $($rows.Count)  -> $Csv"
