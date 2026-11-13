<#
.SYNOPSIS
  Genera resources/zombiewood_cursor.bin (cursor del juego) desde una imagen PNG con transparencia.
.DESCRIPTION
  Toma la figura de la mitad izquierda de la imagen (o toda si -Whole), recorta al contenido opaco, calcula el
  "hotspot" (punta de la flecha = pixel opaco mas arriba-izquierda) y escribe varios tamanos (32/48/64/96 px) en un
  binario simple que CMake incrusta en zombiewood.exe (recurso RCDATA 102). El .bin no se versiona (.gitignore).
.EXAMPLE
  pwsh tools/make_cursor.ps1 -Source cursor_icon.png
#>
param(
  [Parameter(Mandatory)] [string] $Source,
  [string] $Output = (Join-Path $PSScriptRoot '..\resources\zombiewood_cursor.bin'),
  [switch] $Whole,
  [int[]] $Sizes = @(32, 48, 64, 96)
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Bitmap]::new((Resolve-Path $Source).Path)
try {
  $limitX = if ($Whole) { $src.Width } else { [int]($src.Width / 2) }
  # Recorte al contenido opaco (alpha > 16) de la zona elegida.
  $minX = $limitX; $minY = $src.Height; $maxX = -1; $maxY = -1
  $tipScore = [int]::MaxValue; $tipX = 0; $tipY = 0
  for ($y = 0; $y -lt $src.Height; $y++) {
    for ($x = 0; $x -lt $limitX; $x++) {
      if ($src.GetPixel($x, $y).A -gt 16) {
        if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
        if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
        if ($x + $y -lt $tipScore) { $tipScore = $x + $y; $tipX = $x; $tipY = $y }
      }
    }
  }
  if ($maxX -lt 0) { throw "No hay pixeles opacos en la imagen" }
  $w = $maxX - $minX + 1; $h = $maxY - $minY + 1; $side = [Math]::Max($w, $h)
  Write-Host "Figura: ${w}x${h} en ($minX,$minY); punta en ($tipX,$tipY)"

  $entries = foreach ($s in $Sizes) {
    $bmp = [System.Drawing.Bitmap]::new($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $scale = $s / $side   # figura anclada arriba-izquierda (la punta queda cerca de 0,0)
    $g.DrawImage($src, [System.Drawing.RectangleF]::new(0, 0, $w * $scale, $h * $scale),
                 [System.Drawing.RectangleF]::new($minX, $minY, $w, $h), [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $rect = [System.Drawing.Rectangle]::new(0, 0, $s, $s)
    $data = $bmp.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($s * $s * 4)
    for ($row = 0; $row -lt $s; $row++) {   # stride puede tener relleno
      [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $row * $data.Stride), $bytes, $row * $s * 4, $s * 4)
    }
    $bmp.UnlockBits($data); $bmp.Dispose()
    $hx = [Math]::Min($s - 1, [int][Math]::Round(($tipX - $minX) * $scale))
    $hy = [Math]::Min($s - 1, [int][Math]::Round(($tipY - $minY) * $scale))
    , @($s, $hx, $hy, $bytes)
  }

  # Formato: "ZWCU" u32 version=1, u32 n, n x {u32 size, u32 hot_x, u32 hot_y, u32 offset}, pixeles BGRA (ARGB8888 LE).
  $ms = [System.IO.MemoryStream]::new(); $bw = [System.IO.BinaryWriter]::new($ms)
  $bw.Write([byte[]][char[]]'ZWCU'); $bw.Write([uint32]1); $bw.Write([uint32]$entries.Count)
  $offset = 12 + 16 * $entries.Count
  foreach ($e in $entries) { $bw.Write([uint32]$e[0]); $bw.Write([uint32]$e[1]); $bw.Write([uint32]$e[2]); $bw.Write([uint32]$offset); $offset += $e[3].Length }
  foreach ($e in $entries) { $bw.Write([byte[]]$e[3]) }
  $bw.Flush()
  New-Item -ItemType Directory -Force (Split-Path -Parent $Output) | Out-Null
  [System.IO.File]::WriteAllBytes($Output, $ms.ToArray())
  Write-Host "Cursor escrito: $((Resolve-Path $Output).Path) ($($ms.Length) bytes; tamanos $($Sizes -join '/'))"
  foreach ($e in $entries) { Write-Host ("  {0}px hotspot ({1},{2})" -f $e[0], $e[1], $e[2]) }
}
finally { $src.Dispose() }
