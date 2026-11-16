# compare-dataset.ps1 -- four-matcher comparison on any template + image folder
#
# Runs, for every image in <Dir>:
#   1) this project Shape   (build\GeoMatchDemo.exe  -t2 -load <set>.shm)
#   2) this project NCC     (build\NccDemo.exe      -load <set>.ncm)
#   3) Halcon 17 Shape      (HalconDemo\halcon_shape_compare.exe, x86)
#   4) Halcon 17 NCC        (HalconDemo\halcon_ncc_compare.exe,   x86)
# with the same template / angle range / scale range / thresholds, and writes
# one CSV row per image (score, position, angle, scale, milliseconds).
#
# Example:
#   powershell -ExecutionPolicy Bypass -File tools\compare-dataset.ps1 `
#       -Set data1 -Template dataset\sample\data1\template_pad.jpg `
#       -Dir dataset\sample\data1 -AngleStart -10 -AngleExtent 20 `
#       -RebuildModels -OutCsv _ai_tmp\ds_data1.csv
#
# Notes:
#   * CLI contrast units: ours uses Sobel units (-contrast/-hi), Halcon uses
#     grey levels (-contrast/-min-contrast); GUI grey * 4 == ours Sobel.
#   * -RebuildModels creates the four models; without it the models must exist.
#   * the script never writes outside build\, HalconDemo\, _ai_tmp\.
param(
    [Parameter(Mandatory = $true)][string]$Set,
    [Parameter(Mandatory = $true)][string]$Template,
    [Parameter(Mandatory = $true)][string]$Dir,
    [double]$AngleStart  = -10,
    [double]$AngleExtent = 20,
    [double]$ScaleMin    = 1.0,
    [double]$ScaleMax    = 1.0,
    [double]$MinScore    = 0.5,
    [int]   $MaxMatches  = 1,
    [double]$MaxOverlap  = 0.5,
    [int]   $Offset      = 0,
    [int]   $Take        = 0,
    [string]$OutCsv      = '',
    [switch]$Append,
    [switch]$RebuildModels,
    [double]$OursContrast      = 120,
    [double]$OursHi            = 160,
    [double]$HalconContrast    = 40,
    [double]$HalconMinContrast = 30,
    [switch]$NoHalcon,
    [switch]$NoOursNcc
)
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
if ($OutCsv -eq '') { $OutCsv = Join-Path $root ("_ai_tmp\ds_{0}.csv" -f $Set) }

# ---- Halcon 17 (32-bit) runtime env ------------------------------------------
$env:PATH       = (Join-Path $root 'HalconDemo\Halcon17\bin') + ';' + $env:PATH
$env:HALCONARCH = 'x86sse2-win32'
$env:HALCONROOT = Join-Path $root 'HalconDemo\Halcon17'

$shapeExe = Join-Path $root 'build\GeoMatchDemo.exe'
$nccExe   = Join-Path $root 'build\NccDemo.exe'
$halShape = Join-Path $root 'HalconDemo\halcon_shape_compare.exe'
$halNcc   = Join-Path $root 'HalconDemo\halcon_ncc_compare.exe'
$t2out    = Join-Path $root '_ai_tmp\t2out'
New-Item -ItemType Directory -Force $t2out | Out-Null

$tpl    = if ([System.IO.Path]::IsPathRooted($Template)) { $Template } else { Join-Path $root $Template }
$imgDir = if ([System.IO.Path]::IsPathRooted($Dir))      { $Dir }      else { Join-Path $root $Dir }
$shm    = Join-Path $root ("build\_ds_{0}.shm" -f $Set)
$ncm    = Join-Path $root ("build\_ds_{0}.ncm" -f $Set)
$hshm   = Join-Path $root ("HalconDemo\_ds_{0}.shm" -f $Set)
$hncm   = Join-Path $root ("HalconDemo\_ds_{0}.ncm" -f $Set)

function ToD($s) {
    if ($null -eq $s) { return -1.0 }
    $v = 0.0
    if ([double]::TryParse([string]$s, [ref]$v)) { return $v }
    return -1.0
}

