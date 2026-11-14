# test2 batch compare: GeoMatch (x64) vs Halcon 17 (x86), same template/params
$ErrorActionPreference = 'Continue'
$root   = 'd:\work\gitwork\geo-match-enhance'
$build  = Join-Path $root 'build'
$halExe = Join-Path $root 'HalconDemo\halcon_shape_compare.exe'
$tpl    = Join-Path $root 'dataset\test2\template_diamond.jpg'
$data   = Join-Path $root 'dataset\test2\imgLog'
$outCsv = Join-Path $root '_ai_tmp\compare_test2.csv'

$env:PATH = (Join-Path $root 'HalconDemo\Halcon17\bin') + ';' + $env:PATH
$env:HALCONARCH = 'x86sse2-win32'
$env:HALCONROOT = Join-Path $root 'HalconDemo\Halcon17'

$perDay = 15
$imgs = Get-ChildItem $data -Directory | Sort-Object Name | ForEach-Object {
    Get-ChildItem $_.FullName -Filter *.jpg | Sort-Object Name | Select-Object -First $perDay
}
Write-Host ("images to compare: " + $imgs.Count)

$rows = New-Object System.Collections.Generic.List[object]
foreach ($img in $imgs) {
    $day  = Split-Path (Split-Path $img.FullName -Parent) -Leaf
    $name = $img.Name

    $gOut = & (Join-Path $build 'GeoMatchDemo.exe') -t2 -load (Join-Path $build '_diamond.shm') `
                -s $img.FullName -noshow -minscore 0.3 -max 12 2>&1 | Out-String
    $gMatch = [regex]::Match($gOut, 'matches\s*:\s*(\d+)')
    $gN = if ($gMatch.Success) { [int]$gMatch.Groups[1].Value } else { 0 }
    $gTime = [regex]::Match($gOut, 'search\s+(\d+\.\d+)\s*ms')
    $gMs = if ($gTime.Success) { [double]$gTime.Groups[1].Value } else { -1 }
    $g0 = [regex]::Match($gOut, '#0\s+score=([\d.]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
    $gScore = if ($g0.Success) { [double]$g0.Groups[1].Value } else { -1 }
    $gX = if ($g0.Success) { [double]$g0.Groups[2].Value } else { -1 }
    $gY = if ($g0.Success) { [double]$g0.Groups[3].Value } else { -1 }
    $gA = if ($g0.Success) { [double]$g0.Groups[4].Value } else { 0 }

    $hOut = & $halExe $tpl $img.FullName -angle-start -10 -angle-extent 20 `
                -smin 1 -smax 1 -minscore 0.3 -max 12 -max-overlap 1.0 2>&1 | Out-String
    $hN = ([regex]::Matches($hOut, '#\d+\s+score=')).Count
    if ($hN -eq 0) { $hScore=-1; $hX=-1; $hY=-1; $hA=0 }
    $hTime = [regex]::Match($hOut, 'search=([\d.]+)\s*ms')
    $hMs = if ($hTime.Success) { [double]$hTime.Groups[1].Value } else { -1 }
    $h0 = [regex]::Match($hOut, '#0\s+score=([\d.]+)\s+pos=\(([\d.\-]+);([\d.\-]+)\)\s+angle=([\d.\-]+)deg')
    $hScore = if ($h0.Success) { [double]$h0.Groups[1].Value } else { -1 }
    $hX = if ($h0.Success) { [double]$h0.Groups[2].Value } else { -1 }
    $hY = if ($h0.Success) { [double]$h0.Groups[3].Value } else { -1 }
    $hA = if ($h0.Success) { [double]$h0.Groups[4].Value } else { 0 }

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
$gOnly = $rows | Where-Object { $_.gScore -ge 0.7 -and $_.hScore -lt 0 }
$hOnly = $rows | Where-Object { $_.hScore -ge 0.7 -and $_.gScore -lt 0.7 }
$none = $rows | Where-Object { $_.gScore -lt 0.7 -and $_.hScore -lt 0.7 }
function Std($a) {
    $m = ($a | Measure-Object -Average).Average
    $v = ($a | ForEach-Object { ($_ - $m) * ($_ - $m) } | Measure-Object -Average).Average
    [math]::Sqrt($v)
}
Write-Host ""
Write-Host "===== SUMMARY: total=$($rows.Count), both-have-match=$($valid.Count), high-confidence(>=0.7 both)=$($hi.Count) ====="
Write-Host ("hit@0.7  Geo: {0}/{1}   Halcon: {2}/{3}" -f ($rows | Where-Object {$_.gScore -ge 0.7}).Count, $rows.Count, ($rows | Where-Object {$_.hScore -ge 0.7}).Count, $rows.Count)
Write-Host ("Geo-only high: {0}   Halcon-only high: {1}   neither/none: {2}" -f $gOnly.Count, $hOnly.Count, $none.Count)
if ($hi.Count -gt 0) {
    $gm = $hi.gScore | Measure-Object -Average -Minimum -Maximum
    $hm = $hi.hScore | Measure-Object -Average -Minimum -Maximum
    Write-Host ""
    Write-Host "--- HIGH-CONFIDENCE subset (score >= 0.7 on both) ---"
    Write-Host ("GeoMatch score: mean={0:N4} min={1:N4} max={2:N4} std={3:N4}" -f $gm.Average, $gm.Minimum, $gm.Maximum, (Std $hi.gScore))
    Write-Host ("Halcon  score: mean={0:N4} min={1:N4} max={2:N4} std={3:N4}" -f $hm.Average, $hm.Minimum, $hm.Maximum, (Std $hi.hScore))
    $dm = $hi.dScore | Measure-Object -Average -Minimum -Maximum
    Write-Host ("score diff (H-G): mean={0:N4} min={1:N4} max={2:N4}" -f $dm.Average, $dm.Minimum, $dm.Maximum)
    $pm = $hi.posDiff | Measure-Object -Average -Maximum
    Write-Host ("position diff (px): mean={0:N3} max={1:N3}" -f $pm.Average, $pm.Maximum)
    $absAng = $hi.dAngle | ForEach-Object { [math]::Abs($_) }
    $am = $absAng | Measure-Object -Average -Maximum
    Write-Host ("abs angle diff (deg): mean={0:N3} max={1:N3}" -f $am.Average, $am.Maximum)
}
Write-Host ""
Write-Host ("GeoMatch time ms: mean={0:N2} (all images)" -f ($rows.gMs | Measure-Object -Average).Average)
Write-Host ("Halcon  time ms: mean={0:N2} (all images)" -f ($rows.hMs | Measure-Object -Average).Average)
Write-Host ("candidate count Geo mean={0:N1}  Halcon mean={1:N1}" -f ($rows.gN | Measure-Object -Average).Average, ($rows.hN | Measure-Object -Average).Average)
Write-Host ""
Write-Host "--- per day ---"
$rows | Group-Object day | Sort-Object Name | ForEach-Object {
    $v = $_.Group | Where-Object { $_.gScore -ge 0 }
    if ($v.Count -gt 0) {
        Write-Host ("{0}: n={1} G_score={2:N3} H_score={3:N3} posdiff_max={4:N2}px G_ms={5:N1} H_ms={6:N1}" -f $_.Name, $_.Count, ($v.gScore | Measure-Object -Average).Average, ($v.hScore | Measure-Object -Average).Average, ($v.posDiff | Measure-Object -Maximum).Maximum, ($v.gMs | Measure-Object -Average).Average, ($v.hMs | Measure-Object -Average).Average)
    }
}
Write-Host ""
Write-Host "--- lowest 8 GeoMatch scores (hard cases) ---"
$rows | Sort-Object gScore | Select-Object -First 8 | ForEach-Object {
    Write-Host ("{0} {1}  G={2:N3}({3}cand,{4}ms)  H={5:N3}({6}cand,{7}ms)  posdiff={8}px" -f $_.day, $_.img.Substring(5,9), $_.gScore, $_.gN, $_.gMs, $_.hScore, $_.hN, $_.hMs, $_.posDiff)
}
