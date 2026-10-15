# Revisa si este PC tiene lo necesario para compilar el fork de PrusaSlicer con Brick Layers.
#
# Uso (PowerShell normal, no hace falta admin):
#   powershell -ExecutionPolicy Bypass -File tools\check_build_env.ps1
#   powershell -ExecutionPolicy Bypass -File tools\check_build_env.ps1 -DepsPath C:\d\destdir -SourcePath C:\src\ps3
#
# Solo lee información del sistema; no instala ni cambia nada.

param(
    [string]$DepsPath   = "C:\d\destdir",
    [string]$SourcePath = ""
)

$ErrorActionPreference = "Continue"
$script:fails = 0
$script:warns = 0

function Ok($msg)   { Write-Host "  [OK]    $msg" -ForegroundColor Green }
function Warn($msg, $fix) {
    $script:warns++
    Write-Host "  [AVISO] $msg" -ForegroundColor Yellow
    if ($fix) { Write-Host "          -> $fix" -ForegroundColor DarkYellow }
}
function Fail($msg, $fix) {
    $script:fails++
    Write-Host "  [FALTA] $msg" -ForegroundColor Red
    if ($fix) { Write-Host "          -> $fix" -ForegroundColor DarkYellow }
}
function Section($title) { Write-Host ""; Write-Host "== $title" -ForegroundColor Cyan }

# Versiones con las que se compilaron las dependencias en GitHub Actions
$MinVsVersion   = [version]"17.14"
$MinMsvcToolset = [version]"14.44"
$MinCMake       = [version]"3.21"   # el generador "Visual Studio 17 2022" la exige

Write-Host "Chequeo del entorno para compilar PrusaSlicer + Brick Layers" -ForegroundColor White

# ---------------------------------------------------------------- Sistema
Section "Sistema"
$os = Get-CimInstance Win32_OperatingSystem
if ([Environment]::Is64BitOperatingSystem) { Ok "$($os.Caption) de 64 bits (build $($os.BuildNumber))" } else { Fail "Windows de 32 bits" "Se necesita Windows 10/11 de 64 bits" }

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Ok "CPU: $($cpu.Name.Trim()) - $($cpu.NumberOfCores) núcleos / $($cpu.NumberOfLogicalProcessors) hilos"

$ramGB = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
if ($ramGB -ge 16) { Ok "RAM: $ramGB GB" } elseif ($ramGB -ge 8) { Warn "RAM: $ramGB GB" "Funciona, pero cierra otros programas mientras compila" } else { Fail "RAM: $ramGB GB" "Se recomiendan al menos 16 GB" }

$longPaths = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name LongPathsEnabled -ErrorAction SilentlyContinue).LongPathsEnabled
if ($longPaths -eq 1) { Ok "Rutas largas de Windows habilitadas" } else { Warn "Rutas largas de Windows deshabilitadas" "Usa rutas cortas (C:\src\ps3, C:\d) o habilítalas: en PowerShell como admin, New-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force" }

