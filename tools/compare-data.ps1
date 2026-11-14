# compare-data.ps1 -- data1/data2 四器对比采集
# 每数据集参数：
#   data1: 模板 template_pad.jpg, 角度 ±10°（实测目标全在 ±1.1°）
#   data2: 模板 template_via.jpg, 角度 ±100°（实测双峰 0°/-90°）
param(
    [string[]]$Sets = @('data1', 'data2'),
    [string]$OutDir = '',
    [int]$Offset = 0,
    [int]$Take = 0,
    [switch]$Append
)
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
if ($OutDir -eq '') { $OutDir = Join-Path $root '_ai_tmp' }

$env:PATH       = (Join-Path $root 'HalconDemo\Halcon17\bin') + ';' + $env:PATH
$env:HALCONARCH = 'x86sse2-win32'
$env:HALCONROOT = Join-Path $root 'HalconDemo\Halcon17'

$shapeExe = Join-Path $root 'build\GeoMatchDemo.exe'
$nccExe   = Join-Path $root 'build\NccDemo.exe'
$halShape = Join-Path $root 'HalconDemo\halcon_shape_compare.exe'
$halNcc   = Join-Path $root 'HalconDemo\halcon_ncc_compare.exe'
$t2out    = Join-Path $root '_ai_tmp\t2out'
New-Item -ItemType Directory -Force $t2out | Out-Null

# 每数据集的模板/模型/角度配置
$cfg = @{
    'data1' = @{ tpl='dataset\data1\template_pad.jpg'; aStart=-10;  aExt=20;  dir='dataset\data1' }
    'data2' = @{ tpl='dataset\data2\template_via.jpg'; aStart=-100; aExt=200; dir='dataset\data2' }
}
$MIN = 0.5; $MAX = 1; $OVL = 0.5

function ToD($s) { if ($null -eq $s) { return -1.0 }; $v = 0.0; if ([double]::TryParse($s,[ref]$v)) { return $v }; return -1.0 }

