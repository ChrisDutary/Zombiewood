# QA fase 5: jugar la escena 1 ("Body Count") hasta el final con entrada real (max N minutos), pasar por la pantalla de
# resultados, cerrar la ventana y recargar para comprobar que las monedas/progreso persisten. Requiere saves con progreso.
param([string]$Qa = "C:\Users\chdut\ZombiewoodDev\t10qa", [int]$MaxMinutes = 8)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\qa_lib.ps1"
Qa-Init $Qa
function PH { (Get-FileHash "$Qa\saves\player.dat" -Algorithm MD5).Hash.Substring(0, 8) }
"player.dat antes: $(PH)"
Qa-Start
Start-Sleep -Seconds 14; [W]::Tap(0x20); Start-Sleep -Seconds 12
Qa-MouseTo 0.638 0.214; Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Seconds 4
Qa-MouseTo 0.46 0.90;  Start-Sleep -Milliseconds 300; [W]::Click($false); Start-Sleep -Seconds 8
Qa-Shot "p5_01_inicio_escena" | Out-Null
$h0 = PH; $t0 = Get-Date; $end = $t0.AddMinutes($MaxMinutes); $n = 0; $done = $false
$mv = 0x44, 0x57, 0x41, 0x53
$aim = @(@(0x26), @(0x26, 0x25), @(0x25), @(0x25, 0x28), @(0x28), @(0x28, 0x27), @(0x27), @(0x27, 0x26))
while ((Get-Date) -lt $end -and -not $done) {
  $m = $mv[$n % 4]; $a = $aim[$n % 8]; $n++
  [W]::Down($m); foreach ($k in $a) { [W]::Down($k) }
  Start-Sleep -Milliseconds 1800
  foreach ($k in $a) { [W]::Up($k) }; [W]::Up($m)
  if ($n % 15 -eq 0) { Qa-Shot ("p5_t{0:000}" -f [int]((Get-Date) - $t0).TotalSeconds) | Out-Null; if ((PH) -ne $h0) { $done = $true } }
}
"fin del bucle tras {0:N0} s (player.dat cambio: {1})" -f ((Get-Date) - $t0).TotalSeconds, $done
Start-Sleep -Seconds 6; Qa-Shot "p5_90_final" | Out-Null
for ($i = 0; $i -lt 6; $i++) { [W]::Tap(0x20); Start-Sleep -Milliseconds 800; Qa-MouseTo 0.5 0.8; [W]::Click($false); Start-Sleep -Seconds 2; Qa-Shot ("p5_9{0}_resultados" -f ($i + 1)) | Out-Null }
"player.dat tras la escena: $(PH)"
"cierre: $(Qa-Stop)"
Qa-Start; Start-Sleep -Seconds 14; [W]::Tap(0x20); Start-Sleep -Seconds 12; Qa-Shot "p5_99_recarga" | Out-Null
"recarga: '$($script:Proc.MainWindowTitle)'"; Qa-Stop
