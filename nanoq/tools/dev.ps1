# Stop any running Nano Q, build, report compiler and linker errors only, and optionally run
# the standalone on a throwaway profile and capture its window.
#
#   powershell -File nanoq/tools/dev.ps1 -Profile <dir> [-Shot out.png] [-Resize 1100x720] [-Wait 8] [-NoRun]
param(
    [Parameter(Mandatory = $true)][string]$Profile,
    [string]$Shot = "",
    [string]$Resize = "",
    [int]$Wait = 8,
    [switch]$NoRun,
    [string]$Config = "Release"
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Get-Process -Name "Soundshed Nano Q" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700

Push-Location $root
$out = cmd.exe /c "nanoq\build.cmd $Config" 2>&1 | Out-String
Pop-Location

$lines = $out -split "`r?`n"
$errors = $lines | Where-Object { $_ -match "(error C|error LNK|fatal error|CMake Error|FAILED:)" -and $_ -notmatch "showIncludes" }
if ($errors) {
    $errors | Select-Object -First 30 | ForEach-Object { $_.Substring(0, [Math]::Min(420, $_.Length)) }
    exit 1
}
"build ok"

if (-not $NoRun) {
    $params = @{ Profile = $Profile; Wait = $Wait; Config = $Config }
    if ($Shot -ne "") { $params.Shot = $Shot }
    if ($Resize -ne "") { $params.Resize = $Resize }
    & (Join-Path $PSScriptRoot "run-isolated.ps1") @params
}
