# Stress test for the Nano Q UI: drives the running standalone through the DevDriver
# (src/ui/debug/DevDriver.h) with a random but seeded mix of clicks, knob drags, scrolls, keys,
# engine-side chain edits (`cmd` lines, src/ui/debug/DevCommands.h), scene and preset switches,
# undo/redo and window resizes, and reports any crash, hang (the UI thread stops taking the
# drive file), AddressSanitizer report or native dialog, with the seed and the command log.
#
#   powershell -File nanoq/tools/stress-ui.ps1 [-Build asan|RelWithDebInfo|Release] [-Seed 1] [-Steps 2000]
#   powershell -File nanoq/tools/stress-ui.ps1 -Replay D:\t\nqstress\run-asan-1\commands.log
#
# Each run starts from a copy of a template profile (made by the first run), so a seed replays
# the same session (timing still varies, so a timer-bound bug may need a few runs or a focused
# replay). Never runs on the real profile.
#
# Builds: Release and RelWithDebInfo come from nanoq\build.cmd. asan is a RelWithDebInfo tree in
# nanoq\build\asan with the sanitizer on everywhere (MSVC; Ninja; run from a VS dev prompt):
#   set AF=/fsanitize=address /D_DISABLE_VECTOR_ANNOTATION /D_DISABLE_STRING_ANNOTATION /D_DISABLE_OPTIONAL_ANNOTATION
#   cmake -S nanoq -B nanoq\build\asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#         "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /Zi %AF%" "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /Zi %AF%"
#         -DCMAKE_EXE_LINKER_FLAGS=/INCREMENTAL:NO
#   cmake --build nanoq\build\asan --target nanoq_standalone
# The script puts that compiler's ASan runtime on PATH and logs reports to the run folder.
#
# Not isolated: clap-wrapper's own device settings file (%LOCALAPPDATA%\clap-wrapper-standalone),
# which the run never changes: clicks stay off the page tabs (the compact frame's rail, the wide
# frame's top bar), and a device change on a throwaway profile is not saved (ClapWrapperDevices.cpp).
param(
    [ValidateSet("asan", "RelWithDebInfo", "Release")][string]$Build = "asan",
    [int]$Seed = 1,
    [int]$Steps = 1000,
    [string]$Root = "D:\t\nqstress",
    [string]$Replay = "",
    [int]$HangSeconds = 15,
    [int]$StepMs = 40,
    # cdb.exe for a stack dump of a hung UI thread; found under the Windows Kits when not given.
    [string]$Cdb = "",
    # Runs the app under cdb, so a crash ASan does not report (an access violation outside
    # instrumented code, a fail-fast) leaves its stack in cdb.log and a minidump, crash.dmp.
    [switch]$Debugger
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tree = if ($Build -eq "asan") { "nanoq\build\asan" } else { "nanoq\build\$Build" }
$exe = Join-Path $repo "$tree\products\Standalone-nanoq_standalone\Soundshed Nano Q.exe"
if (-not (Test-Path $exe)) { Write-Error "Not built: $exe"; exit 2 }

$runName = if ($Replay) { "replay-" + (Get-Date -Format "HHmmss") } else { "run-$Build-$Seed" }
$out = Join-Path $Root $runName
$template = Join-Path $Root "template-profile"
$profileDir = Join-Path $out "profile"
$drive = Join-Path $out "drive.txt"
$logFile = Join-Path $out "commands.log"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class NqWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    static string Txt(IntPtr h) { var s = new StringBuilder(1024); GetWindowText(h, s, 1024); return s.ToString(); }
    public static List<IntPtr> TopLevel(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    // Visible dialogs (#32770: message boxes, file dialogs) of the process are cancelled; each is
    // reported once, with its title and the text of its controls.
    static HashSet<IntPtr> seen = new HashSet<IntPtr>();
    public static List<string> CloseDialogs(uint pid) {
        var found = new List<string>();
        foreach (var h in TopLevel(pid)) {
            if (Cls(h) != "#32770") continue;
            if (seen.Add(h)) {
                var text = new StringBuilder(Txt(h));
                EnumChildWindows(h, (c, l) => { var t = Txt(c); if (t.Length > 0 && text.Length < 300) text.Append(" | ").Append(t); return true; }, IntPtr.Zero);
                found.Add(text.ToString());
            }
            // WM_COMMAND IDCANCEL: a file dialog ignores WM_CLOSE, and Cancel never saves anything.
            PostMessage(h, 0x0111, (IntPtr)2, IntPtr.Zero);
            PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); // WM_CLOSE, for a message box
        }
        return found;
    }
    // The app's own window: the largest visible top-level window that is not a dialog.
    public static IntPtr MainWindow(uint pid) {
        IntPtr best = IntPtr.Zero; long area = 0;
        foreach (var h in TopLevel(pid)) {
            if (Cls(h) == "#32770") continue;
            RECT r; GetWindowRect(h, out r); long a = (long)(r.R - r.L) * (r.B - r.T);
            if (a > area) { area = a; best = h; }
        }
        return best;
    }
    // Resizes the window so its client area is w x h pixels. The standalone's view works in the
    // client area's own pixels (its log says "scale 1"), which are also the DevDriver's.
    public static void ResizeClient(IntPtr h, int w, int hgt) {
        RECT wr, cr; GetWindowRect(h, out wr); GetClientRect(h, out cr);
        int fw = (wr.R - wr.L) - (cr.R - cr.L), fh = (wr.B - wr.T) - (cr.B - cr.T);
        // SWP_ASYNCWINDOWPOS: a hung UI thread must not hang the script too.
        SetWindowPos(h, IntPtr.Zero, 0, 0, w + fw, hgt + fh, 0x0002 | 0x0004 | 0x0010 | 0x4000);
    }
    public static int[] ClientSize(IntPtr h) {
        RECT cr; GetClientRect(h, out cr);
        return new int[] { cr.R - cr.L, cr.B - cr.T };
    }
}
"@

