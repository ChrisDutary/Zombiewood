# QA fase 1: descomprime el zip del release en una carpeta vacia, arranque limpio, instalacion desde el APK con
# dialogo de Windows real, logo+intro, tutorial, F1. Mide tiempos desde el log.
param([string]$Zip = "C:\Users\chdut\ZombiewoodDev\dist\Zombiewood-Win64-0.1.0.zip",
      [string]$Qa  = "C:\Users\chdut\ZombiewoodDev\t10qa",
      [string]$Apk = "C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk")
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\qa_lib.ps1"
if (Test-Path $Qa) { Remove-Item -Recurse -Force $Qa }
$tmp = "$Qa.unzip"; if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive $Zip $tmp; Move-Item "$tmp\Zombiewood-Win64" $Qa; Remove-Item -Recurse -Force $tmp
Qa-Init $Qa
"contenido: " + ((Get-ChildItem $Qa -Recurse -File | ForEach-Object { $_.FullName.Substring($Qa.Length + 1) }) -join ", ")
$t0 = Get-Date
Qa-Start
"[{0:N1}s] ventana '{1}'" -f ((Get-Date) - $t0).TotalSeconds, $script:Proc.MainWindowTitle
Qa-Shot "1_bienvenida"
[W]::Tap(0x0D); Start-Sleep -Seconds 2                      # Elegir APK -> dialogo de Windows
Qa-Shot "2_dialogo"
Set-Clipboard -Value $Apk; (New-Object -ComObject WScript.Shell).SendKeys("^v"); Start-Sleep -Milliseconds 500
[W]::Tap(0x0D)
$ti = Get-Date
for ($i = 0; $i -lt 120 -and -not (Test-Path "$Qa\data\install.json"); $i++) { Start-Sleep -Milliseconds 250 }
"instalacion: {0:N1}s" -f ((Get-Date) - $ti).TotalSeconds
Start-Sleep -Seconds 1; Qa-Focus; Qa-Shot "3_instalado"
$tp = Get-Date
[W]::Tap(0x0D)                                              # Jugar
Start-Sleep -Seconds 8;  Qa-Shot "4_logo"
Start-Sleep -Seconds 14; Qa-Shot "5_intro"
[W]::Tap(0x20)                                              # saltar video
Start-Sleep -Seconds 20; Qa-Shot "6_juego"
"jugar->juego: {0:N0}s" -f ((Get-Date) - $tp).TotalSeconds
[W]::Tap(0x70); Start-Sleep -Seconds 1; Qa-Shot "7_menu_f1"
Qa-Stop
