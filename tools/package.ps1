<#
.SYNOPSIS
  Genera el release Win64 de Zombiewood desde cero: configura + compila con el preset win-release,
  pasa ctest, copia el exe y las DLL a Zombiewood-Win64/ y crea Zombiewood-Win64-<version>.zip.

.PARAMETER DistDir
  Carpeta de salida (por defecto C:\Users\chdut\ZombiewoodDev\dist).
.PARAMETER BuildDir
  Directorio de build (por defecto el binaryDir del preset: ...\ZombiewoodDev\build\win-release).
.PARAMETER Clean
  Borra BuildDir antes de configurar (build realmente desde cero; tarda 10-20 min la primera vez).
.PARAMETER SkipBuild
  No compila: empaqueta lo que ya hay en BuildDir.
.PARAMETER SkipTests
  No ejecuta ctest.

El paquete NO contiene datos del juego: solo zombiewood.exe, SDL2.dll, libEGL.dll, libGLESv2.dll, z.dll, el runtime
de VC++ (msvcp140/vcruntime140/vcruntime140_1),
gamecontrollerdb.txt, readme.txt y licencias de terceros.
#>
[CmdletBinding()]
param(
  [string]$DistDir  = 'C:\Users\chdut\ZombiewoodDev\dist',
  [string]$BuildDir = 'C:\Users\chdut\ZombiewoodDev\build\win-release',
  [switch]$Clean,
  [switch]$SkipBuild,
  [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $PSScriptRoot            # Zombiewood/
$pkgSrc  = Join-Path $PSScriptRoot 'package'           # tools/package/
$vsBase  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$vcvars  = Join-Path $vsBase 'VC\Auxiliary\Build\vcvars64.bat'
$cmakeDir = Join-Path $vsBase 'Common7\IDE\CommonExtensions\Microsoft\CMake'

# Version desde CMakeLists (project(zombiewood VERSION x.y.z))
$version = '0.1.0'
if ((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw) -match 'project\(zombiewood VERSION ([0-9.]+)') { $version = $Matches[1] }
$stamp = Get-Date -Format 'yyyyMMdd'
$name  = 'Zombiewood-Win64'
$stage = Join-Path $DistDir $name
$zip   = Join-Path $DistDir "$name-$version.zip"

function Invoke-Native([string]$what, [scriptblock]$cmd) {
  & $cmd
  if ($LASTEXITCODE -ne 0) { throw "$what fallo (codigo $LASTEXITCODE)" }
}

# ---- 1. Entorno MSVC (vcvars64 + cmake/ninja de Build Tools) ----
if (-not $SkipBuild) {
  if (-not (Test-Path $vcvars)) { throw "No se encuentra vcvars64.bat: $vcvars (instala VS 2022 Build Tools con C++)" }
  $envDump = cmd /c "`"$vcvars`" >nul && set"
  foreach ($l in $envDump) { if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] } }
  $env:PATH = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"

  if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "== Borrando $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
  }
  Push-Location $root
  try {
    Write-Host '== Configurando (preset win-release)'
    Invoke-Native 'cmake --preset' { cmake --preset win-release -B $BuildDir }
    Write-Host '== Compilando'
    Invoke-Native 'cmake --build' { cmake --build $BuildDir }
    if (-not $SkipTests) {
      Write-Host '== Pruebas (ctest)'
      Invoke-Native 'ctest' { ctest --test-dir $BuildDir --output-on-failure }
    }
  } finally { Pop-Location }
}

# ---- 2. Montar la carpeta ----
$exeDir = Join-Path $BuildDir 'src\frontend'
$files  = 'zombiewood.exe', 'SDL2.dll', 'libEGL.dll', 'libGLESv2.dll', 'z.dll'
foreach ($f in $files) { if (-not (Test-Path (Join-Path $exeDir $f))) { throw "Falta $f en $exeDir (compila primero)" } }

Write-Host "== Montando $stage"
if (Test-Path $stage) {
  # La carpeta de staging se regenera entera; si alguien jugo desde ella (data/saves), no borrarla.
  $userState = 'data', 'saves', 'saves_backup', 'zombiewood.json' | Where-Object { Test-Path (Join-Path $stage $_) }
  if ($userState) {
    throw "$stage contiene datos de usuario ($($userState -join ', ')). Muevelos a otra carpeta de juego o borralos a mano; no se sobrescribe."
  }
  Remove-Item -Recurse -Force $stage
}
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $exeDir $f) $stage }
Copy-Item (Join-Path $pkgSrc 'gamecontrollerdb.txt') $stage

# Runtime de Visual C++ (app-local, redistribuible por Microsoft): sin esto el exe no abre en PCs sin el VC++ Redist.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem (Join-Path $vsPath 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue |
       Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "No encontre VC\Redist\MSVC\*\x64\Microsoft.VC14x.CRT en $vsPath" }
foreach ($d in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item (Join-Path $crt.FullName $d) $stage }
Write-Host "   CRT de VC++ desde $($crt.FullName)"
(Get-Content (Join-Path $pkgSrc 'readme.txt') -Raw).Replace('@VERSION@', $version) | Set-Content -Encoding UTF8 (Join-Path $stage 'readme.txt')

# Licencias de terceros (vcpkg share/*/copyright + LICENSE de los FetchContent)
$lic = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $lic | Out-Null
$share = Join-Path $BuildDir 'vcpkg_installed\x64-windows\share'
foreach ($p in 'sdl2', 'angle', 'zlib', 'nlohmann-json') {
  $c = Join-Path $share "$p\copyright"
  if (Test-Path $c) { Copy-Item $c (Join-Path $lic "$p.txt") } else { Write-Warning "sin licencia de $p" }
}
$deps = Join-Path $BuildDir '_deps'
foreach ($pair in @(@('imgui_src-src', 'imgui'), @('dynarmic-src', 'dynarmic'))) {
  $f = Get-ChildItem (Join-Path $deps $pair[0]) -File | Where-Object { $_.Name -match '^LICEN[SC]E(\.txt|\.md)?$' } | Select-Object -First 1
  if ($f) { Copy-Item $f.FullName (Join-Path $lic "$($pair[1]).txt") } else { Write-Warning "sin licencia de $($pair[1])" }
}
Set-Content -Encoding UTF8 (Join-Path $lic 'gamecontrollerdb.txt') 'SDL_GameControllerDB (mdqinc) - licencia zlib: https://github.com/mdqinc/SDL_GameControllerDB/blob/master/LICENSE'
Copy-Item (Join-Path $root 'README.md') (Join-Path $stage 'README.md')

# Comprobacion: ningun dato del juego
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.apk', '.obb', '.so', '.mp4', '.bar', '.dex' }
if ($bad) { throw "El paquete contiene datos del juego: $($bad.FullName -join ', ')" }

# ---- 3. Zip ----
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$h = (Get-FileHash $zip -Algorithm SHA256).Hash
Set-Content -Encoding ASCII "$zip.sha256" "$h  $(Split-Path -Leaf $zip)"

Write-Host ''
Write-Host "Carpeta : $stage"
Get-ChildItem $stage -Recurse -File | ForEach-Object { '  {0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($stage.Length + 1) }
Write-Host ("Zip     : {0}  ({1:N0} bytes)" -f $zip, (Get-Item $zip).Length)
Write-Host "SHA-256 : $h"
