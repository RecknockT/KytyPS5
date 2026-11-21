# KytyPS5 — Guía Práctica para Colaboradores (Etapa 8.1)

¡Bienvenido! Este documento te guiará paso a paso para compilar KytyPS5, ejecutar las pruebas de rendimiento en **Astro Bot** (`PPSA21567`) y recopilar los archivos de telemetría de la **Etapa 8.1**.

---

## 1. Requisitos Previos

- **Sistema Operativo**: Windows 11 x64.
- **Hardware Recomendado**: CPU multinúcleo moderna (e.g. AMD Ryzen 7 9700X o similar), GPU compatible con Vulkan 1.3 (e.g. NVIDIA RTX 4000/5000 series o AMD Radeon RX 7000 series), mínimo 16–32 GB de RAM.
- **Herramientas de Compilación**:
  - Visual Studio 2022 (MSVC / Clang-cl).
  - CMake 3.25 o superior.
  - Ninja build tool.
- **Juego**: Astro Bot instalado o desencriptado en `C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app` (o edite la variable `GAME_DIR` en el script).

---

## 2. Compilación del Proyecto

Abra una consola de comandos (**x64 Native Tools Command Prompt for VS 2022**) en la raíz del repositorio y ejecute:

```cmd
git checkout etapa8_1_landing_optimization
ninja -C _Build/windows
```

Verifique que el ejecutable se haya generado correctamente:
```cmd
_Build\windows\kyty_emulator.exe --help
```
Debe mostrar en la salida de versión:
```text
git = KytyPS5-...-gdb2aba1e8-dirty
```
junto con las nuevas opciones `--ensure-pipeline-cache` y `--descriptor-cache`.

---

## 3. Uso del Script Interactivo (`stage8_1_camera_and_landing.cmd`)

En la raíz del repositorio, ejecute:
```cmd
stage8_1_camera_and_landing.cmd
```

Aparecerá el menú principal de la Etapa 8.1:
```text
=====================================================================
   KytyPS5 - Etapa 8.1: TRANSICION DE ATERRIZAJE Y OPTIMIZACIONES
=====================================================================

Controles en Gameplay:
  F9  : Iniciar / Detener captura
  F10 : Avanzar fase de aterrizaje (WorldEntryPrep -> DescentStart ->
        ActiveDescent -> GroundApproach -> Touchdown -> EarlyGameplay)

Seleccione la prueba a ejecutar:
  [1] Vista Amplia: Todas las optimizaciones activas (30 seg con F9)
  [2] Vista Amplia: Control / Baseline sin Etapa 8 / 8.1 (30 seg con F9)
  [3] Aterrizaje: Modo Continuo con optimizaciones activas (F9 inicio/fin, F10 fases)
  [4] Aterrizaje: Modo Continuo Baseline (F9 inicio/fin, F10 fases)
  [5] Aterrizaje: Analisis detallado de shaders y pipelines (F9 inicio/fin, F10)
  [6] Aterrizaje: Analisis de memoria y sincronizacion (ExtBuf 256 MiB, F9/F10)
  [7] Salir
```

---

## 4. Protocolo Paso a Paso de las Pruebas

### Bloque A: Pruebas de Vista Amplia (Opciones 1 y 2)
1. Seleccione la opción deseada (`1` o `2`).
2. Presione cualquier tecla para iniciar el emulador.
3. Cargue la partida y diríjase al nivel **Aztec Sky**.
4. Deje al personaje quieto mirando hacia el horizonte abierto del mapa.
5. Espere unos 20 segundos para estabilizar la caché de shaders.
6. Presione **F9**. El emulador capturará automáticamente durante **30 segundos**.
7. Al finalizar, cierre el emulador para confirmar que se generaron los archivos JSON y CSV.

### Bloque B: Pruebas de Transición de Aterrizaje (Opciones 3, 4, 5 y 6)
Estas pruebas utilizan el **Modo Continuo**:
1. Seleccione la opción (`3` para optimizado, `4` para baseline).
2. Inicie el emulador y diríjase a la selección de mundo.
3. **Inicio de Captura**: Justo antes de que la nave inicie el descenso hacia el planeta, presione **F9**. En la consola se mostrará:
   ```text
   [AuditProfiler] Capture STARTED ... (phase: Gameplay)
   ```
4. **Avance de Fases con F10**:
   A medida que transcurre la animación, presione **F10** en los siguientes hitos:
   - **Pulsación 1 de F10**: Al comenzar la preparación de entrada al mundo (`WorldEntryPrep`).
   - **Pulsación 2 de F10**: Al entrar a la atmósfera superior (`DescentStart`).
   - **Pulsación 3 de F10**: Durante el descenso rápido a través de las nubes (`ActiveDescent`).
   - **Pulsación 4 de F10**: Cuando la nave se acerca a los árboles y terreno (`GroundApproach`).
   - **Pulsación 5 de F10**: En el instante del aterrizaje/impacto (`Touchdown`).
   - **Pulsación 6 de F10**: Cuando se abre la nave y Astro sale (`EarlyGameplay`).
5. **Fin de Captura**: Cuando tome control del personaje y comience a caminar, presione **F9** para detener la captura. En la consola se confirmará la exportación:
   ```text
   [AuditProfiler] Capture COMPLETED ...
   [AuditProfiler] Exported X per-frame time series records to ..._frames.csv
   ```
6. Cierre el emulador.

---

## 5. Archivos a Enviar para Análisis

Tras completar las pruebas, en la raíz del repositorio se habrán generado conjuntos de tres archivos por cada prueba ejecutada:

1. **Para Vista Amplia**:
   - `stage8_1_wide_optimized.json`, `stage8_1_wide_optimized.csv` y `stage8_1_wide_optimized_frames.csv`
   - `stage8_1_wide_baseline.json`, `stage8_1_wide_baseline.csv` y `stage8_1_wide_baseline_frames.csv`
2. **Para Aterrizaje**:
   - `stage8_1_landing_optimized.json`, `stage8_1_landing_optimized.csv` y `stage8_1_landing_optimized_frames.csv`
   - `stage8_1_landing_baseline.json`, `stage8_1_landing_baseline.csv` y `stage8_1_landing_baseline_frames.csv`
   - `stage8_1_landing_shaders.*` / `stage8_1_landing_memory.*` (si ejecutó las opciones 5 y 6)

Comprima estos archivos en un `.zip` y compártalos para procesar los gráficos comparativos de frametimes y desgloses de pipeline. ¡Muchas gracias por tu colaboración!
