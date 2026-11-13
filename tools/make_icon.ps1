<#
.SYNOPSIS
  Genera resources/zombiewood.ico (multi-tamano, PNG embebido) a partir de una imagen cuadrada.
.DESCRIPTION
  Recorta el borde blanco, aplica esquinas redondeadas transparentes y escribe un .ico con
  16/24/32/48/64/128/256 px. El .ico NO se versiona (.gitignore): cada usuario genera el suyo
  desde su propia imagen. Si resources/zombiewood.ico existe, CMake lo incrusta en zombiewood.exe.
.EXAMPLE
  pwsh tools/make_icon.ps1 -Source C:\ruta\icono.jpg
#>
param(
  [Parameter(Mandatory)] [string] $Source,
  [string] $Output = (Join-Path $PSScriptRoot '..\resources\zombiewood.ico'),
  [double] $InsetFraction = 0.008,   # borde blanco a recortar (fraccion del lado)
  [double] $RadiusFraction = 0.085   # radio de las esquinas redondeadas (fraccion del lado)
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Bitmap]::new((Resolve-Path $Source).Path)
try {
  $side = [Math]::Min($src.Width, $src.Height)
  $inset = [int][Math]::Round($side * $InsetFraction)
  $crop = [System.Drawing.Rectangle]::new([int](($src.Width - $side) / 2) + $inset, [int](($src.Height - $side) / 2) + $inset,
                                          $side - 2 * $inset, $side - 2 * $inset)

  # Maestro 512x512 con esquinas redondeadas transparentes (antialias).
  $M = 512
  $master = [System.Drawing.Bitmap]::new($M, $M, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($master)
  $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
  $g.Clear([System.Drawing.Color]::Transparent)
  $r = $M * $RadiusFraction; $d = 2 * $r
  $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
  $path.AddArc(0, 0, $d, $d, 180, 90); $path.AddArc($M - $d, 0, $d, $d, 270, 90)
  $path.AddArc($M - $d, $M - $d, $d, $d, 0, 90); $path.AddArc(0, $M - $d, $d, $d, 90, 90); $path.CloseFigure()
  $brush = [System.Drawing.TextureBrush]::new($src, $crop)
  $brush.ScaleTransform($M / $crop.Width, $M / $crop.Height)
  $g.FillPath($brush, $path)
  $g.Dispose(); $brush.Dispose(); $path.Dispose()

  $sizes = 256, 128, 64, 48, 32, 24, 16
  $images = foreach ($s in $sizes) {
    $bmp = [System.Drawing.Bitmap]::new($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gg = [System.Drawing.Graphics]::FromImage($bmp)
    $gg.InterpolationMode = 'HighQualityBicubic'; $gg.SmoothingMode = 'AntiAlias'; $gg.PixelOffsetMode = 'HighQuality'
    $gg.Clear([System.Drawing.Color]::Transparent)
    $gg.DrawImage($master, 0, 0, $s, $s); $gg.Dispose()
    $ms = [System.IO.MemoryStream]::new(); $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    , @($s, $ms.ToArray())
  }
  $master.Dispose()

  # ICO: ICONDIR + ICONDIRENTRY[n] + PNGs.
  $out = [System.IO.MemoryStream]::new(); $w = [System.IO.BinaryWriter]::new($out)
  $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
  $offset = 6 + 16 * $images.Count
  foreach ($im in $images) {
    $s = $im[0]; $data = $im[1]; $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$data.Length); $w.Write([uint32]$offset)
    $offset += $data.Length
  }
  foreach ($im in $images) { $w.Write([byte[]]$im[1]) }
  $w.Flush()
  $dir = Split-Path -Parent $Output; New-Item -ItemType Directory -Force $dir | Out-Null
  [System.IO.File]::WriteAllBytes($Output, $out.ToArray())
  Write-Host "Icono escrito: $((Resolve-Path $Output).Path) ($($out.Length) bytes, tamanos $($sizes -join '/'))"
}
finally { $src.Dispose() }