foreach ($set in $Sets) {
    $c = $cfg[$set]
    $tpl = Join-Path $root $c.tpl
    $imgDir = Join-Path $root $c.dir
    $shm = Join-Path $root "build\_$set.shm"
    $ncm = Join-Path $root "build\_$set.ncm"
    $hshm = Join-Path $root "HalconDemo\_$set.shm"
    $hncm = Join-Path $root "HalconDemo\_$set.ncm"
    $csv = Join-Path $OutDir "$set.csv"
    if (-not $Append -and (Test-Path $csv)) { Remove-Item $csv -Force }

    $allImgs = @(Get-ChildItem $imgDir -Filter *.jpg | Where-Object { $_.Name -notlike 'template_*' } | Sort-Object Name)
    if ($Take -gt 0) { $imgs = @($allImgs | Select-Object -Skip $Offset -First $Take) }
    else { $imgs = $allImgs }
    Write-Host "================ $set  images=$($imgs.Count)/$($allImgs.Count) (offset=$Offset)  angle $($c.aStart)..$($c.aStart+$c.aExt) ================"
    $rows = New-Object System.Collections.Generic.List[object]
    $i = 0
    foreach ($im in $imgs) {
        $i++; $p = $im.FullName; $nm = $im.Name
        $gN=0;$gS=-1.0;$gX=-1.0;$gY=-1.0;$gA=0.0;$gMs=-1.0
        $nN=0;$nS=-1.0;$nX=-1.0;$nY=-1.0;$nA=0.0;$nMs=-1.0
        $hN=0;$hS=-1.0;$hX=-1.0;$hY=-1.0;$hA=0.0;$hMs=-1.0
        $kN=0;$kS=-1.0;$kX=-1.0;$kY=-1.0;$kA=0.0;$kMs=-1.0

        # 1) ours shape (t2)
        $gOut = (& $shapeExe -t2 -load $shm -s $p -noshow -o $t2out -minscore $MIN -max $MAX -max-overlap $OVL 2>&1) -join "`n"
        $m = [regex]::Match($gOut, '\[t2\] matches\s*:\s*(\d+)')
        if ($m.Success) { $gN = [int]$m.Groups[1].Value }
        $m = [regex]::Match($gOut, 'preprocess\s+([\d.]+)\s*ms\s+search\s+([\d.]+)\s*ms')
        if ($m.Success) { $gMs = (ToD $m.Groups[1].Value) + (ToD $m.Groups[2].Value) }
        $m = [regex]::Match($gOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) { $gS=ToD $m.Groups[1].Value;$gX=ToD $m.Groups[2].Value;$gY=ToD $m.Groups[3].Value;$gA=ToD $m.Groups[4].Value }

        # 2) ours ncc
        $nOut = (& $nccExe -load $ncm -s $p -nodraw -minscore $MIN -max $MAX -overlap $OVL 2>&1) -join "`n"
        $nLine = ($nOut -split "`n" | Where-Object { $_ -match [regex]::Escape($nm) } | Select-Object -First 1)
        if ($nLine) {
            $m = [regex]::Match($nLine, '^\s*\S+\s+(\d+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)')
            if ($m.Success) { $nN=[int]$m.Groups[1].Value;$nS=ToD $m.Groups[2].Value;$nX=ToD $m.Groups[3].Value;$nY=ToD $m.Groups[4].Value;$nA=ToD $m.Groups[5].Value }
        }
        $m = [regex]::Match($nOut, 'avg\s+([\d.]+)\s*ms/image')
        if ($m.Success) { $nMs = ToD $m.Groups[1].Value }

        # 3) halcon shape
        $hOut = (& $halShape $tpl $p -load-model $hshm -angle-start $c.aStart -angle-extent $c.aExt -minscore $MIN -max $MAX -max-overlap $OVL 2>&1) -join "`n"
        $m = [regex]::Match($hOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $hN=[int]$m.Groups[1].Value;$hMs=ToD $m.Groups[2].Value }
        $m = [regex]::Match($hOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) { $hS=ToD $m.Groups[1].Value;$hX=ToD $m.Groups[2].Value;$hY=ToD $m.Groups[3].Value;$hA=ToD $m.Groups[4].Value }

        # 4) halcon ncc
        $kOut = (& $halNcc $tpl $p -load-model $hncm -angle-start $c.aStart -angle-extent $c.aExt -minscore $MIN -max $MAX -max-overlap $OVL 2>&1) -join "`n"
        $m = [regex]::Match($kOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $kN=[int]$m.Groups[1].Value;$kMs=ToD $m.Groups[2].Value }
        $m = [regex]::Match($kOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) { $kS=ToD $m.Groups[1].Value;$kX=ToD $m.Groups[2].Value;$kY=ToD $m.Groups[3].Value;$kA=ToD $m.Groups[4].Value }

        $rows.Add([pscustomobject]@{
            set=$set; img=$nm
            gN=$gN;gScore=$gS;gX=$gX;gY=$gY;gA=$gA;gMs=$gMs
            nN=$nN;nScore=$nS;nX=$nX;nY=$nY;nA=$nA;nMs=$nMs
            hN=$hN;hScore=$hS;hX=$hX;hY=$hY;hA=$hA;hMs=$hMs
            kN=$kN;kScore=$kS;kX=$kX;kY=$kY;kA=$kA;kMs=$kMs
        })
        if ($i % 20 -eq 0 -or $i -eq $imgs.Count) { Write-Host ("  [{0}/{1}] {2}  g={3} n={4} h={5} k={6}" -f $i,$imgs.Count,$nm,$gS,$nS,$hS,$kS) }
    }
    $doAppend = $Append -and (Test-Path $csv)
    $rows | Export-Csv -Path $csv -NoTypeInformation -Encoding UTF8 -Append:$doAppend
    Write-Host "wrote $($rows.Count) rows (append=$doAppend) -> $csv`n"
}
