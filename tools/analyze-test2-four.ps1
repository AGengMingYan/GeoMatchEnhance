# analyze-test2-four.ps1 -- 汇总 docs\test2四器对比-进度-2026-11-14.md 里的全部统计
#
# 用法（在仓库根目录执行）：
#   powershell -ExecutionPolicy Bypass -File tools\analyze-test2-four.ps1
#   powershell -ExecutionPolicy Bypass -File tools\analyze-test2-four.ps1 -Csv _ai_tmp\cmp200.csv
#
# 输入：compare-test2-four.ps1 产出的 CSV
# 输出：分位数 / 直方图 / 公平子集耗时 / 相关性 / 位置角度一致性 / 按日命中

param([string]$Csv = '')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Csv -eq '') { $Csv = Join-Path $root '_ai_tmp\cmp200.csv' }
if (-not (Test-Path $Csv)) { Write-Host "missing $Csv"; exit 1 }

function D($v) {
    if ($null -eq $v -or "$v" -eq '') { return $null }
    $o = 0.0
    if ([double]::TryParse("$v", [ref]$o)) { return $o }
    return $null
}
function P($arr, $p) {
    $a = @($arr | Where-Object { $null -ne $_ } | Sort-Object)
    if ($a.Count -eq 0) { return [double]::NaN }
    return $a[[int][math]::Floor(($a.Count - 1) * $p)]
}
function Avg($arr) {
    $a = @($arr | Where-Object { $null -ne $_ })
    if ($a.Count) { return (($a | Measure-Object -Average).Average) }
    return [double]::NaN
}
function Corr($x, $y) {
    if ($x.Count -ne $y.Count -or $x.Count -lt 3) { return [double]::NaN }
    $mx = ($x | Measure-Object -Average).Average
    $my = ($y | Measure-Object -Average).Average
    $sxy = 0.0; $sxx = 0.0; $syy = 0.0
    for ($i = 0; $i -lt $x.Count; $i++) {
        $dx = $x[$i] - $mx; $dy = $y[$i] - $my
        $sxy += $dx * $dy; $sxx += $dx * $dx; $syy += $dy * $dy
    }
    if ($sxx -le 0 -or $syy -le 0) { return [double]::NaN }
    return $sxy / [math]::Sqrt($sxx * $syy)
}

$raw = Import-Csv $Csv
# 无检出时脚本写 -1，这里统一转成 $null（否则分位数/相关会被 -1 污染）
$r = @(foreach ($x in $raw) {
        $gN = [int]$x.gN; $nN = [int]$x.nN; $hN = [int]$x.hN; $kN = [int]$x.kN
        [pscustomobject]@{
            day = $x.day; img = $x.img
            gN = $gN; nN = $nN; hN = $hN; kN = $kN
            gS = $(if ($gN -gt 0) { D $x.gScore } else { $null }); gMs = D $x.gMs
            nS = $(if ($nN -gt 0) { D $x.nScore } else { $null }); nMs = D $x.nMs
            hS = $(if ($hN -gt 0) { D $x.hScore } else { $null }); hMs = D $x.hMs
            kS = $(if ($kN -gt 0) { D $x.kScore } else { $null }); kMs = D $x.kMs
            gX = D $x.gX; gY = D $x.gY; gA = D $x.gA
            hX = D $x.hX; hY = D $x.hY; hA = D $x.hA
            nX = D $x.nX; nY = D $x.nY; nA = D $x.nA
            kX = D $x.kX; kY = D $x.kY; kA = D $x.kA
        }
    })

Write-Host ("rows = {0}   file = {1}" -f $r.Count, $Csv)
Write-Host ""

