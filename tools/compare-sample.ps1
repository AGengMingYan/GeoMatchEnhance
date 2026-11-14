# compare-sample.ps1 -- 本项目 vs Halcon 17 抽样对比（同模板、同参数）
#
# 四个匹配器：本项目 Shape / NCC x Halcon Shape / NCC，同一模板、同一批抽样的图、
# 尽量一致的参数（minScore / maxMatches / maxOverlap / 极性 / 亚像素 / 角度范围 / 尺度）。
#
# 用法：
#   # 重建双方模型（同模板、同角度范围）后，对 data1 等距抽样 60 张采集
#   powershell -ExecutionPolicy Bypass -File tools\compare-sample.ps1 -Sets data1 -Stride 2 -Take 60 -RebuildModels
#   # data2 全量
#   powershell -ExecutionPolicy Bypass -File tools\compare-sample.ps1 -Sets data2 -RebuildModels
#
# 依赖（不在仓库内的部分需自备）：dataset/data1|data2 图片、build/GeoMatchDemo.exe、
# build/NccDemo.exe、HalconDemo/halcon_shape_compare.exe、
# HalconDemo/halcon_ncc_compare.exe、HalconDemo/Halcon17（32 位 SDK）。
param(
    [string[]]$Sets = @('data1', 'data2'),
    [int]$Stride = 0,            # >0 = 等距抽样（每 Stride 张取 1 张）；0 = 全量
    [int]$Take = 0,              # >0 = 抽样后只取前 N 张
    [int]$Offset = 0,            # 抽样后跳过前 N 张（与 Take 配合做分片）
    [switch]$RebuildModels,      # 用同一模板重建双方四个模型
    [double]$MinScore = 0.5,
    [int]$MaxMatches = 1,
    [double]$MaxOverlap = 0.5,
    [string]$OutCsv = '',
    [switch]$Append
)
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
if ($OutCsv -eq '') { $OutCsv = Join-Path $root '_ai_tmp\sample.csv' }

# ---- Halcon 17（32 位）运行环境 -------------------------------------------
$env:PATH       = (Join-Path $root 'HalconDemo\Halcon17\bin') + ';' + $env:PATH
$env:HALCONARCH = 'x86sse2-win32'
$env:HALCONROOT = Join-Path $root 'HalconDemo\Halcon17'

$shapeExe = Join-Path $root 'build\GeoMatchDemo.exe'
$nccExe   = Join-Path $root 'build\NccDemo.exe'
$halShape = Join-Path $root 'HalconDemo\halcon_shape_compare.exe'
$halNcc   = Join-Path $root 'HalconDemo\halcon_ncc_compare.exe'
$t2out    = Join-Path $root '_ai_tmp\t2out'
New-Item -ItemType Directory -Force $t2out | Out-Null

# 每个数据集的模板 / 角度范围（与该数据集实测目标朝向一致）
$cfg = @{
    'data1' = @{ tpl = 'dataset\data1\template_pad.jpg'; aStart = -10;  aExt = 20;  dir = 'dataset\data1' }
    'data2' = @{ tpl = 'dataset\data2\template_via.jpg'; aStart = -100; aExt = 200; dir = 'dataset\data2' }
}

function ToD($s) {
    if ($null -eq $s) { return -1.0 }
    $v = 0.0
    if ([double]::TryParse($s, [ref]$v)) { return $v }
    return -1.0
}

