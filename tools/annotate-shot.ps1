# Crop + annotate a screenshot from a UTF-8 JSON description (this file stays
# pure ASCII so PowerShell 5.1 cannot mis-parse it as GBK).
# Usage: powershell -File _ai_tmp\annotate.ps1 -Spec _ai_tmp\spec_step0.json
param([Parameter(Mandatory=$true)][string]$Spec)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$json = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $Spec).Path,
                                      [System.Text.Encoding]::UTF8) |
        ConvertFrom-Json

$img = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $json.src).Path)

$cx = 0; $cy = 0; $cw = $img.Width; $ch = $img.Height
if ($json.crop) {
    $cx = [int]$json.crop.x; $cy = [int]$json.crop.y
    $cw = [int]$json.crop.w; $ch = [int]$json.crop.h
}
$scale = 1.0
if ($json.scale) { $scale = [double]$json.scale }
$padTop = 0
if ($json.padTop) { $padTop = [int]$json.padTop }
$padBot = 0
if ($json.padBottom) { $padBot = [int]$json.padBottom }
$ow = [int]($cw * $scale); $oh = [int]($ch * $scale) + $padTop + $padBot

$bmp = New-Object System.Drawing.Bitmap($ow, $oh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = 'HighQualityBicubic'
$g.PixelOffsetMode = 'HighQuality'
$g.Clear([System.Drawing.Color]::White)
$ih = $oh - $padTop - $padBot
$dstRect = New-Object System.Drawing.Rectangle(0, $padTop, $ow, $ih)
$srcRect = New-Object System.Drawing.Rectangle($cx, $cy, $cw, $ch)
$g.DrawImage($img, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)

function ToColor([string]$hex) {
    $hex = $hex.TrimStart('#')
    return [System.Drawing.Color]::FromArgb(
        [Convert]::ToInt32($hex.Substring(0, 2), 16),
        [Convert]::ToInt32($hex.Substring(2, 2), 16),
        [Convert]::ToInt32($hex.Substring(4, 2), 16))
}

function FontOf($spec, [int]$defaultSize) {
    $fam = 'Microsoft YaHei'
    if ($spec.font) { $fam = [string]$spec.font }
    $sz = $defaultSize
    if ($spec.size) { $sz = [int]$spec.size }
    $st = [System.Drawing.FontStyle]::Regular
    if ($spec.bold -eq $true) { $st = [System.Drawing.FontStyle]::Bold }
    return New-Object System.Drawing.Font($fam, $sz, $st, [System.Drawing.GraphicsUnit]::Pixel)
}

# ---- rectangles -----------------------------------------------------------
foreach ($r in $json.rects) {
    $col = ToColor($r.color)
    $lw = 3
    if ($r.lw) { $lw = [int]$r.lw }
    $pen = New-Object System.Drawing.Pen($col, $lw)
    $rx = [int](($r.x - $cx) * $scale); $ry = [int](($r.y - $cy) * $scale) + $padTop
    $rw = [int]($r.w * $scale); $rh = [int]($r.h * $scale)
    $g.DrawRectangle($pen, $rx, $ry, $rw, $rh)
    $pen.Dispose()

    if ($r.label) {
        $font = FontOf $r 20
        $txt = [string]$r.label
        $szf = $g.MeasureString($txt, $font)
        $pad = 6
        $bx = $rx - $pad
        $by = $ry - [int]$szf.Height - 2 * $pad
        if ($by -lt 0) { $by = $ry + $rh + 2 * $pad }
        $bw = [int]$szf.Width + 2 * $pad
        $bh = [int]$szf.Height + 2 * $pad
        if ($bx + $bw -gt $ow) { $bx = $ow - $bw - 2 }
        if ($bx -lt 0) { $bx = 2 }
        $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
        $fg = New-Object System.Drawing.SolidBrush($col)
        $bp = New-Object System.Drawing.Pen($col, 2)
        $g.FillRectangle($bg, $bx, $by, $bw, $bh)
        $g.DrawRectangle($bp, $bx, $by, $bw, $bh)
        $g.DrawString($txt, $font, $fg, [float]($bx + $pad), [float]($by + $pad))
        $bg.Dispose(); $fg.Dispose(); $bp.Dispose(); $font.Dispose()
    }
}

# ---- free text -----------------------------------------------------------
foreach ($t in $json.texts) {
    $col = ToColor($t.color)
    $font = FontOf $t 20
    $txt = [string]$t.text
    $szf = $g.MeasureString($txt, $font)
    $pad = 5
    if ($t.abs -eq $true) {
        $x = [int]$t.x; $y = [int]$t.y
    } else {
        $x = [int](($t.x - $cx) * $scale); $y = [int](($t.y - $cy) * $scale) + $padTop
    }
    if ($t.box -ne $false) {
        $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
        $g.FillRectangle($bg, $x, $y, [int]$szf.Width + 2 * $pad, [int]$szf.Height + 2 * $pad)
        $bg.Dispose()
    }
    $fg = New-Object System.Drawing.SolidBrush($col)
    $g.DrawString($txt, $font, $fg, [float]($x + $pad), [float]($y + $pad))
    $fg.Dispose(); $font.Dispose()
}

# ---- arrows --------------------------------------------------------------
foreach ($a in $json.arrows) {
    $col = ToColor($a.color)
    $lw = 3
    if ($a.lw) { $lw = [int]$a.lw }
    $pen = New-Object System.Drawing.Pen($col, $lw)
    $x1 = [int](($a.x1 - $cx) * $scale); $y1 = [int](($a.y1 - $cy) * $scale) + $padTop
    $x2 = [int](($a.x2 - $cx) * $scale); $y2 = [int](($a.y2 - $cy) * $scale) + $padTop
    $g.DrawLine($pen, $x1, $y1, $x2, $y2)
    $ang = [Math]::Atan2($y2 - $y1, $x2 - $x1)
    $hl = 16
    for ($k = -1; $k -le 1; $k += 2) {
        $hx = $x2 - $hl * [Math]::Cos($ang + $k * 0.45)
        $hy = $y2 - $hl * [Math]::Sin($ang + $k * 0.45)
        $g.DrawLine($pen, $x2, $y2, [int]$hx, [int]$hy)
    }
    $pen.Dispose()

    if ($a.label) {
        $font = FontOf $a 20
        $txt = [string]$a.label
        $szf = $g.MeasureString($txt, $font)
        $bx = $x1; $by = $y1 - [int]$szf.Height - 10
        if ($by -lt 0) { $by = 2 }
        if ($bx + $szf.Width -gt $ow) { $bx = $ow - [int]$szf.Width - 6 }
        $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
        $fg = New-Object System.Drawing.SolidBrush($col)
        $g.FillRectangle($bg, $bx, $by, [int]$szf.Width + 10, [int]$szf.Height + 8)
        $g.DrawString($txt, $font, $fg, [float]($bx + 5), [float]($by + 4))
        $bg.Dispose(); $fg.Dispose(); $font.Dispose()
    }
}

$g.Dispose(); $img.Dispose()

$dir = Split-Path -Parent ((Resolve-Path -LiteralPath $json.out -ErrorAction SilentlyContinue).Path)
if (-not $dir) {
    $dir = Split-Path -Parent (Join-Path (Get-Location).Path $json.out)
    $dir = (Resolve-Path -LiteralPath $dir).Path
}
$outPath = Join-Path $dir (Split-Path -Leaf $json.out)
$q = 90
if ($json.quality) { $q = [int]$json.quality }
if ($json.out -match '\.jpg$') {
    $enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
           Where-Object { $_.MimeType -eq 'image/jpeg' }
    $ps = New-Object System.Drawing.Imaging.EncoderParameters(1)
    $ps.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
        [System.Drawing.Imaging.Encoder]::Quality, [int64]$q)
    $bmp.Save($outPath, $enc, $ps)
} else {
    $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
}
$bmp.Dispose()
$b = [System.IO.File]::ReadAllBytes($outPath)
Write-Output ("{0} {1} bytes head={2}" -f $outPath, $b.Length,
              (($b[0..3] | ForEach-Object { $_.ToString('X2') }) -join ' '))