# ---------------------------------------------------------------- Visual Studio
Section "Visual Studio 2022 y compilador C++"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) {
    $vsJson = & $vswhere -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8 2>$null
    if ($vsJson) {
        # Windows PowerShell 5.1 returns a JSON array as a single object; foreach unrolls it.
        $parsed = ConvertFrom-Json ($vsJson -join "`n")
        $list = foreach ($item in $parsed) { $item }
        $vs = $list | Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
    }
}
if (-not $vs) {
    Fail "No se encontró Visual Studio 2022 con herramientas de C++" "Instala Visual Studio 2022 Community con la carga 'Desarrollo para el escritorio con C++'"
} else {
    $vsVer = [version]$vs.installationVersion
    $vsName = "$($vs.displayName) $($vs.catalog.productDisplayVersion)"
    if ($vsVer -ge $MinVsVersion) { Ok "$vsName" } else { Fail "$vsName (se necesita $MinVsVersion o más nuevo)" "Actualiza Visual Studio desde el Visual Studio Installer" }

    $msvcDir = Join-Path $vs.installationPath "VC\Tools\MSVC"
    $toolsets = Get-ChildItem $msvcDir -Directory -ErrorAction SilentlyContinue | ForEach-Object { try { [version]$_.Name } catch { $null } } | Where-Object { $_ } | Sort-Object -Descending
    if ($toolsets -and $toolsets[0] -ge $MinMsvcToolset) { Ok "Compilador MSVC $($toolsets[0])" } elseif ($toolsets) { Fail "Compilador MSVC $($toolsets[0]) (las dependencias se compilaron con $MinMsvcToolset)" "Actualiza Visual Studio; con un MSVC más viejo el enlazado puede fallar" } else { Fail "No se encontró el compilador MSVC" "Reinstala la carga 'Desarrollo para el escritorio con C++'" }

    $vsCMake = Join-Path $vs.installationPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

# ---------------------------------------------------------------- Windows SDK
Section "Windows SDK (headers WinRT para la reparación de mallas)"
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdk = Get-ChildItem "$kits\Include" -Directory -ErrorAction SilentlyContinue |
       Where-Object { Test-Path "$($_.FullName)\winrt\windows.graphics.printing3d.h" } |
       Sort-Object Name -Descending | Select-Object -First 1
if ($sdk) { Ok "Windows SDK $($sdk.Name) con headers WinRT" } else { Fail "No hay un Windows 10/11 SDK con headers WinRT" "En el Visual Studio Installer, marca 'Windows 11 SDK' en la carga de C++ (o configura con -DSLIC3R_ENABLE_WIN10_MESH_REPAIR=OFF)" }

# ---------------------------------------------------------------- CMake y Git
Section "CMake y Git"
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$cmakePath = if ($cmake) { $cmake.Source } elseif ($vsCMake -and (Test-Path $vsCMake)) { $vsCMake } else { $null }
if ($cmakePath) {
    $firstLine = (& $cmakePath --version) | Select-Object -First 1
    $cmakeVer = [version]"0.0"
    if ($firstLine -match '(\d+\.\d+(\.\d+)?)') { $cmakeVer = [version]$Matches[1] }
    $where = if ($cmake) { "en el PATH" } else { "incluido en Visual Studio (disponible en la 'x64 Native Tools Command Prompt')" }
    if ($cmakeVer -ge $MinCMake) { Ok "CMake $cmakeVer $where" } else { Fail "CMake $cmakeVer (se necesita $MinCMake o más nuevo)" "Actualiza Visual Studio o instala CMake desde cmake.org" }
} else {
    Fail "No se encontró CMake" "Viene con la carga de C++ de Visual Studio, o instálalo desde cmake.org"
}

$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    Ok ((& git --version) -join "")
    $gitLong = (& git config --global --get core.longpaths) 2>$null
    if ($gitLong -eq "true") { Ok "git core.longpaths activado" } else { Warn "git core.longpaths no está activado" "Ejecuta: git config --global core.longpaths true" }
} else {
    Fail "No se encontró Git" "Instala Git para Windows desde git-scm.com"
}

# ---------------------------------------------------------------- Dependencias precompiladas
Section "Dependencias precompiladas ($DepsPath)"
$prefix = Join-Path $DepsPath "usr\local"
if (-not (Test-Path $prefix)) {
    Fail "No existe $prefix" "Descarga el artefacto 'deps-win64-msvc-release' de GitHub Actions y descomprímelo para que quede $prefix"
} else {
    $markers = [ordered]@{
        "Boost"     = "lib\cmake\Boost-1.86.0"
        "wxWidgets" = "lib\cmake\wxWidgets\wxWidgetsConfig.cmake"
        "CGAL"      = "lib\cmake\CGAL\CGALConfig.cmake"
        "TBB"       = "lib\cmake\TBB\TBBConfig.cmake"
        "OpenVDB"   = "include\openvdb\openvdb.h"
        "OCCT"      = "lib\cmake\opencascade"
        "z3"        = "lib\cmake\z3\Z3Config.cmake"
        "Catch2"    = "lib\cmake\Catch2"
    }
    $missing = @()
    foreach ($k in $markers.Keys) {
        $found = Test-Path (Join-Path $prefix $markers[$k])
        if (-not $found -and $k -eq "Boost") { $found = [bool](Get-ChildItem $prefix -Recurse -Filter "version.hpp" -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -match '\\boost$' } | Select-Object -First 1) }
        if (-not $found -and $k -eq "OCCT") { $found = [bool](Get-ChildItem $prefix -Recurse -Filter "OpenCASCADEConfig.cmake" -ErrorAction SilentlyContinue | Select-Object -First 1) }
        if (-not $found) { $missing += $k }
    }
    $libs = (Get-ChildItem (Join-Path $prefix "lib") -Filter *.lib -Recurse -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($missing.Count -eq 0) { Ok "Encontradas ($libs librerías .lib)" } else { Warn "No se encontraron: $($missing -join ', ') ($libs librerías .lib en total)" "Si descomprimiste el artefacto, revisa que no haya quedado una carpeta de más (p. ej. C:\d\destdir\destdir\usr\local)" }
    if ($DepsPath -ne "C:\d\destdir") { Warn "Las dependencias no están en C:\d\destdir" "Algunas configuraciones de CMake guardan la ruta en que se compilaron; usa C:\d\destdir para evitar problemas" }
}

