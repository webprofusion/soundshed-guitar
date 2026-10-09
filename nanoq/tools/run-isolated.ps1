# Starts the Release standalone with a throwaway profile (SOUNDSHED_NANOQ_PROFILE), so a test
# run never touches the real Soundshed profile, waits for it to come up, and optionally captures
# its window. Any earlier Nano Q instance is stopped first.
#
#   powershell -File nanoq/tools/run-isolated.ps1 -Profile <dir> [-Shot out.png] [-Resize 1100x720] [-Wait 8]
param(
    [Parameter(Mandatory = $true)][string]$Profile,
    [string]$Shot = "",
    [string]$Resize = "",
    [int]$Wait = 8,
    [string]$Config = "Release"
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root "nanoq\build\$Config\products\Standalone-nanoq_standalone\Soundshed Nano Q.exe"
if (-not (Test-Path $exe)) { Write-Error "Not built: $exe"; exit 1 }

Get-Process -Name "Soundshed Nano Q" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force $Profile | Out-Null
$env:SOUNDSHED_NANOQ_PROFILE = $Profile
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds $Wait
if ($p.HasExited) { Write-Error "Exited with code $($p.ExitCode)"; exit 1 }
Write-Output "pid $($p.Id)"

if ($Shot -ne "") {
    $args2 = @("-NoProfile", "-File", (Join-Path $PSScriptRoot "window-shot.ps1"), "-Out", $Shot)
    if ($Resize -ne "") { $args2 += @("-Resize", $Resize) }
    & powershell @args2
}
