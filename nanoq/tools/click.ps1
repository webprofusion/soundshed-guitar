# Drives the Soundshed Nano Q window for tests by posting mouse messages straight to its view
# window, so it needs neither focus nor the real pointer (the user's pointer is not moved).
# Positions are in the window's own pixels: the coordinates of a window-shot.ps1 capture.
#
#   click.ps1 -X 300 -Y 200                 # left click
#   click.ps1 -X 300 -Y 200 -Double         # double click
#   click.ps1 -X 300 -Y 200 -ToX 500        # drag along x (and -ToY)
#   click.ps1 -X 300 -Y 200 -Wheel -120     # wheel at the point (negative scrolls down)
#   click.ps1 -X 300 -Y 200 -Right          # right click
#   click.ps1 -X 300 -Y 200 -Hover          # move the pointer over a point only
#   click.ps1 -X 300 -Y 200 -Text "abc"     # type into what has the focus (click the box first)
param(
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y,
    [int]$ToX = -1,
    [int]$ToY = -1,
    [int]$Wheel = 0,
    [switch]$Double,
    [switch]$Right,
    [switch]$Hover,
    [string]$Text = "",
    [string]$Process = "Soundshed Nano Q"
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class NativeClick {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    public static IntPtr FindView(IntPtr top) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(top, (h, l) => {
            var sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            if (sb.ToString().Contains("ElementsView")) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr Pack(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
}
"@

[void][NativeClick]::SetProcessDPIAware()
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "No window for process '$Process'"; exit 1 }

$top = $p.MainWindowHandle
# A minimised window has no real size to draw into: bring it back without taking focus.
if ([NativeClick]::IsIconic($top)) { [void][NativeClick]::ShowWindow($top, 4); Start-Sleep -Milliseconds 700 }
$view =[NativeClick]::FindView($top)
if ($view -eq [IntPtr]::Zero) { Write-Error "No ElementsView child window"; exit 1 }

$r = New-Object NativeClick+RECT
[void][NativeClick]::GetWindowRect($top, [ref]$r)

# The point arrives in the capture's coordinates (relative to the window's top-left); turn it
# into the view's client coordinates through screen coordinates.
function ToClient([int]$wx, [int]$wy) {
    $pt = New-Object NativeClick+POINT
    $pt.X = $r.Left + $wx
    $pt.Y = $r.Top + $wy
    [void][NativeClick]::ScreenToClient($view, [ref]$pt)
    return $pt
}

$WM_MOUSEMOVE = 0x0200; $WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$WM_RBUTTONDOWN = 0x0204; $WM_RBUTTONUP = 0x0205; $WM_MOUSEWHEEL = 0x020A
$MK_LBUTTON = 1; $MK_RBUTTON = 2

function Send([uint32]$msg, [int]$wParam, $pt) {
    [void][NativeClick]::PostMessage($view, $msg, [IntPtr]$wParam, [NativeClick]::Pack($pt.X, $pt.Y))
}

$start = ToClient $X $Y
Send $WM_MOUSEMOVE 0 $start
Start-Sleep -Milliseconds 80

if ($Text -ne "") {
    # Typing: one WM_CHAR per character, to whatever has the keyboard focus in the view.
    foreach ($ch in $Text.ToCharArray()) {
        [void][NativeClick]::PostMessage($view, 0x0102, [IntPtr][int]$ch, [IntPtr]1)
        Start-Sleep -Milliseconds 40
    }
}
elseif ($Hover) {
    # nothing more
}
elseif ($Wheel -ne 0) {
    # The wheel's position is in screen coordinates.
    $screen = [NativeClick]::Pack($r.Left + $X, $r.Top + $Y)
    [void][NativeClick]::PostMessage($view, $WM_MOUSEWHEEL, [IntPtr]($Wheel -shl 16), $screen)
}
elseif ($Right) {
    Send $WM_RBUTTONDOWN $MK_RBUTTON $start
    Start-Sleep -Milliseconds 60
    Send $WM_RBUTTONUP 0 $start
}
elseif ($ToX -ge 0 -or $ToY -ge 0) {
    $tx = if ($ToX -ge 0) { $ToX } else { $X }
    $ty = if ($ToY -ge 0) { $ToY } else { $Y }
    Send $WM_LBUTTONDOWN $MK_LBUTTON $start
    Start-Sleep -Milliseconds 80
    $steps = 12
    for ($i = 1; $i -le $steps; $i++) {
        $pt = ToClient ([int]($X + ($tx - $X) * $i / $steps)) ([int]($Y + ($ty - $Y) * $i / $steps))
        Send $WM_MOUSEMOVE $MK_LBUTTON $pt
        Start-Sleep -Milliseconds 25
    }
    Send $WM_LBUTTONUP 0 (ToClient $tx $ty)
}
else {
    $n = if ($Double) { 2 } else { 1 }
    for ($i = 0; $i -lt $n; $i++) {
        Send $WM_LBUTTONDOWN $MK_LBUTTON $start
        Start-Sleep -Milliseconds 50
        Send $WM_LBUTTONUP 0 $start
        Start-Sleep -Milliseconds 90
    }
}
Start-Sleep -Milliseconds 400