function Find-Cdb {
    if ($Cdb -and (Test-Path $Cdb)) { return $Cdb }
    foreach ($c in @("${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe", "$env:ProgramFiles\Windows Kits\10\Debuggers\x64\cdb.exe")) {
        if (Test-Path $c) { return $c }
    }
    $w = Get-ChildItem "$env:ProgramFiles\WindowsApps" -Filter cdb.exe -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "amd64" } | Select-Object -First 1
    if ($w) { return $w.FullName }
    return $null
}

function Start-App([string]$profilePath, [string]$stderrPath) {
    # Only an earlier copy of this build (a run that was stopped); never another one's instance.
    # A copy left under -Debugger is frozen in cdb and cannot be stopped until its cdb goes.
    $stale = @(Get-Process -Name "Soundshed Nano Q" -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })
    foreach ($s in $stale) {
        Get-CimInstance Win32_Process -Filter "Name='cdb.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -match "-p $($s.Id) " } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }
    $stale | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    $env:SOUNDSHED_NANOQ_PROFILE = $profilePath
    $env:SOUNDSHED_NANOQ_DRIVE = $drive
    # Native open/save dialogs answer as cancelled (FileDialogs.cpp): one would wait for a person.
    $env:SOUNDSHED_NANOQ_NO_FILE_DIALOGS = "1"
    if ($Build -eq "asan") {
        # The ASan runtime DLL sits beside the compiler; abort on the first report, logged per pid.
        # The runtime must be the one beside the compiler the tree was built with: another
        # toolset's copy fails to load (0xC0000139).
        $cache = Get-Content (Join-Path $repo "$tree\CMakeCache.txt") | Where-Object { $_ -match "^CMAKE_CXX_COMPILER:FILEPATH=" }
        $binDir = Split-Path -Parent ($cache -replace "^CMAKE_CXX_COMPILER:FILEPATH=", "")
        if (-not (Test-Path (Join-Path $binDir "clang_rt.asan_dynamic-x86_64.dll"))) { Write-Error "No ASan runtime in $binDir" }
        $env:PATH = "$binDir;$env:PATH"
        # Quoted: the drive letter's colon would end the value.
        $env:ASAN_OPTIONS = "log_path='$out\asan':halt_on_error=1:detect_leaks=0:windows_hook_rtl_allocators=true"
    }
    $p = Start-Process -FilePath $exe -PassThru -RedirectStandardError $stderrPath -RedirectStandardOutput "$stderrPath.out"
    $null = $p.Handle # held now, or ExitCode reads empty once the process has gone
    return $p
}

