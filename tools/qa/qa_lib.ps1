# Ayudas de QA con entrada REAL de Windows (keybd_event / mouse_event / ShowWindow). Se usa con dot-source:
#   . tools\qa\qa_lib.ps1 ; Qa-Init <carpeta-qa>
# Nota: WScript SendKeys NO sirve (teclas sin scancode: SDL las ve como Numlock); aqui se usa keybd_event con scancode.
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public static void Down(byte vk) { uint f = IsExt(vk) ? 1u : 0u; keybd_event(vk, (byte)MapVirtualKey(vk, 0), f, UIntPtr.Zero); }
  public static void Up(byte vk)   { uint f = IsExt(vk) ? 3u : 2u; keybd_event(vk, (byte)MapVirtualKey(vk, 0), f, UIntPtr.Zero); }
  static bool IsExt(byte vk) { return vk >= 0x25 && vk <= 0x28; }   // flechas
  public static void Tap(byte vk) { Down(vk); System.Threading.Thread.Sleep(70); Up(vk); }
  public static void Click(bool right) {
    mouse_event(right ? 8u : 2u, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(80);
    mouse_event(right ? 16u : 4u, 0, 0, 0, UIntPtr.Zero);
  }
  public static void Button(bool right, bool down) { mouse_event(down ? (right ? 8u : 2u) : (right ? 16u : 4u), 0, 0, 0, UIntPtr.Zero); }
}
"@
$script:QaDir = $null; $script:Proc = $null; $script:Hwnd = [IntPtr]::Zero
function Qa-Init([string]$dir) { $script:QaDir = $dir; New-Item -ItemType Directory -Force "$dir\qa_shots" | Out-Null }
function Qa-Start([string[]]$ExtraArgs = @(), [hashtable]$Env = @{}) {
  Remove-Item Env:ZOMBIEWOOD_HOME -ErrorAction SilentlyContinue
  foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
  $script:Proc = Start-Process "$script:QaDir\zombiewood.exe" -ArgumentList $ExtraArgs -WorkingDirectory $script:QaDir -PassThru
  Start-Sleep -Seconds 3; $script:Proc.Refresh(); $script:Hwnd = $script:Proc.MainWindowHandle
  foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
  Qa-Focus
}
function Qa-Focus {
  # Alt "fantasma" para que Windows deje robar el foco
  [W]::Down(0x12); [W]::Up(0x12)
  [W]::ShowWindow($script:Hwnd, 9) | Out-Null
  [W]::SetForegroundWindow($script:Hwnd) | Out-Null; Start-Sleep -Milliseconds 400
}
function Qa-Shot([string]$name) {
  $r = New-Object W+RECT; [W]::GetWindowRect($script:Hwnd, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $h = $r.B - $r.T
  if ($w -le 0 -or $h -le 0) { return "captura $name omitida (ventana minimizada)" }
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  $bmp.Save("$script:QaDir\qa_shots\$name.png"); $g.Dispose(); $bmp.Dispose()
  "captura $name ${w}x${h}"
}
function Qa-ClientPoint([double]$fx, [double]$fy) {   # fraccion del area cliente -> pantalla
  $c = New-Object W+RECT; [W]::GetClientRect($script:Hwnd, [ref]$c) | Out-Null
  $p = New-Object W+POINT; [W]::ClientToScreen($script:Hwnd, [ref]$p) | Out-Null
  @([int]($p.X + $fx * $c.R), [int]($p.Y + $fy * $c.B))
}
function Qa-MouseTo([double]$fx, [double]$fy) { $pt = Qa-ClientPoint $fx $fy; [W]::SetCursorPos($pt[0], $pt[1]) | Out-Null; Start-Sleep -Milliseconds 120 }
function Qa-Stop([int]$waitMs = 20000) {
  $script:Proc.CloseMainWindow() | Out-Null
  if (-not $script:Proc.WaitForExit($waitMs)) { $script:Proc.Kill(); return "kill" }
  "exit code " + $script:Proc.ExitCode
}
function Qa-Log { Get-Content "$script:QaDir\logs\zombiewood.log" -ErrorAction SilentlyContinue }