foreach ($set in $Sets) {
    $c      = $cfg[$set]
    $tpl    = Join-Path $root $c.tpl
    $imgDir = Join-Path $root $c.dir
    $shm    = Join-Path $root "build\_$set.shm"
    $ncm    = Join-Path $root "build\_$set.ncm"
    $hshm   = Join-Path $root "HalconDemo\_$set.shm"
    $hncm   = Join-Path $root "HalconDemo\_$set.ncm"

    # ---- 重建模型（同模板、同角度范围、同尺度）---------------------------
    if ($RebuildModels) {
        Write-Host "---- rebuild models: $set  tpl=$($c.tpl) ----"
        & $shapeExe -t1 -t $tpl -save $shm -angle $c.aExt 2>&1 | Out-Null
        & $nccExe   -t1 -t $tpl -save $ncm -angle-start $c.aStart -angle $c.aExt 2>&1 | Out-Null
        & $halShape $tpl -angle-start $c.aStart -angle-extent $c.aExt -save-model $hshm 2>&1 | Out-Null
        & $halNcc   $tpl -angle-start $c.aStart -angle-extent $c.aExt -save-model $hncm 2>&1 | Out-Null
        foreach ($f in @($shm, $ncm, $hshm, $hncm)) {
            if (Test-Path $f) {
                $fi = Get-Item $f
                Write-Host ("   built {0,-24} {1,9} bytes" -f $fi.Name, $fi.Length)
            } else {
                Write-Host ("   MISSING {0}" -f $f)
            }
        }
    }

    # ---- 抽样 ------------------------------------------------------------
    $allImgs = @(Get-ChildItem $imgDir -Filter *.jpg |
                 Where-Object { $_.Name -notlike 'template_*' } |
                 Sort-Object Name)
    if ($Stride -gt 1) {
        $picked = New-Object System.Collections.Generic.List[object]
        for ($k = 0; $k -lt $allImgs.Count; $k += $Stride) { $picked.Add($allImgs[$k]) }
        $allImgs = $picked.ToArray()
    }
    if ($Offset -gt 0) { $allImgs = @($allImgs | Select-Object -Skip $Offset) }
    if ($Take -gt 0)   { $allImgs = @($allImgs | Select-Object -First $Take) }

    Write-Host ("==== {0}  sample={1} (stride={2} offset={3})  minscore={4} max={5} overlap={6} ====" -f `
        $set, $allImgs.Count, $Stride, $Offset, $MinScore, $MaxMatches, $MaxOverlap)


    $rows = New-Object System.Collections.Generic.List[object]
    $i = 0
    foreach ($im in $allImgs) {
        $i++; $p = $im.FullName; $nm = $im.Name
        $gN = 0;  $gS = -1.0; $gX = -1.0; $gY = -1.0; $gA = 0.0; $gMs = -1.0
        $nN = 0;  $nS = -1.0; $nX = -1.0; $nY = -1.0; $nA = 0.0; $nMs = -1.0
        $hN = 0;  $hS = -1.0; $hX = -1.0; $hY = -1.0; $hA = 0.0; $hMs = -1.0
        $kN = 0;  $kS = -1.0; $kX = -1.0; $kY = -1.0; $kA = 0.0; $kMs = -1.0

        # 1) 本项目 shape（skipWeight 默认 0.9 生效）
        $gOut = (& $shapeExe -t2 -load $shm -s $p -noshow -o $t2out -minscore $MinScore `
                 -max $MaxMatches -max-overlap $MaxOverlap 2>&1) -join "`n"
        $m = [regex]::Match($gOut, '\[t2\] matches\s*:\s*(\d+)')
        if ($m.Success) { $gN = [int]$m.Groups[1].Value }
        $m = [regex]::Match($gOut, 'preprocess\s+([\d.]+)\s*ms\s+search\s+([\d.]+)\s*ms')
        if ($m.Success) { $gMs = (ToD $m.Groups[1].Value) + (ToD $m.Groups[2].Value) }
        $m = [regex]::Match($gOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) {
            $gS = ToD $m.Groups[1].Value; $gX = ToD $m.Groups[2].Value
            $gY = ToD $m.Groups[3].Value; $gA = ToD $m.Groups[4].Value
        }

        # 2) 本项目 NCC
        $nOut = (& $nccExe -load $ncm -s $p -nodraw -minscore $MinScore -max $MaxMatches `
                 -overlap $MaxOverlap 2>&1) -join "`n"
        $nLine = ($nOut -split "`n" | Where-Object { $_ -match [regex]::Escape($nm) } | Select-Object -First 1)
        if ($nLine) {
            $m = [regex]::Match($nLine, '^\s*\S+\s+(\d+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)')
            if ($m.Success) {
                $nN = [int]$m.Groups[1].Value; $nS = ToD $m.Groups[2].Value
                $nX = ToD $m.Groups[3].Value; $nY = ToD $m.Groups[4].Value; $nA = ToD $m.Groups[5].Value
            }
        }
        $m = [regex]::Match($nOut, 'avg\s+([\d.]+)\s*ms/image')
        if ($m.Success) { $nMs = ToD $m.Groups[1].Value }

        # 3) Halcon shape
        $hOut = (& $halShape $tpl $p -load-model $hshm -angle-start $c.aStart -angle-extent $c.aExt `
                 -minscore $MinScore -max $MaxMatches -max-overlap $MaxOverlap 2>&1) -join "`n"
        $m = [regex]::Match($hOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $hN = [int]$m.Groups[1].Value; $hMs = ToD $m.Groups[2].Value }
        $m = [regex]::Match($hOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) {
            $hS = ToD $m.Groups[1].Value; $hX = ToD $m.Groups[2].Value
            $hY = ToD $m.Groups[3].Value; $hA = ToD $m.Groups[4].Value
        }

        # 4) Halcon NCC
        $kOut = (& $halNcc $tpl $p -load-model $hncm -angle-start $c.aStart -angle-extent $c.aExt `
                 -minscore $MinScore -max $MaxMatches -max-overlap $MaxOverlap 2>&1) -join "`n"
        $m = [regex]::Match($kOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $kN = [int]$m.Groups[1].Value; $kMs = ToD $m.Groups[2].Value }
        $m = [regex]::Match($kOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) {
            $kS = ToD $m.Groups[1].Value; $kX = ToD $m.Groups[2].Value
            $kY = ToD $m.Groups[3].Value; $kA = ToD $m.Groups[4].Value
        }

        $rows.Add([pscustomobject]@{
            set = $set; img = $nm
            gN = $gN; gScore = $gS; gX = $gX; gY = $gY; gA = $gA; gMs = $gMs
            nN = $nN; nScore = $nS; nX = $nX; nY = $nY; nA = $nA; nMs = $nMs
            hN = $hN; hScore = $hS; hX = $hX; hY = $hY; hA = $hA; hMs = $hMs
            kN = $kN; kScore = $kS; kX = $kX; kY = $kY; kA = $kA; kMs = $kMs
        })
        if ($i % 15 -eq 0 -or $i -eq $allImgs.Count) {
            Write-Host ("  [{0}/{1}] {2}  g={3} n={4} h={5} k={6}" -f `
                $i, $allImgs.Count, $nm, $gS, $nS, $hS, $kS)
        }
    }

    $doAppend = $Append -and (Test-Path $OutCsv)
    $rows | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8 -Append:$doAppend
    Write-Host ("wrote {0} rows (append={1}) -> {2}" -f $rows.Count, $doAppend, $OutCsv)
}

