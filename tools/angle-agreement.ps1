# angle-agreement.ps1 -- 重复目标场景下更公平的角度一致性 & 同实例分数差
# dPos 在 max=1 重复目标上不可比（各器选不同实例），角度则不受影响
param([string[]]$Sets = @('data1', 'data2'), [string]$OutDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($OutDir -eq '') { $OutDir = Join-Path $root '_ai_tmp' }
function D($v){ if($null -eq $v -or "$v" -eq ''){return $null}; $o=0.0; if([double]::TryParse("$v",[ref]$o)){return $o}; return $null }
function Avg($a){ $v=@($a|Where-Object{$null -ne $_}); if($v.Count){($v|Measure-Object -Average).Average}else{[double]::NaN} }

foreach ($set in $Sets) {
    $csv = Join-Path $OutDir "$set.csv"
    if (-not (Test-Path $csv)) { continue }
    $r = @(Import-Csv $csv)
    Write-Host "==================== $set 角度一致性（双方都检出）===================="
    # shape ours vs halcon
    $sa=@(); $na=@()
    foreach($x in $r){
        if([int]$x.gN -gt 0 -and [int]$x.hN -gt 0){ $sa += [math]::Abs((D $x.gA)-(D $x.hA)) }
        if([int]$x.nN -gt 0 -and [int]$x.kN -gt 0){ $na += [math]::Abs((D $x.nA)-(D $x.kA)) }
    }
    if($sa.Count){ Write-Host ("  shape |dAngle|: n={0} mean={1:N3}deg p90={2:N3}deg max={3:N3}deg" -f $sa.Count,(Avg $sa),($sa|Sort-Object)[[int][math]::Floor(($sa.Count-1)*.9)],($sa|Measure-Object -Maximum).Maximum) }
    if($na.Count){ Write-Host ("  ncc   |dAngle|: n={0} mean={1:N3}deg p90={2:N3}deg max={3:N3}deg" -f $na.Count,(Avg $na),($na|Sort-Object)[[int][math]::Floor(($na.Count-1)*.9)],($na|Measure-Object -Maximum).Maximum) }

    # NCC 两器同实例判定：位置接近(<15px)才算同一个，统计这些对上的分数差
    $paired=@(); $far=0
    foreach($x in $r){
        if([int]$x.nN -gt 0 -and [int]$x.kN -gt 0){
            $dx=(D $x.nX)-(D $x.kX); $dy=(D $x.nY)-(D $x.kY)
            if([math]::Sqrt($dx*$dx+$dy*$dy) -lt 15){ $paired += [math]::Abs((D $x.nScore)-(D $x.kScore)) } else { $far++ }
        }
    }
    Write-Host ("  NCC 同实例(<15px)对数={0} 不同实例 top1={1}  同实例分数差 mean={2:N4} max={3:N4}" -f $paired.Count,$far,(Avg $paired),($paired|Measure-Object -Maximum).Maximum)
    # shape 同实例分数差
    $sp=@(); $sfar=0
    foreach($x in $r){
        if([int]$x.gN -gt 0 -and [int]$x.hN -gt 0){
            $dx=(D $x.gX)-(D $x.hX); $dy=(D $x.gY)-(D $x.hY)
            if([math]::Sqrt($dx*$dx+$dy*$dy) -lt 15){ $sp += [math]::Abs((D $x.gScore)-(D $x.hScore)) } else { $sfar++ }
        }
    }
    if($sp.Count){ Write-Host ("  shape 同实例(<15px)对数={0} 不同实例 top1={1}  同实例分数差 mean={2:N4} max={3:N4}" -f $sp.Count,$sfar,(Avg $sp),($sp|Measure-Object -Maximum).Maximum) }
    Write-Host ''
}
