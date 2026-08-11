param(
    [string]$DevkitPro = "C:/msys64/opt/devkitpro"
)

$env:DEVKITPRO = $DevkitPro

$msysBash = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path $msysBash)) {
    Write-Error "MSYS2 bash not found at $msysBash. Install MSYS2 at C:\msys64."
    exit 1
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$shScript = Join-Path $scriptDir "build-wii.sh"
$msysScript = ($shScript -replace '\\', '/') -replace '^([A-Za-z]):', {
    '/' + $_.Groups[1].Value.ToLower()
}

& $msysBash -lc "export DEVKITPRO='$DevkitPro'; bash '$msysScript'"
exit $LASTEXITCODE
