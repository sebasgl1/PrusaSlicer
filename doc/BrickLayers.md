# Brick Layers en PrusaSlicer 3.0 (fork)

Fork de PrusaSlicer `version_3.0.0-alpha12` que integra **Brick Layers** de Geek Detour
(<https://github.com/GeekDetour/BrickLayers>, GPL-3.0) directamente en el slicer.

## Qué hace

Cada otro loop de perímetro interno se imprime media capa más arriba. Las cuentas de la
pared pasan de estar apiladas (rectangular) a trabadas (hexagonal), como ladrillos, lo que
aumenta la resistencia entre capas.

Ventajas frente al script de post-procesado:

- **El preview muestra el resultado** al instante (el script obligaba a re-arrastrar el G-code).
- Las **estimaciones de tiempo y filamento** incluyen los perímetros reubicados.
- Diseñado para funcionar con **G-code binario** (el script exige desactivarlo), porque la
  transformación ocurre antes de la codificación. Pendiente de verificar en una compilación real.
- No requiere Python instalado.

## Opciones (Walls & Perimeters → Walls quality)

| Opción | Clave | Defecto |
|---|---|---|
| Brick layers | `brick_layers` | desactivado |
| Empezar en la capa | `brick_layers_start_layer` | 3 |
| Multiplicador de extrusión | `brick_layers_extrusion_multiplier` | 1.05 |

Requisitos: **Arc fitting desactivado** (el slicer muestra un error si ambos están activos).
Recomendado: orden de perímetros interior → exterior (el defecto). No pensado para impresión
secuencial (complete objects).

## Cómo está hecho

| Archivo | Papel |
|---|---|
| `src/libslic3r/src/libslic3r/GCode/BrickLayers.{hpp,cpp}` | Port en C++ del algoritmo. Solo usa la STL. |
| `src/libslic3r/src/libslic3r/GCode.{hpp,cpp}` | `GCodeOutputStream` pasa el texto por BrickLayers antes del `Processor`. |
| `src/slic3r-domain/src/Slic3r/Domain/ConfigDefsFDM.cpp` | Definición de las 3 opciones. |
| `src/libslic3r/src/libslic3r/StepsInvalidation.cpp` | Cambiarlas solo regenera el G-code (no re-slicea). |
| `src/libslic3r/src/libslic3r/Print.cpp`, `SlicingStatus.hpp`, `DisplayStrings.cpp` | Error de validación con Arc fitting. |
| `tests/bricklayers/` | Driver independiente y script de comparación contra `bricklayers.py`. |

El port sigue el script línea por línea (incluido el formato `repr()` de Python para los
números) y produce **exactamente los mismos bytes** que `bricklayers.py` v0.2.1-10-g6409588.
Se valida con:

```bash
git clone https://github.com/GeekDetour/BrickLayers.git /tmp/BrickLayers
tests/bricklayers/compare.sh /tmp/BrickLayers
```

## Compilar en Windows

Requisitos: Visual Studio 2022 (carga "Desarrollo para el escritorio con C++"), CMake y git.
Desde "x64 Native Tools Command Prompt for VS 2022", en la raíz del repo:

```cmd
cmake -S deps -B deps/build -G "Visual Studio 17 2022" -A x64
cmake --build deps/build --config Release --parallel
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%CD%/deps/build/destdir/usr/local" -DSLIC3R_BUILD_TESTS=OFF -DSLIC3R_PCH=OFF
cmake --build build --config Release --parallel
```

Las dependencias tardan horas la primera vez. Usa una ruta corta y sin espacios
(p. ej. `C:\src\ps3`) para evitar errores de longitud de ruta.

Alternativa sin compilar localmente: subir esta rama a un fork en GitHub; el workflow
`.github/workflows/build_windows_bricklayers.yml` compila y publica el ejecutable como artefacto.
Los workflows originales de Prusa fallarán en un fork (usan un repo privado); se pueden
desactivar desde la pestaña Actions.

## Actualizar a una nueva alpha

Todo el algoritmo vive en `BrickLayers.{hpp,cpp}`; el resto son ~110 líneas en 7 archivos.
Para pasar a la siguiente alpha: `git fetch --tags upstream && git rebase --onto version_3.0.0-alphaNN version_3.0.0-alpha12 bricklayers`.

## Licencia

PrusaSlicer es AGPL-3.0; Brick Layers es GPL-3.0-or-later. La sección 13 de la GPLv3 permite
combinarlos; el conjunto se distribuye bajo AGPL-3.0.

Sobre patentes: la patente estadounidense US11331848B2 (Addman) cubre una técnica similar en
EE.UU. No aplica fuera de ese país; revísalo antes de distribuir binarios a usuarios en EE.UU.
