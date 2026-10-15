# Compara los parámetros con los que se generaron dos G-codes de PrusaSlicer
# (por ejemplo, el mismo modelo cortado en 2.9 y en 3.0) para entender por qué
# uno imprime más lento que el otro.
#
# Uso:
#   powershell -ExecutionPolicy Bypass -File compare_gcode_configs.ps1 -A modelo_29.gcode -B modelo_30.gcode
#
# PrusaSlicer escribe al final de cada G-code un bloque "; prusaslicer_config = begin ... end"
# con todos los parámetros; este script lee ese bloque en los dos archivos y muestra:
#   1. Tiempo estimado, filamento y capas de cada archivo.
#   2. Los parámetros que influyen en la velocidad y que cambiaron.
#   3. El resto de parámetros que cambiaron o que solo existen en una versión.
# Guarda además el informe completo en un .txt junto al archivo B.
#
# Los G-codes deben ser de texto (.gcode). Si tienes .bgcode, desactiva
# "Supports binary G-code" en la impresora y vuelve a exportar.

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B
)

$ErrorActionPreference = "Stop"

function Read-GCodeInfo([string]$path) {
    if (-not (Test-Path $path)) { throw "No existe el archivo: $path" }
    $info = [ordered]@{ Path = (Resolve-Path $path).Path; Config = @{}; Stats = [ordered]@{}; Layers = 0; Brick = $false }
    $inConfig = $false
    foreach ($line in [System.IO.File]::ReadLines($info.Path)) {
        if ($line.Length -eq 0) { continue }
        if ($line -eq "; prusaslicer_config = begin") { $inConfig = $true; continue }
        if ($line -eq "; prusaslicer_config = end") { $inConfig = $false; continue }
        if ($inConfig) {
            $m = [regex]::Match($line, '^;\s*([^=]+?)\s*=\s?(.*)$')
            if ($m.Success) { $info.Config[$m.Groups[1].Value] = $m.Groups[2].Value }
            continue
        }
        if (-not $info.Brick -and $line.Contains("BRICK")) { $info.Brick = $true }
        if ($line[0] -ne ';') { continue }
        if ($line -eq ";LAYER_CHANGE") { $info.Layers++; continue }
        $s = [regex]::Match($line, '^;\s*(estimated printing time \(normal mode\)|estimated printing time \(silent mode\)|filament used \[mm\]|filament used \[g\]|total layers count)\s*=\s*(.*)$')
        if ($s.Success) { $info.Stats[$s.Groups[1].Value] = $s.Groups[2].Value.Trim() }
    }
    if ($info.Config.Count -eq 0) {
        throw "No se encontró el bloque '; prusaslicer_config' en $path. ¿Es un G-code de texto exportado por PrusaSlicer?"
    }
    return $info
}

# Palabras que identifican parámetros relacionados con velocidad / tiempo de impresión
$speedPattern = 'speed|accel|jerk|feedrate|machine_max|machine_limits|slowdown|cooling|min_layer_time|layer_time|fan_below|silent_mode|travel|retract|wipe|z_hop|lift|ramping|avoid_crossing|seam|overhang|dynamic|pressure|linear_advance|perimeters$|top_solid|bottom_solid|fill_density|fill_pattern|infill_every|layer_height|extrusion_width|nozzle_diameter|gcode_flavor|arc_fitting|resolution|brick_layers'

Write-Host "Leyendo A: $A" -ForegroundColor Cyan
$ia = Read-GCodeInfo $A
Write-Host "Leyendo B: $B" -ForegroundColor Cyan
$ib = Read-GCodeInfo $B

$report = New-Object System.Collections.Generic.List[string]
function Out-Line([string]$text, [string]$color = "Gray") {
    Write-Host $text -ForegroundColor $color
    $report.Add($text)
}

