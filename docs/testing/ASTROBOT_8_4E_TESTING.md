# Guia de Pruebas de Rendimiento y Estabilidad: Astro Bot (PPSA21567) - Etapa 8.4E

Este documento proporciona las instrucciones completas para que desarrolladores y evaluadores externos compilen, ejecuten y comparen de manera cientifica y reproducible las optimizaciones graficas de la **Etapa 8.4E** de **KytyPS5** en el juego **Astro Bot** (*Title ID: PPSA21567*).

---

## 1. Resumen Arquitectonico de la Etapa 8.4E

La Etapa 8.4E introduce una arquitectura de renderizado paralela basada en tres componentes desacoplados:

```mermaid
flowchart LR
    PM4[Hilos de CPU Guest\nComandos PM4] -->|Lock-Free Ring| CPS[CpSequencer\nHilo PM4 Sequencer]
    CPS -->|Snapshot State| DP[DrawPrep\n4 Workers Paralelos]
    CPS -->|OpStream Secuencial| TG[Thread_Gpu\nRenderExecutor Vulkan]
    DP -->|PreparedDrawState\n(Hash, Layout, Descriptores)| TG
    TG -->|Vulkan Command Buffer| GPU[GPU Host\n(Vulkan 1.3 Driver)]
```

### Principios Fundamentales:
1. **Propiedad Exclusiva de Vulkan**: Unicamente `Thread_Gpu` manipula los `VkCommandBuffer` y la cola gráfica de Vulkan. Los workers de `DrawPrep` y el hilo `CpSequencer` operan puramente sobre decodificación de registros de hardware, cálculo de hashes y preparación de estructuras sin llamadas a Vulkan.
2. **0% Duplicación de Trabajo**: La decodificación de estados de draw (`VkPipelineLayout`, descriptores, dirty checks de render targets) preparada por los workers es consumida directamente por `RenderExecutor::ExecutePreparedDraw`, eliminando el 100% de recalculación redundante en el hilo de render (`draw_prep_duplication_pct == 0.0%`).
3. **Sincronización por Barreras y Drains**: Operaciones de computo (`DispatchDirect`), transferencias, flips y liberaciones de memoria (`ReleaseMem`) ejecutan un `Drain()` del pipeline de draws antes de emitirse a Vulkan, garantizando orden secuencial estricto y ausencia de condiciones de carrera.
4. **Fidelidad Grafica Preservada**: La optimización `--draw-state-dirty-check` (causante de regresiones visuales en Sky Garden en etapas previas) se mantiene **desactivada por defecto** tanto en el Baseline como en el Modo Experimental.

---

## 2. Requisitos del Sistema

### Hardware Recomendado:
- **CPU**: x86_64 con soporte AVX2 / FMA. Mínimo 8 núcleos / 16 hilos recomendado (AMD Ryzen 7 5700X/7700X/9700X o Intel Core i7-12700K/13700K/14700K).
- **GPU**: Tarjeta gráfica dedicada compatible con **Vulkan 1.3** y soporte nativo de timeline semaphores:
  - NVIDIA: GeForce RTX serie 20, 30, 40 o 50 (VRAM mínima: 8 GB; recomendado: 16 GB).
  - AMD: Radeon RX serie 6000 o 7000 (VRAM mínima: 8 GB; recomendado: 16 GB).
- **RAM**: 16 GB DDR4/DDR5 mínimo (32 GB recomendado).
- **Almacenamiento**: SSD NVMe para carga fluida de shaders y recursos.

### Software Requerido:
- **Sistema Operativo**: Windows 10 (22H2+) o Windows 11 x64.
- **Controladores Gráficos**: Controladores actualizados (NVIDIA 550+ o AMD Adrenalin 24+).
- **Juego**: Copia descifrada legal de *Astro Bot* (PPSA21567), versión 01.000 o parche 01.019.

---

## 3. Compilacion desde Codigo Fuente

Si compila localmente el emulador:

