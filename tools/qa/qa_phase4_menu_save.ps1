# QA fase 4: menu F1 con CLICS REALES de raton (Graficos > Mostrar FPS), clic real en una escena del juego, partida corta,
# cierre de la ventana (guardado) y recarga. Requiere una partida con progreso en <Qa>\saves.
param([string]$Qa = "C:\Users\chdut\ZombiewoodDev\t10qa")
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\qa_lib.ps1"
Qa-Init $Qa
function Cfg { Get-Content "$Qa\zombiewood.json" -Raw | ConvertFrom-Json }
function Hashes { (Get-ChildItem "$Qa\saves" -File | Where-Object { $_.Name -match "^(player|level|zomb)\.dat$" } | Get-FileHash -Algorithm MD5 | ForEach-Object { "{0}={1}" -f (Split-Path -Leaf $_.Path), $_.Hash.Substring(0, 8) }) -join " " }
"show_fps inicial: $((Cfg).show_fps)"; "saves antes: $(Hashes)"
Qa-Start
Start-Sleep -Seconds 14; [W]::Tap(0x20); Start-Sleep -Seconds 12
Qa-Shot "p4_01_menu_juego" | Out-Null
[W]::Tap(0x70); Start-Sleep -Seconds 1
Qa-MouseTo 0.098 0.014; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Milliseconds 500
Qa-MouseTo 0.105 0.118; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Milliseconds 800     # Mostrar FPS
"show_fps tras clic real en el menu: $((Cfg).show_fps)"; Qa-Shot "p4_02_fps_on" | Out-Null
Qa-MouseTo 0.146 0.014; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Milliseconds 600; Qa-Shot "p4_03_menu_audio" | Out-Null
Qa-MouseTo 0.098 0.014; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Milliseconds 400
Qa-MouseTo 0.105 0.118; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Milliseconds 800     # Mostrar FPS otra vez (apaga)
"show_fps tras segundo clic: $((Cfg).show_fps)"
[W]::Tap(0x70); Start-Sleep -Seconds 1                                                                         # cierra el menu
# clic real sobre la escena 1 (pantalla de peliculas)
Qa-MouseTo 0.638 0.214; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Seconds 4; Qa-Shot "p4_04_tras_clic_escena" | Out-Null
Qa-MouseTo 0.46 0.90; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Seconds 8     # START del guion
Qa-Shot "p4_05_escena" | Out-Null
$end = (Get-Date).AddSeconds(90); $n = 0
$dirs = @(@(0x44, 0x27), @(0x57, 0x26), @(0x41, 0x25), @(0x53, 0x28))
while ((Get-Date) -lt $end) { $d = $dirs[$n++ % 4]; [W]::Down($d[0]); [W]::Down($d[1]); Start-Sleep -Milliseconds 2200; [W]::Up($d[1]); [W]::Up($d[0]) }
Qa-Shot "p4_06_partida" | Out-Null
[W]::Tap(0x1B); Start-Sleep -Seconds 2; Qa-Shot "p4_07_pausa_esc" | Out-Null          # Esc = BACK -> pausa/QUIT del juego
[W]::Tap(0x1B); Start-Sleep -Seconds 2; Qa-Shot "p4_07b_esc2" | Out-Null
"cierre: $(Qa-Stop)"
"saves despues: $(Hashes)"
"saves_backup: $((Get-ChildItem "$Qa\saves_backup" -Directory).Count) carpetas; config_keep: $((Cfg).backups_keep)"
# recarga
Qa-Start; Start-Sleep -Seconds 14; [W]::Tap(0x20); Start-Sleep -Seconds 12; Qa-Shot "p4_08_recarga" | Out-Null
"recarga: ventana '$($script:Proc.MainWindowTitle)'"; Qa-Stop
