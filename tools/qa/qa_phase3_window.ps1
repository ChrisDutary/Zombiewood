# QA fase 3: recarga de partida, pantalla completa (F11 y Alt+Enter), redimensionar, minimizar/restaurar, Alt+Tab.
param([string]$Qa = "C:\Users\chdut\ZombiewoodDev\t10qa")
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\qa_lib.ps1"
Qa-Init $Qa
function Rect { $r = New-Object W+RECT; [W]::GetWindowRect($script:Hwnd, [ref]$r) | Out-Null; "{0}x{1}@{2},{3}" -f ($r.R - $r.L), ($r.B - $r.T), $r.L, $r.T }
function Fg { $sb = New-Object System.Text.StringBuilder 256; [W]::GetWindowText([W]::GetForegroundWindow(), $sb, 256) | Out-Null; $sb.ToString() }
$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; "pantalla: $($scr.Width)x$($scr.Height)"
$t0 = Get-Date
Qa-Start
Start-Sleep -Seconds 14; [W]::Tap(0x20); Start-Sleep -Seconds 12
"cargada partida (arranque+logo): {0:N0}s; ventana {1}" -f ((Get-Date) - $t0).TotalSeconds, (Rect)
Qa-Shot "p3_01_recarga" | Out-Null
# --- F11 ---
[W]::Tap(0x7A); Start-Sleep -Seconds 2; "F11 -> $(Rect)"; Qa-Shot "p3_02_f11" | Out-Null
[W]::Tap(0x7A); Start-Sleep -Seconds 2; "F11 de nuevo -> $(Rect)"
# --- Alt+Enter ---
[W]::Down(0x12); Start-Sleep -Milliseconds 80; [W]::Tap(0x0D); Start-Sleep -Milliseconds 80; [W]::Up(0x12); Start-Sleep -Seconds 2
"Alt+Enter -> $(Rect)"; Qa-Shot "p3_03_altenter" | Out-Null
[W]::Down(0x12); Start-Sleep -Milliseconds 80; [W]::Tap(0x0D); Start-Sleep -Milliseconds 80; [W]::Up(0x12); Start-Sleep -Seconds 2
"Alt+Enter de nuevo -> $(Rect)"
# --- redimensionar (SWP_NOZORDER=4) ---
foreach ($s in @(@(800, 600), @(1600, 900), @(640, 360), @(1280, 720))) {
  [W]::SetWindowPos($script:Hwnd, [IntPtr]::Zero, 100, 60, $s[0], $s[1], 4) | Out-Null; Start-Sleep -Seconds 2
  "resize $($s[0])x$($s[1]) -> $(Rect)"; Qa-Shot ("p3_04_resize_{0}x{1}" -f $s[0], $s[1]) | Out-Null
}
# --- minimizar / restaurar ---
$logBefore = (Qa-Log).Count
[W]::ShowWindow($script:Hwnd, 6) | Out-Null; Start-Sleep -Seconds 4
"minimizada: IsIconic=$([W]::IsIconic($script:Hwnd))"
[W]::ShowWindow($script:Hwnd, 9) | Out-Null; Start-Sleep -Seconds 1; Qa-Focus; Start-Sleep -Seconds 2
"restaurada: IsIconic=$([W]::IsIconic($script:Hwnd)) $(Rect)"; Qa-Shot "p3_05_restaurada" | Out-Null
"log durante minimizar/restaurar:"; (Qa-Log | Select-Object -Skip $logBefore | Where-Object { $_ -match "pause|resume|pausa|reanud|foco|focus|minimi|restor" } | Select-Object -First 10) -join "`n"
# --- Alt+Tab ---
$fg0 = Fg
[W]::Down(0x12); Start-Sleep -Milliseconds 100; [W]::Tap(0x09); Start-Sleep -Milliseconds 400; [W]::Up(0x12); Start-Sleep -Seconds 3
"Alt+Tab: primer plano antes '$fg0' -> ahora '$(Fg)'"
$logMid = (Qa-Log).Count; Start-Sleep -Seconds 6
"fps con la ventana en segundo plano:"; Qa-Log | Select-Object -Skip $logMid | Where-Object { $_ -match "\] fps " } | Select-Object -First 3
[W]::Down(0x12); Start-Sleep -Milliseconds 100; [W]::Tap(0x09); Start-Sleep -Milliseconds 400; [W]::Up(0x12); Start-Sleep -Seconds 2
Qa-Focus; "Alt+Tab de vuelta: '$(Fg)'"; Start-Sleep -Seconds 2; Qa-Shot "p3_06_tras_alttab" | Out-Null
# --- entrada tras todo lo anterior: mover y disparar sigue respondiendo ---
[W]::Down(0x44); [W]::Down(0x27); Start-Sleep -Seconds 2; [W]::Up(0x27); [W]::Up(0x44); Qa-Shot "p3_07_input_ok" | Out-Null
"fps final:"; Qa-Log | Where-Object { $_ -match "\] fps " } | Select-Object -Last 2
Qa-Stop