Write-Host ("==== set={0}  tpl={1}  dir={2}" -f $Set, $Template, $Dir)
Write-Host ("     angle {0}..{1} deg   scale {2}..{3}   minScore {4}  max {5}  overlap {6}" -f `
            $AngleStart, ($AngleStart + $AngleExtent), $ScaleMin, $ScaleMax, $MinScore, $MaxMatches, $MaxOverlap)

# ---- models ------------------------------------------------------------------
if ($RebuildModels) {
    $log   = Join-Path $root ("_ai_tmp\ds_{0}_build.log" -f $Set)
    $lines = New-Object System.Collections.Generic.List[string]

    $o = (& $shapeExe -t1 -t $tpl -save $shm -angle $AngleExtent -smin $ScaleMin -smax $ScaleMax `
                       -contrast $OursContrast -hi $OursHi -noshow 2>&1) -join "`n"
    $lines.Add("[ours shape] exit=$LASTEXITCODE`n$o")

    if (-not $NoOursNcc) {
        $o = (& $nccExe -t1 -t $tpl -save $ncm -angle-start $AngleStart -angle $AngleExtent 2>&1) -join "`n"
        $lines.Add("[ours ncc] exit=$LASTEXITCODE`n$o")
    }
    if (-not $NoHalcon) {
        $o = (& $halShape $tpl -save-model $hshm -angle-start $AngleStart -angle-extent $AngleExtent `
                              -smin $ScaleMin -smax $ScaleMax -contrast $HalconContrast `
                              -min-contrast $HalconMinContrast 2>&1) -join "`n"
        $lines.Add("[halcon shape] exit=$LASTEXITCODE`n$o")
        $o = (& $halNcc $tpl -save-model $hncm -angle-start $AngleStart -angle-extent $AngleExtent 2>&1) -join "`n"
        $lines.Add("[halcon ncc] exit=$LASTEXITCODE`n$o")
    }
    [System.IO.File]::WriteAllLines($log, $lines, (New-Object System.Text.UTF8Encoding($false)))

    $want = @($shm)
    if (-not $NoOursNcc) { $want += $ncm }
    if (-not $NoHalcon)  { $want += $hshm; $want += $hncm }
    foreach ($f in $want) {
        if (Test-Path $f) {
            Write-Host ("   model {0,-18} {1,10} bytes" -f (Split-Path $f -Leaf), (Get-Item $f).Length)
        } else {
            Write-Host ("   model {0,-18} MISSING (see {1})" -f (Split-Path $f -Leaf), $log)
        }
    }
}

# ---- image list --------------------------------------------------------------
if (Test-Path -LiteralPath $imgDir -PathType Leaf) {
    $allImgs = @(Get-Item -LiteralPath $imgDir)
} else {
    $allImgs = @(Get-ChildItem $imgDir -File |
                 Where-Object { $_.Extension -match '^\.(jpg|jpeg|png|bmp|tif|tiff)$' -and $_.BaseName -notlike 'template*' } |
                 Sort-Object Name)
}
if ($Take -gt 0) { $imgs = @($allImgs | Select-Object -Skip $Offset -First $Take) }
else             { $imgs = $allImgs }
Write-Host ("     images {0}/{1} (offset {2})" -f $imgs.Count, $allImgs.Count, $Offset)

