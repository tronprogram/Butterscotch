# Launch Felk Dolphin with mcp-dolphin bridge for Butterscotch Wii debugging.
param(
  [string]$Dol = "C:\Users\Tron\Butterscotch\build-wii\apps\butterscotch\boot.dol"
)
$felk = "C:\Users\Tron\Downloads\dolphin-felk-scripting\Dolphin.exe"
$bridge = "C:\Users\Tron\Butterscotch\tools\dolphin-mcp\mcp_bridge.py"
Stop-Process -Name Dolphin -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Start-Process -FilePath $felk -ArgumentList @("--script", $bridge, "-e", $Dol)
