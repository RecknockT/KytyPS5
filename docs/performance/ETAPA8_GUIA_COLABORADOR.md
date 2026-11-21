# KytyPS5 — Guía del Colaborador (Etapa 8)
## Reproducción, Compilación y Ejecución de Benchmarks en Astro Bot

Esta guía está diseñada para que cualquier ingeniero o colaborador pueda clonar el repositorio, compilar el proyecto en Windows 11 x64, preparar Astro Bot (`PPSA21567`), ejecutar la batería de pruebas de rendimiento automatizadas y reportar los resultados de manera estandarizada.

---

## A. Requisitos del Entorno

### 1. Sistema Operativo y Hardware
- **Sistema Operativo**: Windows 11 x64 (Build 22000 o superior).
- **Procesador (CPU)**: AMD Ryzen (Zen 4 / Zen 5 recomendado, p. ej. Ryzen 7 7700X / 9700X / 7800X3D / 9800X3D) o Intel Core 13ª/14ª Gen (8 núcleos físicos o más).
- **Tarjeta Gráfica (GPU)**: GPU con soporte completo de Vulkan 1.3 (NVIDIA GeForce RTX 3070 / 4070 / 4080 / 5080 o AMD Radeon RX 6800 / 7800 / 7900 con drivers actualizados).
- **Memoria RAM**: 32 GB DDR4/DDR5 recomendados (mínimo 16 GB).
- **Almacenamiento**: SSD NVMe con al menos 50 GB libres para dependencias de build y archivos de juego.

### 2. Herramientas de Compilación Requeridas
- **Visual Studio 2022** (Community, Professional o Enterprise):
  - Cargas de trabajo: *"Desarrollo para el escritorio con C++"*.
  - Componentes individuales obligatorios:
    - Compilador MSVC v143 (v14.30 o superior, p. ej. 14.44).
    - Windows 10/11 SDK (10.0.22621.0 o superior).
    - Herramientas de CMake para Windows.
- **Compilador Clang/LLVM** (v20 o superior recomendado, o `clang-cl` integrado en LLVM para Windows).
- **Ninja Build System**: Versión 1.11 o superior (disponible a través de Visual Studio o Winget).
- **Vulkan SDK**: Versión **1.3.268.0 o superior** (descargable desde [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)).
- **Git**: Versión 2.30+ con soporte para Git LFS y submódulos.

---

## B. Clonar y Compilar el Proyecto

### 1. Clonar el Repositorio
Abrir PowerShell o la consola de comandos de desarrollador de Visual Studio (*x64 Native Tools Command Prompt for VS 2022*):

```cmd
git clone --recursive https://github.com/RecknockT/KytyPS5.git
cd KytyPS5
```

Si el clon se realizó sin `--recursive`, inicializar los submódulos de terceros:
```cmd
git submodule update --init --recursive
```

### 2. Configurar y Compilar con CMake y Ninja
Se recomienda utilizar el generador **Ninja** y el tipo de compilación **Release**:

```cmd
cmake -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _Build/windows
```

La compilación generará:
- `_Build\windows\kyty_emulator.exe` (ejecutable del emulador)
- `_Build\windows\launcher\kyty_launcher.exe` (interfaz gráfica Qt6)
- Bibliotecas estáticas del motor y pruebas unitarias.

### 3. Ejecutar las Pruebas Unitarias
Para validar que el motor y el sistema de profiling funcionan correctamente:

```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|stream_buffer_ring|emulator_gpu_submit_cadence)"
```
Todos los tests deben pasar exitosamente (100% passed).

> [!IMPORTANT]
> **Ubicación del Ejecutable**: Debe ejecutarse siempre `_Build\windows\kyty_emulator.exe` desde su directorio de compilación o a través del script `stage8_camera_ab.cmd`. **No mueva ni copie únicamente `kyty_emulator.exe` a la raíz del repositorio**, ya que requiere las bibliotecas dinámicas satélite (`SDL3.dll`, librerías Qt6 y capas de Vulkan) ubicadas en `_Build\windows\`.

---

## C. Preparación de Astro Bot (`PPSA21567`)

### 1. Instalación y Ruta
- El juego de referencia es **Astro Bot** (Title ID: `PPSA21567`).
- Obtenga una copia legítima del juego desde su consola PS5 y colóquela desencriptada en una ruta local accesible, por ejemplo:
  `C:\Users\<Usuario>\Downloads\PPSA21567\PPSA21567-app`
- Abra el archivo `stage8_camera_ab.cmd` en un editor de texto y verifique que la variable `GAME_DIR` apunte a su carpeta real del juego:
  ```bat
  set "GAME_DIR=C:\Ruta\A\Su\PPSA21567\PPSA21567-app"
  ```

### 2. Resolución y Escena de Referencia
- **Resolución**: 1280 × 720 (configurada por defecto en el script).
- **Nivel de Referencia**: **Aztec Sky**.
- **Posición del Personaje**: Tras cruzar el primer arco del nivel, en la plataforma amplia abierta. Dejar a Astro inmóvil en ese punto para todas las pruebas.

---

## D. Ejecución de Benchmarks (`stage8_camera_ab.cmd`)

El script interactivo `stage8_camera_ab.cmd` se encuentra en la raíz del repositorio y automatiza la captura de 30 segundos con el profiler:

```cmd
stage8_camera_ab.cmd
```

El script mostrará el menú principal con siete opciones:

```text
=====================================================================
   KytyPS5 - Etapa 8: DIAGNOSTICO TEMPORAL, HITCHES Y OPTIMIZACION