### Herramientas:
- [Visual Studio 2022](https://visualstudio.microsoft.com/) con el componente *Desarrollo para el escritorio con C++* y *Clang-cl*.
- [CMake](https://cmake.org/) (versión 3.28 o superior).
- [Ninja](https://ninja-build.org/) (recomendado para compilaciones de alta velocidad).
- Qt 6.x configurado en el `PATH` o `CMAKE_PREFIX_PATH`.

### Pasos de Compilacion (Release):
Abra una consola de comandos de Visual Studio (*x64 Native Tools Command Prompt*) o PowerShell:

```cmd
cmake -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _Build/windows kyty_emulator audit_profiler_tests cp_sequencer_tests draw_prep_tests
```

### Validacion de Tests Unitarios de la Arquitectura:
Antes de ejecutar el juego, verifique que los 4 módulos críticos de la etapa pasan exitosamente:

```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|stream_buffer_ring|cp_sequencer|draw_prep)"
```

Debe obtener `100% tests passed out of 4`.

---

## 4. Perfiles de Prueba A/B

Para aislar con exactitud la ganancia de la paralelización PM4 + DrawPrep, se definen dos perfiles de ejecución con opciones base idénticas:

| Opción de Motor | Perfil A (Baseline Control) | Perfil B (PM4 + DrawPrep Experimental) |
|---|:---:|:---:|
| `--gpu-submit-cadence` | **Activado** (1.0 ms) | **Activado** (1.0 ms) |
| `--redundant-bind-elision` | **Activado** | **Activado** |
| `--pipeline-cache-mru` | **Activado** | **Activado** |
| `--pipeline-key-cache` | **Activado** | **Activado** |
| `--ensure-pipeline-cache` | **Activado** | **Activado** |
| `--descriptor-cache` | **Activado** | **Activado** |
| `--fast-pending-ops` | **Activado** | **Activado** |
| `--priority-spin-wait` | **Activado** (100 µs) | **Activado** (100 µs) |
| `--no-draw-state-dirty-check` | **Desactivado** (seguro) | **Desactivado** (seguro) |
| `--cp-sequencer` | **Desactivado** (monohilo) | **Activado** (hilo PM4) |
| `--draw-prep` | **Desactivado** | **Activado** (paralelo) |
| `--draw-prep-workers` | *(N/A)* | **4 workers** |

### Ejecucion con Scripts Automatizados

El repositorio incluye scripts en `scripts/testing/` listos para usar:

#### Suite Interactiva:
```cmd
scripts\testing\run_astrobot_test_suite.bat "C:\Ruta\AstroBot\eboot.bin"
```

#### Ejecución Directa de Perfil A:
```cmd
scripts\testing\run_astrobot_profile_a_baseline.bat "C:\Ruta\AstroBot\eboot.bin"
```
*(O en PowerShell: `.\scripts\testing\run_astrobot_profile_a_baseline.ps1 -GamePath "C:\Ruta\AstroBot\eboot.bin"`)*

#### Ejecución Directa de Perfil B:
```cmd
scripts\testing\run_astrobot_profile_b_experimental.bat "C:\Ruta\AstroBot\eboot.bin"
```
*(O en PowerShell: `.\scripts\testing\run_astrobot_profile_b_experimental.ps1 -GamePath "C:\Ruta\AstroBot\eboot.bin"`)*

Ambos perfiles generan automáticamente un reporte JSON de telemetría de alto rendimiento en el directorio `_TestLogs/`.

---

## 5. Escenarios Clave de Evaluacion

Para evaluar integralmente estabilidad, rendimiento y gráficos, realice las pruebas en las siguientes 4 zonas del juego:

### Escenario 1: Inicio, Pantallas de Carga y Precarga de Shaders (Boot / Warmup)
- **Objetivo**: Evaluar la estabilidad inicial, ausencia de crashes o deadlocks durante la inicialización de recursos y calentamiento del pipeline cache.
- **Acción**: Iniciar el juego desde cero, pasar los logotipos de inicio y cargar la partida guardada.

### Escenario 2: Sky Garden — Vista Amplia de Camara (Overlook)
- **Objetivo**: Máxima presión de draw calls, geometría densa y shaders de follaje/terreno.
- **Ubicación**: Al inicio de Sky Garden, detener al personaje en la plataforma de aterrizaje y rotar la cámara 360° para abarcar todo el horizonte vegetal y el cielo.
- **Medición**: Tomar una muestra de 60 segundos de framerate y frametime estable.

### Escenario 3: Nivel de las Esponjas / Fluidos (Sponge Level)
- **Objetivo**: Probar la memoria compartida CPU/GPU, absorción de agua, deformación dinámica de mallas y saturación del stream buffer.
- **Ubicación**: Zona donde Astro absorbe agua con el traje de esponja y riega las plantas circundantes.
- **Medición**: Observar si se presentan tirones (*hitches* > 50 ms), stuttering o fugas de descriptores.

### Escenario 4: Zonas con Iluminacion y Sombras Exigentes
- **Objetivo**: Verificar fidelidad gráfica absoluta.
- **Verificación**: Comprobar que no existan parpadeos (*flickering*), sombras partidas, artefactos en el render target ni oclusión ambiental rota.

---

## 6. Que Observar y Metricas a Reportar

Al finalizar las sesiones de prueba con ambos perfiles, registre los resultados en la plantilla [`docs/testing/ASTROBOT_TEST_RESULTS_TEMPLATE.md`](file:///c:/Users/Recko/Documents/github/kyty/docs/testing/ASTROBOT_TEST_RESULTS_TEMPLATE.md):

1. **Rendimiento**:
   - FPS promedio (Average FPS).
   - 1% Low FPS y 0.1% Low FPS.
   - Frametime promedio en milisegundos (ms).
   - Cantidad de tirones (*hitches* mayores a 50 ms).
2. **Telemetria de Paralelizacion (Perfil B)**:
   - Inspeccionar el archivo generado en `_TestLogs/astrobot_profile_b_*.json` y verificar:
     - `draw_prep_published`: draws preparados en paralelo.
     - `draw_prep_consumed`: draws consumidos por `Thread_Gpu`.
     - `draw_prep_duplication_pct`: debe reportar **0.0%**.
3. **Fidelidad Visual y Errores**:
   - Confirmar si se presentó algún artefacto visual no existente en el Perfil A.
   - En caso de crash o error de Vulkan (`VK_ERROR_DEVICE_LOST` o fallo en `masterSemaphore.cpp`), registrar el log completo de la consola.

---

## 7. Reglas de Comparacion Cientifica

1. **PROHIBICIÓN ESTRICTA DE FALSEAR FPS**:
   - No active opciones de aceleración artificial de reloj (`--guest-tsc` fuera de valor nominal, modificaciones de frecuencia de vblank o eliminación de sincronización de audio) para simular mayor fluidez.
   - Mantenga idéntica resolución (`1280x720` nativo recomendado para benchmarks) y mismas condiciones de sombreado.
2. **CALENTAMIENTO DE SHADERS (SHADER WARMUP)**:
   - Realice siempre una pasada previa por la escena para calentar el pipeline cache de Vulkan (`_PipelineCache/`) antes de registrar las métricas definitivas, a fin de aislar la compilación inicial de shaders del throughput de renderizado continuo.
3. **SEPARACIÓN ENTRE MEDIDO Y ESTIMADO**:
   - Reporte únicamente valores efectivamente medidos mediante herramientas objetivas (CapFrameX, RTSS, AuditProfiler integrado en `_TestLogs/`).