$rows = New-Object System.Collections.Generic.List[object]
$i = 0
foreach ($im in $imgs) {
    $i++
    $p  = $im.FullName
    $nm = $im.Name

    $gN = 0; $gS = -1.0; $gX = -1.0; $gY = -1.0; $gA = 0.0; $gK = -1.0; $gMs = -1.0
    $nN = 0; $nS = -1.0; $nX = -1.0; $nY = -1.0; $nA = 0.0; $nK = -1.0; $nMs = -1.0
    $hN = 0; $hS = -1.0; $hX = -1.0; $hY = -1.0; $hA = 0.0; $hK = -1.0; $hMs = -1.0
    $kN = 0; $kS = -1.0; $kX = -1.0; $kY = -1.0; $kA = 0.0; $kMs = -1.0

    # 1) ours shape
    $gOut = (& $shapeExe -t2 -load $shm -s $p -noshow -o $t2out -minscore $MinScore `
                         -max $MaxMatches -max-overlap $MaxOverlap 2>&1) -join "`n"
    $m = [regex]::Match($gOut, '\[t2\] matches\s*:\s*(\d+)')
    if ($m.Success) { $gN = [int]$m.Groups[1].Value }
    $m = [regex]::Match($gOut, 'preprocess\s+([\d.]+)\s*ms\s+search\s+([\d.]+)\s*ms')
    if ($m.Success) { $gMs = (ToD $m.Groups[1].Value) + (ToD $m.Groups[2].Value) }
    $m = [regex]::Match($gOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg\s+scale=([\d.\-]+)')
    if ($m.Success) {
        $gS = ToD $m.Groups[1].Value; $gX = ToD $m.Groups[2].Value
        $gY = ToD $m.Groups[3].Value; $gA = ToD $m.Groups[4].Value; $gK = ToD $m.Groups[5].Value
    }

    # 2) ours ncc
    if (-not $NoOursNcc) {
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
    }

    if (-not $NoHalcon) {
        # 3) halcon shape
        $hOut = (& $halShape $tpl $p -load-model $hshm -angle-start $AngleStart -angle-extent $AngleExtent `
                                -smin $ScaleMin -smax $ScaleMax -minscore $MinScore -max $MaxMatches `
                                -max-overlap $MaxOverlap 2>&1) -join "`n"
        $m = [regex]::Match($hOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $hN = [int]$m.Groups[1].Value; $hMs = ToD $m.Groups[2].Value }
        $m = [regex]::Match($hOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg\s+scale=([\d.\-]+)')
        if ($m.Success) {
            $hS = ToD $m.Groups[1].Value; $hX = ToD $m.Groups[2].Value
            $hY = ToD $m.Groups[3].Value; $hA = ToD $m.Groups[4].Value; $hK = ToD $m.Groups[5].Value
        }

        # 4) halcon ncc
        $kOut = (& $halNcc $tpl $p -load-model $hncm -angle-start $AngleStart -angle-extent $AngleExtent `
                                -minscore $MinScore -max $MaxMatches -max-overlap $MaxOverlap 2>&1) -join "`n"
        $m = [regex]::Match($kOut, 'matches=(\d+)\s+search=([\d.]+)\s*ms')
        if ($m.Success) { $kN = [int]$m.Groups[1].Value; $kMs = ToD $m.Groups[2].Value }
        $m = [regex]::Match($kOut, '#0\s+score=([\d.\-]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
        if ($m.Success) {
            $kS = ToD $m.Groups[1].Value; $kX = ToD $m.Groups[2].Value
            $kY = ToD $m.Groups[3].Value; $kA = ToD $m.Groups[4].Value
        }
    }

    $rows.Add([pscustomobject]@{
        set = $Set; img = $nm
        gN = $gN; gScore = $gS; gX = $gX; gY = $gY; gA = $gA; gK = $gK; gMs = $gMs
        nN = $nN; nScore = $nS; nX = $nX; nY = $nY; nA = $nA; nK = $nK; nMs = $nMs
        hN = $hN; hScore = $hS; hX = $hX; hY = $hY; hA = $hA; hK = $hK; hMs = $hMs
        kN = $kN; kScore = $kS; kX = $kX; kY = $kY; kA = $kA; kMs = $kMs
    })

    if ($i % 10 -eq 0 -or $i -eq $imgs.Count) {
        Write-Host ("  [{0}/{1}] {2}  g={3} n={4} h={5} k={6}" -f `
            $i, $imgs.Count, $nm, $gS, $nS, $hS, $kS)
    }
}

$doAppend = $Append -and (Test-Path $OutCsv)
$rows | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8 -Append:$doAppend
Write-Host ("wrote {0} rows (append={1}) -> {2}" -f $rows.Count, $doAppend, $OutCsv)