function Wait-Window($p, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ($p.HasExited) { return [IntPtr]::Zero }
        $h = [NqWin]::MainWindow([uint32]$p.Id)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 200
    }
    return [IntPtr]::Zero
}

# The drive file is still there: the UI thread has not taken it. A file the app is deleting can
# refuse access for a moment; that counts as taken (Test-Path threw on it).
function Drive-Waiting {
    return [System.IO.File]::Exists($drive)
}

# Writes the lines whole, then renames them into place, so the app never reads half a batch.
# LF endings (the driver strips a CR too). The rename is retried while the last file is still
# being deleted.
function Send-Drive([string[]]$lines) {
    [System.IO.File]::WriteAllText("$drive.tmp", (($lines -join "`n") + "`n"))
    for ($i = 0; $i -lt 50; $i++) {
        try { [System.IO.File]::Move("$drive.tmp", $drive); return } catch { Start-Sleep -Milliseconds 10 }
    }
    throw "Could not write $drive"
}

# ── Template profile: made once, by letting the app create and fill one ─────────────────────
if (-not (Test-Path (Join-Path $template "done"))) {
    Write-Output "Making the template profile in $template (first run only)"
    if (Test-Path $template) { Remove-Item -Recurse -Force $template }
    New-Item -ItemType Directory -Force $template | Out-Null
    $p = Start-App $template (Join-Path $out "template-stderr.txt")
    Start-Sleep -Seconds 30
    [NqWin]::CloseDialogs([uint32]$p.Id) | Out-Null
    $p | Stop-Process -Force
    Start-Sleep -Seconds 1
    New-Item -ItemType File (Join-Path $template "done") | Out-Null
}
Copy-Item -Recurse $template $profileDir

# ── The command stream ───────────────────────────────────────────────────────────────────────
$rng = New-Object System.Random($Seed)
function Rnd([int]$lo, [int]$hi) { return $rng.Next($lo, $hi) }   # [lo, hi)
function Frac() { return [Math]::Round($rng.NextDouble(), 3) }

$sizes = @(@(960, 600), @(1280, 800), @(800, 480), @(1100, 720), @(1600, 960), @(980, 620), @(720, 460))
$keys = @("ctrl+z", "ctrl+y", "ctrl+shift+z", "escape", "escape", "delete", "space", "tab", "left", "right", "up", "down", "enter", "pageup", "pagedown", "home", "end")
$cmds = @(
    { "cmd add $(Rnd 0 200) $(Rnd -1 12)" }, { "cmd add $(Rnd 0 200) $(Rnd -1 12)" },
    { "cmd remove $(Rnd 0 12)" }, { "cmd move $(Rnd 0 12) $(Rnd 0 12)" }, { "cmd bypass $(Rnd 0 12)" },
    { "cmd replace $(Rnd 0 12) $(Rnd 0 200)" },
    { "cmd param $(Rnd 0 12) $(Rnd 0 30) $(Frac)" }, { "cmd param $(Rnd 0 12) $(Rnd 0 30) $(Frac)" }, { "cmd param $(Rnd 0 12) $(Rnd 0 30) $(Frac)" },
    { "cmd scene $(Rnd 0 4)" }, { "cmd addscene" }, { "cmd removescene $(Rnd 0 4)" },
    { "cmd preset $(@(-1, 1)[(Rnd 0 2)])" }, { "cmd load $(Rnd 0 400)" },
    { "cmd branches $(Rnd 0 12) $(Rnd 2 5)" }, { "cmd collapse $(Rnd 0 12)" }
)

# The page tabs (Settings, Tones, Jam) are left alone: the compact frame's rail at the left and
# the wide frame's top bar. Nothing left of railWidth or above topBar is clicked.
$railWidth = 64
$topBar = 40

