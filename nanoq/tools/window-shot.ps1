# Captures the Soundshed Nano Q window to a PNG (PrintWindow, so it works when it is behind
# other windows). Usage: powershell -File nanoq/tools/window-shot.ps1 -Out shot.png [-Process "Soundshed Nano Q"]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Process = "Soundshed Nano Q",
    [string]$Resize = ""   # e.g. "1100x700": resizes the window first
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class NativeShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);
}
"@

[void][NativeShot]::SetProcessDPIAware()
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "No window for process '$Process'"; exit 1 }

if ([NativeShot]::IsIconic($p.MainWindowHandle)) {
    # A minimised window has no real size to capture: bring it back without taking focus.
    [void][NativeShot]::ShowWindow($p.MainWindowHandle, 4)
    Start-Sleep -Milliseconds 700
}

if ($Resize -match '^(\d+)x(\d+)$') {
    $r0 = New-Object NativeShot+RECT
    [void][NativeShot]::GetWindowRect($p.MainWindowHandle, [ref]$r0)
    [void][NativeShot]::MoveWindow($p.MainWindowHandle, $r0.Left, $r0.Top, [int]$Matches[1], [int]$Matches[2], $true)
    Start-Sleep -Milliseconds 800
}
$rect = New-Object NativeShot+RECT
[void][NativeShot]::GetWindowRect($p.MainWindowHandle, [ref]$rect)
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 2 = PW_RENDERFULLCONTENT, which captures GPU-composited windows.
[void][NativeShot]::PrintWindow($p.MainWindowHandle, $dc, 2)
$g.ReleaseHdc($dc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "$Out ${w}x${h}"