# ---------------------------------------------------------------- Código fuente
if ($SourcePath) {
    Section "Código fuente ($SourcePath)"
    if (Test-Path (Join-Path $SourcePath "src\libslic3r\src\libslic3r\GCode\BrickLayers.cpp")) { Ok "Rama con Brick Layers encontrada" } elseif (Test-Path (Join-Path $SourcePath "CMakeLists.txt")) { Fail "El código no tiene Brick Layers" "Cambia a la rama: git -C $SourcePath checkout bricklayers" } else { Fail "No hay código en $SourcePath" "git clone -b bricklayers https://github.com/sebasgl1/PrusaSlicer.git $SourcePath" }
    if ($SourcePath.Length -gt 20) { Warn "Ruta del código algo larga ($($SourcePath.Length) caracteres)" "Recomendado algo corto como C:\src\ps3" }
}

# ---------------------------------------------------------------- Disco y antivirus
Section "Disco y antivirus"
$checkPath = "C:\"
if ($SourcePath -and (Test-Path $SourcePath)) { $checkPath = (Resolve-Path $SourcePath).Path }
$drive = (Get-Item ([System.IO.Path]::GetPathRoot($checkPath))).PSDrive
$freeGB = [math]::Round((Get-PSDrive $drive.Name).Free / 1GB, 1)
if ($freeGB -ge 30) { Ok "Espacio libre en $($drive.Name):\ $freeGB GB" } elseif ($freeGB -ge 15) { Warn "Espacio libre en $($drive.Name):\ $freeGB GB" "La compilación de PrusaSlicer ocupa ~10-15 GB" } else { Fail "Espacio libre en $($drive.Name):\ $freeGB GB" "Libera espacio: se necesitan al menos 15 GB" }

try {
    $part = Get-Partition -DriveLetter $drive.Name -ErrorAction Stop
    $disk = Get-PhysicalDisk -ErrorAction Stop | Where-Object { $_.DeviceId -eq "$($part.DiskNumber)" }
    if ($disk.MediaType -eq "HDD") { Warn "La unidad $($drive.Name): es un disco mecánico" "Compilar en un SSD/NVMe es mucho más rápido" } elseif ($disk) { Ok "Unidad $($drive.Name): es $($disk.MediaType) ($($disk.BusType))" }
} catch { }

try {
    $mp = Get-MpPreference -ErrorAction Stop
    $exclusions = @($mp.ExclusionPath) | Where-Object { $_ }
    if ($mp.DisableRealtimeMonitoring) { Ok "Protección en tiempo real de Defender desactivada" } elseif ($exclusions | Where-Object { $_ -like "C:\src*" -or $_ -like "C:\d*" -or ($SourcePath -and $SourcePath -like "$_*") }) { Ok "Hay exclusiones de Defender para las carpetas de compilación" } else { Warn "Defender escanea las carpetas de compilación" "Opcional, acelera ~30%: Seguridad de Windows > Virus y amenazas > Exclusiones > agregar C:\src y C:\d" }
} catch {
    Warn "No se pudo leer la configuración de Defender (requiere admin)" "Si quieres, agrega C:\src y C:\d a las exclusiones de Defender"
}

# ---------------------------------------------------------------- Resumen
Write-Host ""
if ($script:fails -eq 0) {
    Write-Host "Todo listo para compilar ($($script:warns) avisos opcionales)." -ForegroundColor Green
    Write-Host "Abre 'x64 Native Tools Command Prompt for VS 2022' y ejecuta:" -ForegroundColor White
    Write-Host "  cmake -S . -B build -G `"Visual Studio 17 2022`" -A x64 -DCMAKE_PREFIX_PATH=`"$($DepsPath -replace '\\','/')/usr/local`" -DSLIC3R_BUILD_TESTS=OFF -DSLIC3R_PCH=OFF"
    Write-Host "  cmake --build build --config Release --parallel"
} else {
    Write-Host "Faltan $($script:fails) cosa(s) y hay $($script:warns) aviso(s). Revisa los puntos marcados arriba." -ForegroundColor Red
}
exit $script:fails