function Next-Batch([int]$w, [int]$h) {
    $lines = @()
    $n = Rnd 1 4
    for ($i = 0; $i -lt $n; $i++) {
        $x = Rnd $railWidth ([Math]::Max($railWidth + 1, $w - 4)); $y = Rnd $topBar ([Math]::Max($topBar + 1, $h - 4))
        $roll = Rnd 0 100
        if ($roll -lt 34) { $lines += "click $x $y" }
        elseif ($roll -lt 46) { $dy = Rnd -120 120; $lines += "drag $x $y $x $([Math]::Max($topBar, $y + $dy))" }
        elseif ($roll -lt 49) { $dx = Rnd -200 200; $lines += "drag $x $y $([Math]::Max($railWidth, $x + $dx)) $y" }
        elseif ($roll -lt 53) { $lines += "scroll $x $y $(Rnd -3 4)" }
        elseif ($roll -lt 56) { $lines += "double $x $y" }
        elseif ($roll -lt 59) { $lines += "right $x $y" }
        elseif ($roll -lt 68) { $lines += "key $($keys[(Rnd 0 $keys.Count)])" }
        elseif ($roll -lt 69) { $lines += "type $(@('1', '-3', 'abc', '0.5', '99999', 'x')[(Rnd 0 6)])" }
        elseif ($roll -lt 70) { $lines += "move $x $y" }
        elseif ($roll -lt 97) { $lines += (& $cmds[(Rnd 0 $cmds.Count)]) }
        else { $s = $sizes[(Rnd 0 $sizes.Count)]; $lines += "#resize $($s[0]) $($s[1])" }
    }
    return , $lines
}

# ── Run ─────────────────────────────────────────────────────────────────────────────────────
$stderrPath = Join-Path $out "stderr.txt"
$p = Start-App $profileDir $stderrPath
# A failure of the script itself must not leave the app running (it holds the script's output).
$dbg = $null
# Stops the app, and cdb first when it is attached: a debuggee cannot finish exiting until its
# debugger lets it, and cdb's kill-on-exit takes the app with it.
function Stop-Run {
    if ($dbg -and -not $dbg.HasExited) { $dbg | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500 }
    if ($p -and -not $p.HasExited) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
}
trap { Stop-Run; break }
$hwnd = Wait-Window $p 60
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "The app did not open a window (exit $($p.ExitCode))"; exit 2 }
if ($Debugger) {
    $cdbExe = Find-Cdb
    if (-not $cdbExe) { Write-Error "-Debugger needs cdb.exe (-Cdb)" }
    # First-chance access violations are ASan's own business (its shadow memory); the second
    # chance is the crash. A fail-fast (0xC0000409) is reported to the debugger once.
    $dump = Join-Path $out "crash.dmp"
    @(
        "sxd -c2 `"~.kn 80; .dump /ma $dump; qd`" av"
        "sxe -c `"~.kn 80; .dump /ma $dump; qd`" c0000409"
        "sxe -c `"~.kn 80; .dump /ma $dump; qd`" c0000374"
        "g"
    ) | Out-File -Encoding ascii (Join-Path $out "cdb-cmds.txt")
    $env:_NT_SYMBOL_PATH = Split-Path -Parent $exe
    $dbg = Start-Process -PassThru -FilePath $cdbExe -WindowStyle Hidden -ArgumentList @("-p", $p.Id, "-cf", "`"$(Join-Path $out 'cdb-cmds.txt')`"", "-logo", "`"$(Join-Path $out 'cdb.log')`"")
}
Start-Sleep -Seconds 6   # let the first state arrive
[NqWin]::ResizeClient($hwnd, 960, 600)

$replayLines = if ($Replay) { Get-Content $Replay | Where-Object { $_ -notmatch "^;" } } else { $null }
$result = "ok"
$detail = ""
$step = 0
$replayAt = 0
$started = Get-Date
"; seed $Seed build $Build steps $Steps started $started" | Out-File -Encoding utf8 $logFile