Out-Line ""
Out-Line "== Resumen" "Cyan"
Out-Line ("{0,-42} {1,-28} {2}" -f "", "A", "B")
$statKeys = @($ia.Stats.Keys) + @($ib.Stats.Keys) | Select-Object -Unique
foreach ($k in $statKeys) {
    Out-Line ("{0,-42} {1,-28} {2}" -f $k, $ia.Stats[$k], $ib.Stats[$k]) "White"
}
Out-Line ("{0,-42} {1,-28} {2}" -f "capas (incluye intermedias de Brick)", $ia.Layers, $ib.Layers) "White"
Out-Line ("{0,-42} {1,-28} {2}" -f "Brick Layers aplicado", $ia.Brick, $ib.Brick) "White"
if ($ia.Brick -ne $ib.Brick) {
    Out-Line "  -> Solo uno de los dos tiene Brick Layers: agrega recorridos y capas intermedias, así que la comparación de tiempos no es justa." "Yellow"
}
foreach ($v in @("printer_settings_id", "print_settings_id", "filament_settings_id")) {
    if ($ia.Config.ContainsKey($v) -or $ib.Config.ContainsKey($v)) {
        Out-Line ("{0,-42} {1,-28} {2}" -f $v, $ia.Config[$v], $ib.Config[$v])
    }
}

$allKeys = @($ia.Config.Keys) + @($ib.Config.Keys) | Sort-Object -Unique
$changed = @(); $onlyA = @(); $onlyB = @()
foreach ($k in $allKeys) {
    $inA = $ia.Config.ContainsKey($k); $inB = $ib.Config.ContainsKey($k)
    if ($inA -and $inB) { if ($ia.Config[$k] -ne $ib.Config[$k]) { $changed += $k } }
    elseif ($inA) { $onlyA += $k } else { $onlyB += $k }
}
$ignore = '^(print_settings_id|printer_settings_id|filament_settings_id|physical_printer_settings_id|inherits.*|compatible_.*|post_process|output_filename_format|notes|printer_notes|filament_notes|.*_gcode|thumbnails.*)$'
$speedChanged = $changed | Where-Object { $_ -match $speedPattern -and $_ -notmatch $ignore }
$otherChanged = $changed | Where-Object { $_ -notmatch $speedPattern -and $_ -notmatch $ignore }
$gcodeChanged = $changed | Where-Object { $_ -match '_gcode$' }

function Show-Value([string]$v) {
    if ($null -eq $v) { return "" }
    if ($v.Length -gt 60) { return $v.Substring(0, 57) + "..." }
    return $v
}

Out-Line ""
Out-Line "== Parámetros que afectan la velocidad y cambiaron ($($speedChanged.Count))" "Cyan"
if ($speedChanged.Count -eq 0) { Out-Line "  (ninguno)" } else {
    Out-Line ("  {0,-45} {1,-30} {2}" -f "parámetro", "A", "B")
    foreach ($k in $speedChanged) { Out-Line ("  {0,-45} {1,-30} {2}" -f $k, (Show-Value $ia.Config[$k]), (Show-Value $ib.Config[$k])) "Yellow" }
}

Out-Line ""
Out-Line "== Otros parámetros que cambiaron ($($otherChanged.Count))" "Cyan"
foreach ($k in $otherChanged) { Out-Line ("  {0,-45} {1,-30} {2}" -f $k, (Show-Value $ia.Config[$k]), (Show-Value $ib.Config[$k])) }

if ($gcodeChanged.Count -gt 0) {
    Out-Line ""
    Out-Line "== G-code personalizado que cambió (start/end/layer...)" "Cyan"
    foreach ($k in $gcodeChanged) { Out-Line "  $k" }
}

Out-Line ""
Out-Line "== Solo en A ($($onlyA.Count))" "Cyan"
foreach ($k in $onlyA) { Out-Line ("  {0,-45} {1}" -f $k, (Show-Value $ia.Config[$k])) }
Out-Line ""
Out-Line "== Solo en B ($($onlyB.Count))" "Cyan"
foreach ($k in $onlyB) { Out-Line ("  {0,-45} {1}" -f $k, (Show-Value $ib.Config[$k])) }

$outFile = [System.IO.Path]::ChangeExtension($ib.Path, $null).TrimEnd('.') + "_vs_" + [System.IO.Path]::GetFileNameWithoutExtension($ia.Path) + ".txt"
$header = @("A = $($ia.Path)", "B = $($ib.Path)", "")
[System.IO.File]::WriteAllLines($outFile, [string[]]($header + $report), (New-Object System.Text.UTF8Encoding($true)))
Write-Host ""
Write-Host "Informe completo guardado en: $outFile" -ForegroundColor Green