=====================================================================

  [1] Vista Amplia: Todas las optimizaciones de Etapa 8 activas
      (--pipeline-key-cache --draw-state-dirty-check)
      Salida: stage8_camera_wide.json / .csv

  [2] Vista Amplia: Control / Baseline sin optimizaciones de Etapa 8
      (--no-pipeline-key-cache --no-draw-state-dirty-check)
      Salida: stage8_camera_wide_baseline.json / .csv

  [3] Vista al Suelo: StreamBuffer normal de 64 MiB
      Salida: stage8_camera_ground_normal.json / .csv

  [4] Vista al Suelo: StreamBuffer extendido de 256 MiB
      (--extended-stream-buffer)
      Salida: stage8_camera_ground_extbuf.json / .csv

  [5] Vista Amplia: Captura detallada de etapas del pipeline (F9)
      Salida: stage8_camera_wide_stages.json / .csv

  [6] Vista al Suelo: Captura detallada de hitches y correlacion de bloqueos (F9)
      Salida: stage8_camera_ground_hitches.json / .csv

  [7] Salir
```

### Protocolo de Medición Obligatorio (Para cada prueba del 1 al 6):
1. **Seleccionar la opción** (1, 2, 3, 4, 5 o 6) en el menú.
2. El emulador se abrirá en modo de espera manual (`--audit-manual-trigger`).
3. Cargar la partida y entrar a **Aztec Sky**.
4. Orientar la cámara según la prueba:
   - **Vista Amplia** (Opciones 1, 2, 5): Mirando al frente hacia el horizonte, abarcando la mayor cantidad de geometría y escenario visible.
   - **Vista al Suelo** (Opciones 3, 4, 6): Inclinando el stick derecho totalmente hacia arriba para que la cámara enfoque directamente el suelo bajo los pies del personaje.
5. **Calentamiento (Warmup Obligatorio)**: Dejar al personaje quieto durante **20 a 30 segundos** sin tocar controles para permitir que los shaders se compilen y el streaming se estabilice.
6. **Presionar la tecla F9**: Se activará la captura de alta precisión durante exactamente 30 segundos.
7. **No mover el personaje ni la cámara durante los 30 segundos**.
8. Una vez transcurridos los 30 segundos, el emulador cerrará la captura y guardará automáticamente el archivo `.json` y `.csv`.
9. Cerrar el emulador y volver al menú para la siguiente prueba.

---

## E. Archivos que se Deben Entregar

Al finalizar la batería completa, recopilar los siguientes archivos generados en la raíz del emulador:

1. `stage8_camera_wide.json` y `stage8_camera_wide.csv`
2. `stage8_camera_wide_baseline.json` y `stage8_camera_wide_baseline.csv`
3. `stage8_camera_ground_normal.json` y `stage8_camera_ground_normal.csv`
4. `stage8_camera_ground_extbuf.json` y `stage8_camera_ground_extbuf.csv`
5. `stage8_camera_wide_stages.json` y `stage8_camera_wide_stages.csv`
6. `stage8_camera_ground_hitches.json` y `stage8_camera_ground_hitches.csv`

> [!TIP]
> **Verificación Rápida**: Abra los archivos `.json` con un editor de texto o visor JSON y compruebe que el campo `"total_frames"` sea superior a 500 y que no contengan valores vacíos.

---

## F. Problemas Frecuentes y Soluciones

1. **La ventana CMD se cierra inmediatamente**:
   - Asegúrese de que la ruta `GAME_DIR` dentro de `stage8_camera_ab.cmd` sea válida y no contenga comillas duplicadas.
   - Verifique que `_Build\windows\kyty_emulator.exe` exista. Si no compiló, siga la Sección B.
2. **Error de la aplicación: `0xc000007b`**:
   - Este error ocurre cuando Windows mezcla binarios de 32 y 64 bits o intenta ejecutar `kyty_emulator.exe` fuera de su carpeta sin sus DLLs dependientes (`SDL3.dll`, etc.).
   - Utilice siempre el script `stage8_camera_ab.cmd` o ejecute directamente desde `_Build\windows\`.
3. **Pulsar F9 no inicia la captura**:
   - Haga clic sobre la ventana del emulador para asegurarse de que tiene el foco de entrada del teclado.
   - En el overlay de rendimiento (F2) se mostrará el estado de la captura.
4. **Archivos JSON o CSV vacíos (0 bytes)**:
   - El emulador se cerró antes de completar los 30 segundos. Debe esperar a que el profiler finalice el tiempo estipulado.
5. **Crashes de Vulkan o Device Lost**:
   - Actualice los drivers de la tarjeta gráfica a la última versión disponible del fabricante (NVIDIA Game Ready / Studio 550+ o AMD Adrenalin).
