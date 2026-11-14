# analyze-data.ps1 -- data1/data2 四器对比汇总（配合 compare-data.ps1 的 CSV）
param(
    [string[]]$Sets = @('data1', 'data2'),
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($OutDir -eq '') { $OutDir = Join-Path $root '_ai_tmp' }

function D($v) { if ($null -eq $v -or "$v" -eq '') { return $null }; $o=0.0; if ([double]::TryParse("$v",[ref]$o)) { return $o }; return $null }
function P($a,$p) { $v=@($a|Where-Object{$null -ne $_}|Sort-Object); if(-not $v.Count){return [double]::NaN}; $v[[int][math]::Floor(($v.Count-1)*$p)] }
function Avg($a) { $v=@($a|Where-Object{$null -ne $_}); if($v.Count){($v|Measure-Object -Average).Average}else{[double]::NaN} }
function Corr($x,$y){ if($x.Count -ne $y.Count -or $x.Count -lt 3){return [double]::NaN}; $mx=($x|Measure-Object -Average).Average; $my=($y|Measure-Object -Average).Average; $sxy=0.0;$sxx=0.0;$syy=0.0; for($i=0;$i -lt $x.Count;$i++){ $dx=$x[$i]-$mx;$dy=$y[$i]-$my;$sxy+=$dx*$dy;$sxx+=$dx*$dx;$syy+=$dy*$dy }; if($sxx -le 0 -or $syy -le 0){return [double]::NaN}; $sxy/[math]::Sqrt($sxx*$syy) }

foreach ($set in $Sets) {
    $csv = Join-Path $OutDir "$set.csv"
    if (-not (Test-Path $csv)) { Write-Host "missing $csv"; continue }
    $r = @(Import-Csv $csv | ForEach-Object {
        [pscustomobject]@{
            img=$_.img
            gN=[int]$_.gN; gS=$(if([int]$_.gN -gt 0){D $_.gScore}else{$null}); gX=D $_.gX; gY=D $_.gY; gA=D $_.gA; gMs=D $_.gMs
            nN=[int]$_.nN; nS=$(if([int]$_.nN -gt 0){D $_.nScore}else{$null}); nX=D $_.nX; nY=D $_.nY; nA=D $_.nA; nMs=D $_.nMs
            hN=[int]$_.hN; hS=$(if([int]$_.hN -gt 0){D $_.hScore}else{$null}); hX=D $_.hX; hY=D $_.hY; hA=D $_.hA; hMs=D $_.hMs
            kN=[int]$_.kN; kS=$(if([int]$_.kN -gt 0){D $_.kScore}else{$null}); kX=D $_.kX; kY=D $_.kY; kA=D $_.kA; kMs=D $_.kMs
        }
    })
    $N = $r.Count
    Write-Host ("==================== $set  N=$N ====================")

    # 1. 检出率
    Write-Host '[1] 检出数 (N>0)'
    Write-Host ("    shape  ours={0}/{1}   halcon={2}/{1}" -f @($r|Where-Object{$_.gN -gt 0}).Count,$N,@($r|Where-Object{$_.hN -gt 0}).Count)
    Write-Host ("    ncc    ours={0}/{1}   halcon={2}/{1}" -f @($r|Where-Object{$_.nN -gt 0}).Count,$N,@($r|Where-Object{$_.kN -gt 0}).Count)

    # 2. 分数统计（仅检出）
    Write-Host '[2] 分数 min/p10/p50/p90/max/mean（仅检出）'
    foreach ($c in @(@{n='shape ours  ';k='gS'},@{n='shape halcon';k='hS'},@{n='ncc   ours  ';k='nS'},@{n='ncc   halcon';k='kS'})) {
        $v=@($r|ForEach-Object{$_.($c.k)})
        Write-Host ("    {0}: {1:N3}/{2:N3}/{3:N3}/{4:N3}/{5:N3}/{6:N3}  (n={7})" -f $c.n,(P $v 0),(P $v .1),(P $v .5),(P $v .9),(P $v 1),(Avg $v),@($v|Where-Object{$null -ne $_}).Count)
    }

    # 3. 耗时（全部图）
    Write-Host '[3] 耗时 ms p10/p50/p90/mean（全部图）'
    foreach ($c in @(@{n='shape ours  ';k='gMs'},@{n='shape halcon';k='hMs'},@{n='ncc   ours  ';k='nMs'},@{n='ncc   halcon';k='kMs'})) {
        $v=@($r|ForEach-Object{$_.($c.k)})
        Write-Host ("    {0}: {1:N2}/{2:N2}/{3:N2}/{4:N2}" -f $c.n,(P $v .1),(P $v .5),(P $v .9),(Avg $v))
    }

    # 4. 相关性（双方都检出）
    Write-Host '[4] 分数相关性（双方都检出）'
    $sx=@();$sy=@(); foreach($x in $r){ if($null -ne $x.gS -and $null -ne $x.hS){$sx+=$x.gS;$sy+=$x.hS} }
    Write-Host ("    shape corr={0:N4} (n={1})" -f (Corr $sx $sy),$sx.Count)
    $sx=@();$sy=@(); foreach($x in $r){ if($null -ne $x.nS -and $null -ne $x.kS){$sx+=$x.nS;$sy+=$x.kS} }
    Write-Host ("    ncc   corr={0:N4} (n={1})" -f (Corr $sx $sy),$sx.Count)

    # 5. 位置/角度一致性（双方都检出，top-1）
    Write-Host '[5] 位置/角度一致性（双方都检出）'
    $dp=@();$da=@();$dq=@();$dk=@()
    foreach($x in $r){
        if($null -ne $x.gX -and $null -ne $x.hX){ $dp+=[math]::Sqrt(($x.gX-$x.hX)*($x.gX-$x.hX)+($x.gY-$x.hY)*($x.gY-$x.hY)); $da+=[math]::Abs($x.gA-$x.hA) }
        if($null -ne $x.nX -and $null -ne $x.kX){ $dq+=[math]::Sqrt(($x.nX-$x.kX)*($x.nX-$x.kX)+($x.nY-$x.kY)*($x.nY-$x.kY)); $dk+=[math]::Abs($x.nA-$x.kA) }
    }
    if($dp.Count){ Write-Host ("    shape: n={0} mean dPos={1:N3}px max={2:N3}px  mean dAng={3:N3}deg max={4:N3}deg" -f $dp.Count,(Avg $dp),($dp|Measure-Object -Maximum).Maximum,(Avg $da),($da|Measure-Object -Maximum).Maximum) }
    if($dq.Count){ Write-Host ("    ncc  : n={0} mean dPos={1:N3}px max={2:N3}px  mean dAng={3:N3}deg max={4:N3}deg" -f $dq.Count,(Avg $dq),($dq|Measure-Object -Maximum).Maximum,(Avg $dk),($dk|Measure-Object -Maximum).Maximum) }
    Write-Host ''
}

# ---- data2 专属：按 NCC 真值朝向（0° / -90°）分组的 shape 检出率 ----
$d2 = Join-Path $OutDir 'data2.csv'
if (Test-Path $d2) {
    Write-Host '==================== data2 按朝向分组（真值取 NCC ours 角度）===================='
    $rows = @(Import-Csv $d2)
    $g0 = @{ g = 0; h = 0; n = 0 }; $g90 = @{ g = 0; h = 0; n = 0 }
    foreach ($x in $rows) {
        $na = D $x.nA
        $bucket = if ([math]::Abs([math]::Abs($na) - 90) -le 15) { $g90 } else { $g0 }
        $bucket.n++
        if ([int]$x.gN -gt 0) { $bucket.g++ }
        if ([int]$x.hN -gt 0) { $bucket.h++ }
    }
    Write-Host ("  0° 簇  n={0,2}:  本项目shape 命中 {1,2}/{0,2}   Halcon shape 命中 {2,2}/{0,2}" -f $g0.n,$g0.g,$g0.h)
    Write-Host (" -90°簇  n={0,2}:  本项目shape 命中 {1,2}/{0,2}   Halcon shape 命中 {2,2}/{0,2}" -f $g90.n,$g90.g,$g90.h)
    Write-Host ''
    Write-Host 'data2 -90° 簇逐图（NCC 真值 vs 两器 shape）:'
    Write-Host ("  {0,-34} {1,7} {2,8} {3,8} {4,9}" -f 'image','NCCang','NCCscr','ourShp','hlcShp')
    foreach ($x in $rows) {
        $na = D $x.nA
        if ([math]::Abs([math]::Abs($na) - 90) -le 15) {
            $gs = if ([int]$x.gN -gt 0) { ('{0:N3}' -f (D $x.gScore)) } else { 'MISS' }
            $hs = if ([int]$x.hN -gt 0) { ('{0:N3}' -f (D $x.hScore)) } else { 'MISS' }
            Write-Host ("  {0,-34} {1,7:N1} {2,8:N3} {3,8} {4,9}" -f $x.img,$na,(D $x.nScore),$gs,$hs)
        }
    }
}