# ---------- 1. 分数分位数 ----------
Write-Host "===== 1. 分数分位数（top-1）====="
foreach ($c in @(@{n = 'shape ours  '; k = 'gS' }, @{n = 'shape halcon'; k = 'hS' },
                 @{n = 'ncc   ours  '; k = 'nS' }, @{n = 'ncc   halcon'; k = 'kS' })) {
    $v = @($r | ForEach-Object { $_.($c.k) })
    $none = @($v | Where-Object { $null -eq $_ }).Count
    Write-Host ("  {0}: p10={1:N3} p50={2:N3} p90={3:N3}  (无检出 {4})" -f `
        $c.n, (P $v 0.1), (P $v 0.5), (P $v 0.9), $none)
}
Write-Host ""

# ---------- 2. 直方图 ----------
Write-Host "===== 2. 分数直方图 ====="
$edges = @(0.0, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.01)
$labels = @('[0.00,0.30)', '[0.30,0.40)', '[0.40,0.50)', '[0.50,0.60)',
            '[0.60,0.70)', '[0.70,0.80)', '[0.80,0.90)', '[0.90,1.01]')
foreach ($grp in @(@{ t = 'SHAPE'; a = 'gS'; b = 'hS' }, @{ t = 'NCC'; a = 'nS'; b = 'kS' })) {
    Write-Host ("{0,-6} {1,-12} {2,6} {3,6}" -f $grp.t, '区间', 'ours', 'halcon')
    for ($i = 0; $i -lt $labels.Count; $i++) {
        $lo = $edges[$i]; $hi = $edges[$i + 1]
        $ca = @($r | Where-Object { $null -ne $_.($grp.a) -and $_.($grp.a) -ge $lo -and $_.($grp.a) -lt $hi }).Count
        $cb = @($r | Where-Object { $null -ne $_.($grp.b) -and $_.($grp.b) -ge $lo -and $_.($grp.b) -lt $hi }).Count
        Write-Host ("       {0,-12} {1,6} {2,6}" -f $labels[$i], $ca, $cb)
    }
    $na = @($r | Where-Object { $null -eq $_.($grp.a) }).Count
    $nb = @($r | Where-Object { $null -eq $_.($grp.b) }).Count
    Write-Host ("       {0,-12} {1,6} {2,6}" -f '无检出', $na, $nb)
    Write-Host ""
}

# ---------- 3. 耗时 ----------
Write-Host "===== 3. 耗时（ms/张，全量）====="
foreach ($c in @(@{n = 'shape ours  '; k = 'gMs' }, @{n = 'shape halcon'; k = 'hMs' },
                 @{n = 'ncc   ours  '; k = 'nMs' }, @{n = 'ncc   halcon'; k = 'kMs' })) {
    $v = @($r | ForEach-Object { $_.($c.k) })
    Write-Host ("  {0}: p10={1,6:N2} p50={2,6:N2} p90={3,6:N2} mean={4,6:N2}" -f `
        $c.n, (P $v 0.1), (P $v 0.5), (P $v 0.9), (Avg $v))
}
Write-Host ""
Write-Host "===== 3b. 公平子集耗时 ====="
$subsets = @(
    @{ n = 'SHAPE 双方都 >= 0.7'; f = { ($null -ne $_.gS -and $null -ne $_.hS -and $_.gS -ge 0.7 -and $_.hS -ge 0.7) }; a = 'gMs'; b = 'hMs' },
    @{ n = 'SHAPE 双方都 >= 0.9'; f = { ($null -ne $_.gS -and $null -ne $_.hS -and $_.gS -ge 0.9 -and $_.hS -ge 0.9) }; a = 'gMs'; b = 'hMs' },
    @{ n = 'SHAPE halcon 无检出'; f = { ($null -eq $_.hS) }; a = 'gMs'; b = 'hMs' },
    @{ n = 'NCC   双方都 >= 0.7'; f = { ($null -ne $_.nS -and $null -ne $_.kS -and $_.nS -ge 0.7 -and $_.kS -ge 0.7) }; a = 'nMs'; b = 'kMs' },
    @{ n = 'NCC   双方都 >= 0.9'; f = { ($null -ne $_.nS -and $null -ne $_.kS -and $_.nS -ge 0.9 -and $_.kS -ge 0.9) }; a = 'nMs'; b = 'kMs' }
)
foreach ($s in $subsets) {
    $sel = @($r | Where-Object $s.f)
    Write-Host ("  {0}: ours {1,6:N2} (n={2,3})   halcon {3,6:N2}" -f `
        $s.n, (Avg @($sel | ForEach-Object { $_.($s.a) })), $sel.Count, (Avg @($sel | ForEach-Object { $_.($s.b) })))
}
Write-Host ""


# ---------- 4. 相关性 ----------
Write-Host "===== 4. 分数相关性（双方都有有效分数的子集）====="
$sx = @(); $sy = @()
foreach ($x in $r) { if ($null -ne $x.gS -and $null -ne $x.hS) { $sx += $x.gS; $sy += $x.hS } }
Write-Host ("  shape corr = {0:N4}  (n={1})" -f (Corr $sx $sy), $sx.Count)
$sx = @(); $sy = @()
foreach ($x in $r) { if ($null -ne $x.nS -and $null -ne $x.kS) { $sx += $x.nS; $sy += $x.kS } }
Write-Host ("  ncc   corr = {0:N4}  (n={1})" -f (Corr $sx $sy), $sx.Count)
Write-Host ""

# ---------- 5. 位置/角度一致性（真阳性：ours 分数 >= 0.9 的图，取 top-1）----------
Write-Host "===== 5. 位置/角度一致性（真阳性图 top-1）====="
$pos = @($r | Where-Object { $null -ne $_.gS -and $_.gS -ge 0.9 })
$dp = @(); $da = @(); $dq = @(); $dk = @()
foreach ($x in $pos) {
    $s1 = $null; $s2 = $null; $s3 = $null; $s4 = $null
    if ($null -ne $x.hX) { $s1 = [math]::Sqrt(([math]::Pow($x.gX - $x.hX, 2)) + ([math]::Pow($x.gY - $x.hY, 2))) }
    if ($null -ne $x.hA) { $s2 = [math]::Abs($x.gA - $x.hA) }
    if ($null -ne $x.kX) { $s3 = [math]::Sqrt(([math]::Pow($x.nX - $x.kX, 2)) + ([math]::Pow($x.nY - $x.kY, 2))) }
    if ($null -ne $x.kA) { $s4 = [math]::Abs($x.nA - $x.kA) }
    if ($null -ne $s1) { $dp += $s1 }; if ($null -ne $s2) { $da += $s2 }
    if ($null -ne $s3) { $dq += $s3 }; if ($null -ne $s4) { $dk += $s4 }
    Write-Host ("  {0,-34} gN/hN={1,2}/{2,-2} nN/kN={3,2}/{4,-2} dPos={5,6:N3}/{6,6:N3} dAng={7,6:N3}/{8,6:N3}" -f `
        $x.img, $x.gN, $x.hN, $x.nN, $x.kN, $s1, $s3, $s2, $s4)
}
if ($dp.Count) {
    Write-Host ("  shape: mean dPos={0:N3} px  max={1:N3} px   mean dAng={2:N3} deg  max={3:N3} deg" -f `
        (($dp | Measure-Object -Average).Average), (($dp | Measure-Object -Maximum).Maximum),
        (($da | Measure-Object -Average).Average), (($da | Measure-Object -Maximum).Maximum))
}
if ($dq.Count) {
    Write-Host ("  ncc  : mean dPos={0:N3} px  max={1:N3} px   mean dAng={2:N3} deg  max={3:N3} deg" -f `
        (($dq | Measure-Object -Average).Average), (($dq | Measure-Object -Maximum).Maximum),
        (($dk | Measure-Object -Average).Average), (($dk | Measure-Object -Maximum).Maximum))
}
Write-Host ""

