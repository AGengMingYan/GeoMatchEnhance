# analyze-sample.ps1 -- compare-sample.ps1 的汇总（四器：本项目/Halcon x Shape/NCC）
#
# 输出：检出率、分数分位数、相关性、同实例分数差/位置差/角度差、耗时、
#       data2 按真实朝向（以 NCC 项目内结果的角度为真值）分簇的检出率。
param(
    [string]$Csv = '_ai_tmp\sample.csv'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$path = Join-Path $root $Csv
if (-not (Test-Path $path)) { Write-Host "missing $path"; exit 1 }
$raw = @(Import-Csv $path)

function D($v) {
    if ($null -eq $v -or "$v" -eq '') { return $null }
    $o = 0.0
    if ([double]::TryParse("$v", [ref]$o)) { return $o }
    return $null
}
function Pet($a, $p) {
    $v = @($a | Where-Object { $null -ne $_ } | Sort-Object)
    if (-not $v.Count) { return [double]::NaN }
    return $v[[int][math]::Floor(($v.Count - 1) * $p)]
}
function Avg($a) {
    $v = @($a | Where-Object { $null -ne $_ })
    if ($v.Count) { return ($v | Measure-Object -Average).Average }
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

$sets = @($raw | Group-Object set | ForEach-Object { $_.Name })
foreach ($set in $sets) {
    $rows = @($raw | Where-Object { $_.set -eq $set } | ForEach-Object {
        [pscustomobject]@{
            img = $_.img
            gN = [int]$_.gN; gS = $(if ([int]$_.gN -gt 0) { D $_.gScore } else { $null })
            gX = D $_.gX; gY = D $_.gY; gA = D $_.gA; gMs = D $_.gMs
            nN = [int]$_.nN; nS = $(if ([int]$_.nN -gt 0) { D $_.nScore } else { $null })
            nX = D $_.nX; nY = D $_.nY; nA = D $_.nA; nMs = D $_.nMs
            hN = [int]$_.hN; hS = $(if ([int]$_.hN -gt 0) { D $_.hScore } else { $null })
            hX = D $_.hX; hY = D $_.hY; hA = D $_.hA; hMs = D $_.hMs
            kN = [int]$_.kN; kS = $(if ([int]$_.kN -gt 0) { D $_.kScore } else { $null })
            kX = D $_.kX; kY = D $_.kY; kA = D $_.kA; kMs = D $_.kMs
        }
    })
    $N = $rows.Count
    Write-Host ("==================== {0}  N={1} ====================" -f $set, $N)

    Write-Host '[1] 检出（>=1 个匹配）'
    Write-Host ("    shape  ours={0}/{1}   halcon={2}/{1}" -f `
        @($rows | Where-Object { $_.gN -gt 0 }).Count, $N, @($rows | Where-Object { $_.hN -gt 0 }).Count)
    Write-Host ("    ncc    ours={0}/{1}   halcon={2}/{1}" -f `
        @($rows | Where-Object { $_.nN -gt 0 }).Count, $N, @($rows | Where-Object { $_.kN -gt 0 }).Count)

    Write-Host '[2] 分数 min/p10/p50/p90/max/mean（仅检出）'
    foreach ($c in @(@{ n = 'shape ours  '; k = 'gS' }, @{ n = 'shape halcon'; k = 'hS' },
                     @{ n = 'ncc   ours  '; k = 'nS' }, @{ n = 'ncc   halcon'; k = 'kS' })) {
        $v = @($rows | ForEach-Object { $_.($c.k) })
        Write-Host ("    {0}: {1:N3}/{2:N3}/{3:N3}/{4:N3}/{5:N3}/{6:N3}  (n={7})" -f `
            $c.n, (Pet $v 0), (Pet $v 0.1), (Pet $v 0.5), (Pet $v 0.9), (Pet $v 1), (Avg $v),
            @($v | Where-Object { $null -ne $_ }).Count)
    }

    Write-Host '[3] 耗时 ms p10/p50/p90/mean（全部图）'
    foreach ($c in @(@{ n = 'shape ours  '; k = 'gMs' }, @{ n = 'shape halcon'; k = 'hMs' },
                     @{ n = 'ncc   ours  '; k = 'nMs' }, @{ n = 'ncc   halcon'; k = 'kMs' })) {
        $v = @($rows | ForEach-Object { $_.($c.k) })
        Write-Host ("    {0}: {1:N2}/{2:N2}/{3:N2}/{4:N2}" -f `
            $c.n, (Pet $v 0.1), (Pet $v 0.5), (Pet $v 0.9), (Avg $v))
    }

    Write-Host '[4] 分数相关性（双方都检出）'
    $sx = @(); $sy = @()
    foreach ($x in $rows) { if ($null -ne $x.gS -and $null -ne $x.hS) { $sx += $x.gS; $sy += $x.hS } }
    Write-Host ("    shape corr={0:N4} (n={1})" -f (Corr $sx $sy), $sx.Count)
    $sx = @(); $sy = @()
    foreach ($x in $rows) { if ($null -ne $x.nS -and $null -ne $x.kS) { $sx += $x.nS; $sy += $x.kS } }
    Write-Host ("    ncc   corr={0:N4} (n={1})" -f (Corr $sx $sy), $sx.Count)

    Write-Host '[5] 同实例一致性（双方都检出且位置差 <15px）'
    foreach ($p in @(@{ n = 'shape'; a = 'g'; b = 'h' }, @{ n = 'ncc  '; a = 'n'; b = 'k' })) {
        $dp = @(); $dpn = @(); $da = @(); $ds = @()
        foreach ($x in $rows) {
            $xa = $x.($p.a + 'X'); $ya = $x.($p.a + 'Y')
            $xb = $x.($p.b + 'X'); $yb = $x.($p.b + 'Y')
            if ($null -eq $xa -or $null -eq $xb) { continue }
            $d = [math]::Sqrt(($xa - $xb) * ($xa - $xb) + ($ya - $yb) * ($ya - $yb))
            $dp += $d
            if ($d -lt 15) {
                $dpn += $d
                $da += [math]::Abs([double]$x.($p.a + 'A') - [double]$x.($p.b + 'A'))
                $ds += ([double]$x.($p.b + 'S') - [double]$x.($p.a + 'S'))
            }
        }
        if ($dp.Count) {
            Write-Host ("    {0}: 全部 n={1}  dPos mean={2:N3} p50={3:N3} max={4:N3}px" -f `
                $p.n, $dp.Count, (Avg $dp), (Pet $dp 0.5), (Pet $dp 1))
        }
        if ($dpn.Count) {
            Write-Host ("            同实例 n={0}  dPos mean={1:N3} p50={2:N3} max={3:N3}px" -f `
                $dpn.Count, (Avg $dpn), (Pet $dpn 0.5), (Pet $dpn 1))
            Write-Host ("            同实例 dAngle mean={0:N3}deg  Δscore(Halcon-ours) mean={1:N4} min={2:N4} max={3:N4}" -f `
                (Avg $da), (Avg $ds), (Pet $ds 0), (Pet $ds 1))
        }
        else {
            Write-Host "            同实例 n=0（本数据集的 top-1 落在不同实例/有漏检）"
        }
    }
    Write-Host ''

}

# ---- data2 按真实朝向分簇（真值取本项目 NCC top-1 角度）-------------------
$d2 = @($raw | Where-Object { $_.set -eq 'data2' })
if ($d2.Count -gt 0) {
    Write-Host '==================== data2 按 NCC 真值朝向分簇 ===================='
    $b0  = @{ g = 0; h = 0; n = 0 }
    $b90 = @{ g = 0; h = 0; n = 0 }
    foreach ($x in $d2) {
        $na = D $x.nA
        if ($null -eq $na) { continue }
        $b = if ([math]::Abs([math]::Abs($na) - 90) -le 15) { $b90 } else { $b0 }
        $b.n++
        if ([int]$x.gN -gt 0) { $b.g++ }
        if ([int]$x.hN -gt 0) { $b.h++ }
    }
    Write-Host ("  0 deg   n={0,2}:  shape ours {1,2}/{0,2}   halcon {2,2}/{0,2}" -f $b0.n, $b0.g, $b0.h)
    Write-Host (" -90 deg  n={0,2}:  shape ours {1,2}/{0,2}   halcon {2,2}/{0,2}" -f $b90.n, $b90.g, $b90.h)
}

