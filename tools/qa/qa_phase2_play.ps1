# QA fase 2: partida de N minutos con teclado/raton REALES (mover WASD + disparar con flechas/clic derecho),
# captura cada minuto, volcado PCM del audio (ZW_AUDIO_DUMP) y fps del log. Cierra la ventana al final (guardado).
param([string]$Qa = "C:\Users\chdut\ZombiewoodDev\t10qa", [int]$Minutes = 10)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\qa_lib.ps1"
Qa-Init $Qa
$pcm = "$Qa\audio_dump.pcm"; Remove-Item $pcm -ErrorAction SilentlyContinue
$before = Get-ChildItem "$Qa\saves" -File | Select-Object Name, Length, LastWriteTime
$t0 = Get-Date
Qa-Start -Env @{ ZW_AUDIO_DUMP = $pcm }
"inicio {0:HH:mm:ss}" -f $t0
Start-Sleep -Seconds 14; [W]::Tap(0x20)           # logo (se salta con una tecla)
Start-Sleep -Seconds 12
Qa-Shot "p2_00_inicio" | Out-Null
$dirs = @(@(0x44, 0x27), @(0x57, 0x26), @(0x41, 0x25), @(0x53, 0x28), @(0x44, 0x26), @(0x41, 0x28))   # (mover, disparar)
$end = (Get-Date).AddMinutes($Minutes); $n = 0; $next = (Get-Date).AddSeconds(60); $shots = 0
while ((Get-Date) -lt $end) {
  $d = $dirs[$n % $dirs.Count]; $n++
  [W]::Down($d[0]); [W]::Down($d[1]); Start-Sleep -Milliseconds 2500
  [W]::Up($d[1]); [W]::Up($d[0])
  if ($n % 4 -eq 0) {                                  # disparo con raton: clic derecho mantenido sobre el area de juego
    Qa-MouseTo (0.3 + 0.1 * ($n % 5)) 0.35; [W]::Button($true, $true); Start-Sleep -Milliseconds 1500; [W]::Button($true, $false)
  }
  if ($n % 6 -eq 0) { [W]::Tap(0x20) }                 # aceptar (>>) por si hay dialogos
  if ($n % 9 -eq 0) { Qa-MouseTo 0.5 0.5; [W]::Click($false) }   # toque en el centro (avanzar textos)
  if ((Get-Date) -ge $next) { $shots++; Qa-Shot ("p2_{0:00}_min" -f $shots) | Out-Null; $next = $next.AddSeconds(60) }
}
Qa-Shot "p2_99_final" | Out-Null
$alive = -not $script:Proc.HasExited
"proceso vivo al final: $alive; ciclos de input: $n; duracion {0:N1} min" -f ((Get-Date) - $t0).TotalMinutes
Qa-Stop
"--- saves antes/despues ---"
$before | Format-Table | Out-String
Get-ChildItem "$Qa\saves" -File | Select-Object Name, Length, LastWriteTime | Format-Table | Out-String
"audio dump: {0:N0} bytes" -f (Get-Item $pcm).Length
