# Batch shape-matching test on dataset/image (Halcon-style sample set).
# Each group: one single-object template (m*.bmp) -> several multi-instance scenes.
# Run from repo root:  powershell -File tools\test-image-dataset.ps1
# ASCII only in this script (PS 5.1 reads a BOM-less .ps1 as GBK).
param(
    [string]$Exe  = 'build\GeoMatchDemo.exe',
    [string]$Root = 'dataset\image',
    [string]$Out  = '_ai_tmp\imtest'
)
$ErrorActionPreference = 'Continue'
$proj = Split-Path $PSScriptRoot -Parent
Set-Location $proj
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }

# template, scenes, angle-range, min-score
$groups = @(
    @{ tpl = 'm3.bmp'; kind = 'gear';     scenes = @('retainers_01.png','retainers_02.png','retainers_03.png','rings_01.png','rings_02.png','rings_03.png','rings_04.png','rings_06.png'); ms = 0.5 },
    @{ tpl = 'm4.bmp'; kind = 'nut';      scenes = @('nuts_01.png','nuts_02.png','nuts_03.png'); ms = 0.5 },
    @{ tpl = 'm1.bmp'; kind = 'retainer'; scenes = @('rings_01.png','rings_02.png','rings_03.png','rings_04.png','rings_06.png'); ms = 0.45 }
)

$rows = @()
foreach ($g in $groups) {
    $tpl = Join-Path $Root $g.tpl
    foreach ($sc in $g.scenes) {
        $img = Join-Path $Root $sc
        if (-not (Test-Path $img)) { continue }
        $odir = Join-Path $Out ($g.kind + '_' + [IO.Path]::GetFileNameWithoutExtension($sc))
        $arg = @('-t', $tpl, '-s', $img, '-angle', '360',
                 '-minscore', $g.ms, '-max', '30',
                 '-max-overlap', '0.5', '-min-dist', '40',
                 '-noshow', '-o', $odir)
        $log = & $Exe @arg 2>&1 | Out-String
        $mLine = ($log -split "`n" | Where-Object { $_ -match 'matches =' } | Select-Object -First 1)
        $n = -1
        if ($mLine -match 'matches\s*=\s*(\d+)') { $n = [int]$Matches[1] }
        $det = ($log -split "`n" | Where-Object { $_ -match '^\s*#\d' })
        $top = if ($det.Count -gt 0) { ($det[0] -replace '^\s*','') } else { '' }
        $low = if ($det.Count -gt 0) {
                   $scores = $det | ForEach-Object { if ($_ -match 's=([\d.]+)') { [double]$Matches[1] } }
                   if ($scores) { ($scores | Measure-Object -Minimum).Minimum.ToString('0.000') } else { '' }
               } else { '' }
        $rows += [pscustomobject]@{
            kind = $g.kind; scene = $sc; matches = $n; top = $top; minScore = $low
        }
        Write-Host ("{0,-9} {1,-16} matches={2,2}  {3}" -f $g.kind, $sc, $n, $top)
    }
}
$csv = Join-Path $Out 'summary.csv'
$rows | Export-Csv -Path $csv -NoTypeInformation -Encoding UTF8
Write-Host "`nCSV -> $csv"
