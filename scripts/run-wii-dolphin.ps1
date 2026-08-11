param(
    [string]$Dol = ""
)

$dolphin = "$env:USERPROFILE\Downloads\dolphin-master-2606-300-x64\Dolphin-x64\Dolphin.exe"

if (-not (Test-Path $dolphin)) {
    Write-Error "Dolphin not found at $dolphin"
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot

if ($Dol -eq "") {
    $bootDol = "$repoRoot\build-wii\apps\butterscotch\boot.dol"
    $mainDol = "$repoRoot\build-wii\butterscotch.dol"

    if (Test-Path $bootDol) {
        $Dol = $bootDol
    } elseif (Test-Path $mainDol) {
        $Dol = $mainDol
    } else {
        Write-Error "No .dol found. Build first with scripts\build-wii.ps1"
        exit 1
    }
}

Write-Host "Launching Dolphin with: $Dol"
& $dolphin -e $Dol
