# test2 NCC batch compare: GeoMatch NccDemo (x64) vs Halcon 17 NCC (x86)
$ErrorActionPreference = 'Continue'
$root   = 'd:\work\gitwork\geo-match-enhance'
$build  = Join-Path $root 'build'
$nccExe = Join-Path $build 'NccDemo.exe'
$halExe = Join-Path $root 'HalconDemo\halcon_ncc_compare.exe'
$tpl    = Join-Path $root 'dataset\test2\template_diamond.jpg'
$ncm    = Join-Path $build '_diamond.ncm'
$data   = Join-Path $root 'dataset\test2\imgLog'
$outCsv = Join-Path $root '_ai_tmp\compare_test2_ncc.csv'

$env:PATH = (Join-Path $root 'HalconDemo\Halcon17\bin') + ';' + $env:PATH
$env:HALCONARCH = 'x86sse2-win32'
$env:HALCONROOT = Join-Path $root 'HalconDemo\Halcon17'

$perDay = 15
$imgs = Get-ChildItem $data -Directory | Sort-Object Name | ForEach-Object {
    Get-ChildItem $_.FullName -Filter *.jpg | Sort-Object Name | Select-Object -First $perDay
}
Write-Host ("NCC images to compare: " + $imgs.Count)

# NccDemo output rows look like:
#   <name>  <count>  <ncc> <col> <row> <angle>
# and the final summary line carries "avg X ms/image".
$rows = New-Object System.Collections.Generic.List[object]
foreach ($img in $imgs) {
    $day  = Split-Path (Split-Path $img.FullName -Parent) -Leaf
    $name = $img.Name

    # ---- GeoMatch NCC (x64) ----
    $gOut = & $nccExe -t2 -load $ncm -s $img.FullName -nodraw `
                -minscore 0.3 -max 12 -overlap 1.0 2>&1 | Out-String
    $gLine = ($gOut -split "`n" | Where-Object { $_ -match [regex]::Escape($name) } | Select-Object -First 1)
    $gN = 0; $gScore=-1; $gX=-1; $gY=-1; $gA=0
    if ($gLine -and $gLine -match '^\s*\S+\s+(\d+)\s+([\d.]+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)') {
        $gN=[int]$Matches[1]; $gScore=[double]$Matches[2]; $gX=[double]$Matches[3]
        $gY=[double]$Matches[4]; $gA=[double]$Matches[5]
    }
    $gAvg = [regex]::Match($gOut, 'avg\s+([\d.]+)\s*ms/image')
    $gMs = if ($gAvg.Success) { [double]$gAvg.Groups[1].Value } else { -1 }

    # ---- Halcon NCC (x86) ----
    $hOut = & $halExe $tpl $img.FullName -angle-start -10 -angle-extent 20 `
                -minscore 0.3 -max 12 -max-overlap 1.0 2>&1 | Out-String
    $hN = ([regex]::Matches($hOut, '#\d+\s+score=')).Count
    $hTime = [regex]::Match($hOut, 'search=([\d.]+)\s*ms')
    $hMs = if ($hTime.Success) { [double]$hTime.Groups[1].Value } else { -1 }
    $h0 = [regex]::Match($hOut, '#0\s+score=([\d.]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
    if ($hN -eq 0) { $hScore=-1; $hX=-1; $hY=-1; $hA=0 }
    else {
        $hScore=[double]$h0.Groups[1].Value; $hX=[double]$h0.Groups[2].Value
        $hY=[double]$h0.Groups[3].Value; $hA=[double]$h0.Groups[4].Value
    }

    $posDiff = if ($gX -ge 0 -and $hX -ge 0) { [math]::Round([math]::Sqrt(($gX-$hX)*($gX-$hX)+($gY-$hY)*($gY-$hY)), 3) } else { -1 }
    $rows.Add([pscustomobject]@{
        day=$day; img=$name;
        gN=$gN; gScore=$gScore; gX=$gX; gY=$gY; gA=$gA; gMs=$gMs;
        hN=$hN; hScore=$hScore; hX=$hX; hY=$hY; hA=$hA; hMs=$hMs;
        dScore=[math]::Round($hScore-$gScore,4); dAngle=[math]::Round($hA-$gA,3); posDiff=$posDiff
    })
}
$rows | Export-Csv -Path $outCsv -NoTypeInformation -Encoding UTF8
Write-Host ("CSV -> " + $outCsv)

$valid = $rows | Where-Object { $_.gScore -ge 0 -and $_.hScore -ge 0 }
$hi = $valid | Where-Object { $_.gScore -ge 0.7 -and $_.hScore -ge 0.7 }
function Std($a) {
    $m = ($a | Measure-Object -Average).Average
    $v = ($a | ForEach-Object { ($_ - $m) * ($_ - $m) } | Measure-Object -Average).Average
    [math]::Sqrt($v)
}
Write-Host ""
Write-Host "===== NCC SUMMARY: total=$($rows.Count), both-match=$($valid.Count), high(>=0.7 both)=$($hi.Count) ====="
Write-Host ("hit@0.7  GeoNCC: {0}/{1}   HalconNCC: {2}/{3}" -f ($rows|Where-Object{$_.gScore -ge 0.7}).Count, $rows.Count, ($rows|Where-Object{$_.hScore -ge 0.7}).Count, $rows.Count)
if ($hi.Count -gt 0) {
    $gm = $hi.gScore | Measure-Object -Average -Minimum -Maximum
    $hm = $hi.hScore | Measure-Object -Average -Minimum -Maximum
    Write-Host "--- HIGH-CONFIDENCE (>=0.7 both) ---"
    Write-Host ("GeoNCC  score: mean={0:N4} min={1:N4} max={2:N4} std={3:N4}" -f $gm.Average,$gm.Minimum,$gm.Maximum,(Std $hi.gScore))
    Write-Host ("Halcon  score: mean={0:N4} min={1:N4} max={2:N4} std={3:N4}" -f $hm.Average,$hm.Minimum,$hm.Maximum,(Std $hi.hScore))
    $dm = $hi.dScore | Measure-Object -Average -Minimum -Maximum
    Write-Host ("score diff (H-G): mean={0:N4} min={1:N4} max={2:N4}" -f $dm.Average,$dm.Minimum,$dm.Maximum)
    $pm = $hi.posDiff | Measure-Object -Average -Maximum
    Write-Host ("position diff (px): mean={0:N3} max={1:N3}" -f $pm.Average,$pm.Maximum)
    $absAng = $hi.dAngle | ForEach-Object { [math]::Abs($_) }
    $am = $absAng | Measure-Object -Average -Maximum
    Write-Host ("abs angle diff (deg): mean={0:N3} max={1:N3}" -f $am.Average,$am.Maximum)
}
# NccDemo reports preprocess+search average; Halcon reports search only, so this
# is an order-of-magnitude timing comparison, not a like-for-like ms number.
Write-Host ""
Write-Host ("GeoNCC  time ms/image (pre+search avg): mean={0:N2}" -f ($rows.gMs | Measure-Object -Average).Average)
Write-Host ("Halcon  time ms (search only):           mean={0:N2}" -f ($rows.hMs | Measure-Object -Average).Average)
Write-Host ""
Write-Host "--- per day ---"
$rows | Group-Object day | Sort-Object Name | ForEach-Object {
    $v = $_.Group | Where-Object { $_.gScore -ge 0 -and $_.hScore -ge 0 }
    if ($v.Count -gt 0) {
        Write-Host ("{0}: n={1} both={2} G_ncc={3:N3} H_ncc={4:N3} posdiff_max={5:N2}px" -f $_.Name,$_.Count,$v.Count,($v.gScore|Measure-Object -Average).Average,($v.hScore|Measure-Object -Average).Average,($v.posDiff|Measure-Object -Maximum).Maximum)
    } else {
        Write-Host ("{0}: n={1} both=0" -f $_.Name,$_.Count)
    }
}