# ---------- 6. 按日命中 + 真阳性均值 ----------
Write-Host "===== 6. 按日命中（>=0.9）====="
$r | Group-Object day | Sort-Object Name | ForEach-Object {
    $g = @($_.Group)
    $a = @($g | Where-Object { $null -ne $_.gS -and $_.gS -ge 0.9 }).Count
    $b = @($g | Where-Object { $null -ne $_.hS -and $_.hS -ge 0.9 }).Count
    $c = @($g | Where-Object { $null -ne $_.nS -and $_.nS -ge 0.9 }).Count
    $d = @($g | Where-Object { $null -ne $_.kS -and $_.kS -ge 0.9 }).Count
    Write-Host ("  {0}  n={1,3}  shape {2,2}/{3,2}   ncc {4,2}/{5,2}" -f $_.Name, $g.Count, $a, $b, $c, $d)
}
Write-Host ""
Write-Host "===== 6b. 真阳性图均值 ====="
Write-Host ("  score  ours-shape={0:N4} halcon-shape={1:N4} ours-ncc={2:N4} halcon-ncc={3:N4}" -f `
    (Avg @($pos | ForEach-Object { $_.gS })), (Avg @($pos | ForEach-Object { $_.hS })),
    (Avg @($pos | ForEach-Object { $_.nS })), (Avg @($pos | ForEach-Object { $_.kS })))
Write-Host ("  time   ours-shape={0:N2} halcon-shape={1:N2} ours-ncc={2:N2} halcon-ncc={3:N2}" -f `
    (Avg @($pos | ForEach-Object { $_.gMs })), (Avg @($pos | ForEach-Object { $_.hMs })),
    (Avg @($pos | ForEach-Object { $_.nMs })), (Avg @($pos | ForEach-Object { $_.kMs })))
Write-Host ""

# ---------- 7. 实例数一致性 ----------
Write-Host "===== 7. 实例数一致性（全量）====="
$sameS = @($r | Where-Object { $_.gN -eq $_.hN }).Count
$sameN = @($r | Where-Object { $_.nN -eq $_.kN }).Count
Write-Host ("  shape 相同 {0}/{1}   ours 总数={2}  halcon 总数={3}" -f $sameS, $r.Count,
    (($r | ForEach-Object { $_.gN } | Measure-Object -Sum).Sum), (($r | ForEach-Object { $_.hN } | Measure-Object -Sum).Sum))
Write-Host ("  ncc   相同 {0}/{1}   ours 总数={2}  halcon 总数={3}" -f $sameN, $r.Count,
    (($r | ForEach-Object { $_.nN } | Measure-Object -Sum).Sum), (($r | ForEach-Object { $_.kN } | Measure-Object -Sum).Sum))