while ($true) {
    if ($Replay) {
        if ($replayAt -ge $replayLines.Count) { break }
        # A replayed log is played a step at a time: each "# step" marker starts a batch.
        $batch = @()
        while ($replayAt -lt $replayLines.Count) {
            $l = $replayLines[$replayAt]; $replayAt++
            if ($l -match "^# step") { if ($batch.Count -gt 0) { $replayAt--; break } else { continue } }
            $batch += $l
        }
    }
    else {
        if ($step -ge $Steps) { break }
        $size = [NqWin]::ClientSize($hwnd)
        $batch = Next-Batch $size[0] $size[1]
    }
    $step++
    Add-Content -Encoding utf8 $logFile "# step $step"
    Add-Content -Encoding utf8 $logFile $batch

    # Resizes go to the window; the rest to the drive file, written whole then renamed into place.
    $driveLines = @()
    foreach ($l in $batch) {
        if ($l -match "^#resize (\d+) (\d+)") { [NqWin]::ResizeClient($hwnd, [int]$Matches[1], [int]$Matches[2]) }
        else { $driveLines += $l }
    }
    if ($driveLines.Count -gt 0) { Send-Drive $driveLines }

    # Taken within HangSeconds, or the UI thread is stuck (or the process is gone).
    $deadline = (Get-Date).AddSeconds($HangSeconds)
    while ((Drive-Waiting) -and -not $p.HasExited -and (Get-Date) -lt $deadline) {
        $dialogs = [NqWin]::CloseDialogs([uint32]$p.Id)
        foreach ($d in $dialogs) { Add-Content -Encoding utf8 $logFile "; dialog closed: $d"; Write-Output "step ${step}: dialog closed: $d" }
        Start-Sleep -Milliseconds 15
    }
    if ($p.HasExited) { $result = "crash"; $detail = ("exit code 0x{0:X8}" -f $p.ExitCode); break }
    if (Drive-Waiting) {
        $result = "hang"; $detail = "the UI thread did not take the drive file within $HangSeconds s"
        $cdbExe = Find-Cdb
        if ($cdbExe) {
            try {
                # Non-invasive (-pv), so the process is left as it was; symbols from the build.
                $env:_NT_SYMBOL_PATH = Split-Path -Parent $exe
                # Out-File utf8: Windows PowerShell's > writes UTF-16, which grep cannot read.
                & $cdbExe -pv -p $p.Id -c "~*kn 40; qd" 2>&1 | Out-File -Encoding utf8 (Join-Path $out "hang-stacks.txt")
                $detail += "; stacks in hang-stacks.txt"
            }
            catch { $detail += "; cdb failed: $_" }
        }
        break
    }
    foreach ($d in [NqWin]::CloseDialogs([uint32]$p.Id)) { Add-Content -Encoding utf8 $logFile "; dialog closed: $d"; Write-Output "step ${step}: dialog closed: $d" }
    if ($StepMs -gt 0) { Start-Sleep -Milliseconds $StepMs }
    if ($step % 100 -eq 0) { Write-Output ("step {0} ({1:N0} s)" -f $step, ((Get-Date) - $started).TotalSeconds) }
}

# The last few batches, settled, then a final check that the UI still answers.
if ($result -eq "ok") {
    Start-Sleep -Seconds 2
    Send-Drive @("shot $out\final.png")
    $deadline = (Get-Date).AddSeconds($HangSeconds)
    while ((Drive-Waiting) -and -not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
    if ($p.HasExited) { $result = "crash"; $detail = ("exit code 0x{0:X8} at the end" -f $p.ExitCode) }
    elseif (Drive-Waiting) { $result = "hang"; $detail = "no answer at the end" }
}

$asan = Get-ChildItem $out -Filter "asan*" -ErrorAction SilentlyContinue
if ($asan) {
    if ($result -eq "ok") { $result = "asan" }
    $detail += "; ASan report: $($asan[0].FullName)"
}
Stop-Run
Start-Sleep -Milliseconds 300
$stderrText = if (Test-Path $stderrPath) { Get-Content $stderrPath -Raw } else { "" }
if ($stderrText -match "(?i)assert|ERROR: AddressSanitizer") { $detail += "; stderr has an assert or ASan report: $stderrPath" }

$summary = "$result after $step steps (seed $Seed, build $Build) $detail"
Add-Content -Encoding utf8 $logFile "; result: $summary"
Write-Output $summary
Write-Output "log: $logFile"
if ($result -ne "ok") { exit 1 }
exit 0
