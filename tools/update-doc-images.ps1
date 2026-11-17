# Regenerate every GUI screenshot referenced by the docs (ASCII only).
#
#   powershell -File tools\update-doc-images.ps1 [-SkipCapture]
#
# 1) tools\shot-gui-docs.ps1  -> _ai_tmp\gui_raw\*.png   (synthetic mouse walk-through)
# 2) tools\annotate-shot.ps1  -> docs\img\step*.png + docs\gui_*.{png,jpg}
#
# The annotation text lives in tools\shot-specs\*.json (UTF-8 with Chinese).  The
# scripts themselves stay pure ASCII so PowerShell 5.1 cannot parse them as GBK.
param(
    [switch]$SkipCapture,
    [string]$RawDir = '_ai_tmp\gui_raw'
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path

if (-not $SkipCapture) {
    $cap = Join-Path $root 'tools\shot-gui-docs.ps1'
    if (-not (Test-Path -LiteralPath $cap)) { Write-Output "MISSING $cap"; exit 1 }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $cap -OutDir $RawDir
}

$specs = Get-ChildItem -LiteralPath (Join-Path $root 'tools\shot-specs') -Filter '*.json' |
         Sort-Object Name
if (-not $specs) { Write-Output 'no specs found'; exit 1 }

$fail = 0
foreach ($s in $specs) {
    Write-Output "--- $($s.Name)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\annotate-shot.ps1') -Spec $s.FullName
    if ($LASTEXITCODE -ne 0) { $fail++ }
}

# self-check: plaintext + captions present + nothing clipped + all links resolve
Write-Output '--- verify'
& python (Join-Path $root 'tools\verify-doc-images.py')
if ($LASTEXITCODE -ne 0) { $fail++ }
& python (Join-Path $root 'tools\check-doc-links.py')
if ($LASTEXITCODE -ne 0) { $fail++ }

Write-Output "done, failures=$fail"
if ($fail -ne 0) { exit 1 }
