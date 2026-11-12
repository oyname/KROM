# =============================================================================
# KROM Engine - tools/shader_cross/capture_window.ps1
# Starts an executable, waits, saves its client area as PNG and ends it.
# Used to compare hand-written and generated GLSL under OpenGL pixel by pixel.
#   -Generated "1" sets KROM_GLSL_EXPERIMENT_GENERATED_TONEMAP for the run.
# Example:
#   capture_window.ps1 -Exe bin\ibl_sphere_opengl.exe -WorkDir bin -OutPng hand.png -LogFile hand.log
# =============================================================================
param(
    [string]$Exe,
    [string]$WorkDir,
    [string]$OutPng,
    [string]$LogFile,
    [string]$Generated = "0",
    [int]$WaitSeconds = 12
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KromCap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

$env:KROM_GLSL_EXPERIMENT_GENERATED_TONEMAP = $Generated
$p = Start-Process -FilePath $Exe -WorkingDirectory $WorkDir -RedirectStandardOutput $LogFile -RedirectStandardError "$LogFile.err" -PassThru
Start-Sleep -Seconds $WaitSeconds
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { "no window"; Stop-Process -Id $p.Id -Force; exit 1 }
$r = New-Object KromCap+RECT
[void][KromCap]::GetClientRect($h, [ref]$r)
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
# 1 = PW_CLIENTONLY, 2 = PW_RENDERFULLCONTENT (needed for GL/DX surfaces)
$ok = [KromCap]::PrintWindow($h, $hdc, 3)
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Stop-Process -Id $p.Id -Force
"captured ${w}x${hh} printwindow=$ok"
