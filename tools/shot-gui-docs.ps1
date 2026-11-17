# Capture the GeoGUIDemo screenshots used by docs (ASCII only, no BOM needed).
#
#   powershell -File tools\shot-gui-docs.ps1 [-OutDir _ai_tmp\gui_raw] [-Exe build\GeoGUIDemo.exe]
#
# Produces (full 1500x920 window PNGs):
#   00_overview.png       startup, 3rd image (a real match) on screen
#   01_dir.png            panel at top ("2. matching target" block)
#   02_roi.png            target tab with a cyan ROI rectangle
#   03_useroi.png         after clicking "use target ROI as template"
#   04_panel_bottom.png   panel scrolled to "4. execute match"
#   05_after_exec.png     after executing
#   06_result.png         "match result" view
#   07_model.png          "template model" view
#
# Then run tools\update-doc-images.ps1 to crop/annotate them into docs/.
param(
    [string]$OutDir = '_ai_tmp\gui_raw',
    [string]$Exe = 'build\GeoGUIDemo.exe',
    [string]$Dir = '../case1'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$root = (Get-Location).Path
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$wd = Split-Path -Parent $exePath
$out = Join-Path $root $OutDir
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null

# wait until the physical mouse has been still for ~2 s (the user may be working)
$lastX = -99999; $lastY = -99999
for ($k = 0; $k -lt 40; $k++) {
    $p0 = [System.Windows.Forms.Cursor]::Position
    if ($p0.X -eq $lastX -and $p0.Y -eq $lastY) { break }
    $lastX = $p0.X; $lastY = $p0.Y
    Start-Sleep -Milliseconds 500
}

$proc = Start-Process -FilePath $exePath -ArgumentList $Dir -WorkingDirectory $wd -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 160; $i++) {
    Start-Sleep -Milliseconds 250
    if ($proc.HasExited) { Write-Output "EXITED code=$($proc.ExitCode)"; exit 1 }
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $h = $proc.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) { Write-Output 'NO WINDOW'; Stop-Process -Id $proc.Id -Force; exit 1 }

$sig = @'
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, WHEEL = 0x0800;
  public static void Click(int x, int y) {
    SetCursorPos(x, y); System.Threading.Thread.Sleep(200);
    mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(110);
    mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero);
  }
  public static void Drag(int x1, int y1, int x2, int y2) {
    SetCursorPos(x1, y1); System.Threading.Thread.Sleep(250);
    mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(250);
    for (int i = 1; i <= 18; i++) {
      SetCursorPos(x1 + (x2 - x1) * i / 18, y1 + (y2 - y1) * i / 18);
      System.Threading.Thread.Sleep(60);
    }
    System.Threading.Thread.Sleep(250);
    mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero);
  }
  public static void Scroll(int x, int y, int notches) {
    SetCursorPos(x, y); System.Threading.Thread.Sleep(220);
    for (int i = 0; i < Math.Abs(notches); i++) {
      mouse_event(WHEEL, 0, 0, (uint)(notches > 0 ? -120 : 120), IntPtr.Zero);
      System.Threading.Thread.Sleep(70);
    }
  }
  public static void Park(int x, int y) { SetCursorPos(x, y); System.Threading.Thread.Sleep(500); }
}
'@
Add-Type -TypeDefinition $sig

[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 3000
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
$L = $r.L; $T = $r.T
Write-Output "window ${w}x${ht} at ($L,$T)"

function Snap([string]$name) {
  $bmp = New-Object System.Drawing.Bitmap($w, $ht)
  $g   = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($L, $T, 0, 0, $bmp.Size)
  $g.Dispose()
  $bmp.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Output "snap $name"
}

# Calibrated on the 1500x920 window (client coordinates).  If the window size or
# the panel layout changes, re-calibrate with _ai_tmp\find_rows.py / find_tabs.py.
$TAB_RESULT = 255; $TAB_TARGET = 316; $TAB_MODEL = 434; $TAB_Y = 66
$PNL_X      = 1257     # centre of the right-panel buttons
$NEXT_Y     = 490      # "next image >>"
$ROI_TPL_Y  = 280      # "use target ROI as template"
$EXEC_Y     = 628      # "execute match" once the panel is scrolled to the bottom

# go to the 3rd image, which really matches (1280x1024 target), so the
# overview shot is not the "matches=0" startup state
[W]::Click($L + 1420, $T + $NEXT_Y); Start-Sleep -Milliseconds 800
[W]::Click($L + 1420, $T + $NEXT_Y); Start-Sleep -Milliseconds 2600
[W]::Park($L + 460, $T + 800)
Snap '00_overview.png'
Snap '01_dir.png'

# target tab -> drag a ROI -> back to the target tab so the cyan box is visible
[W]::Click($L + $TAB_TARGET, $T + $TAB_Y); Start-Sleep -Milliseconds 1400
[W]::Drag($L + 380, $T + 330, $L + 640, $T + 560)
Start-Sleep -Milliseconds 2000
[W]::Click($L + $TAB_TARGET, $T + $TAB_Y); Start-Sleep -Milliseconds 1400
[W]::Park($L + 900, $T + 800)
Snap '02_roi.png'

[W]::Click($L + $PNL_X, $T + $ROI_TPL_Y); Start-Sleep -Milliseconds 3400
[W]::Park($L + 460, $T + 800)
Snap '03_useroi.png'

[W]::Scroll($L + $PNL_X, $T + 600, 12); Start-Sleep -Milliseconds 1000
[W]::Park($L + 460, $T + 800)
Snap '04_panel_bottom.png'
[W]::Click($L + $PNL_X, $T + $EXEC_Y); Start-Sleep -Milliseconds 2800
[W]::Park($L + 460, $T + 800)
Snap '05_after_exec.png'

[W]::Click($L + $TAB_RESULT, $T + $TAB_Y); Start-Sleep -Milliseconds 1500
[W]::Park($L + 900, $T + 800)
Snap '06_result.png'
[W]::Click($L + $TAB_MODEL, $T + $TAB_Y); Start-Sleep -Milliseconds 1800
[W]::Park($L + 900, $T + 800)
Snap '07_model.png'

Stop-Process -Id $proc.Id -Force
Write-Output "done -> $out"
