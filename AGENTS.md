# KytyPS5 — Instrucciones Técnicas para Agentes y Desarrolladores

Este documento contiene las directrices de arquitectura, convenciones de desarrollo y reglas de rendimiento para cualquier agente de IA o desarrollador que trabaje en el repositorio **KytyPS5**.

---

## 1. Arquitectura General y Rutas Principales

KytyPS5 es un emulador de PlayStation 5 para Windows x64 basado en Vulkan y C++20.

### Directorios Clave del Motor Gráfico:
- `src/graphics/guest_gpu/`: Emulación de GPU de PS5 (AMD GCN/RDNA), procesador de comandos PM4 (`command_processor/`), paquetes PM4 (`pm4Handlers.cpp`) y dispatch loop (`graphicsRun.cpp`).
- `src/graphics/host_gpu/renderer/`: Renderizador Vulkan:
  - `renderDraw.cpp` / `renderDraw.h`: Ejecución de draws (`PrepareDrawRenderState`, `ExecutePreparedDraw`).
  - `commandScheduler.cpp`: Cola de comandos Vulkan, batches y sumisiones periódicas (`Submit`).
  - `masterSemaphore.cpp`: Sincronización CPU/GPU basada en timeline semaphores (`vkWaitSemaphores`).
  - `cache/streamBuffer.cpp`: Búfer circular mapeable para subidas de vértices y constantes.
  - `cache/textureCache.cpp`: Caché de texturas, write tracking y refrescos parciales.
  - `pipeline/pipelineCache.cpp`: Caché de pipelines gráficos, claves de hardware y hash map.
- `src/common/auditProfiler.cpp` / `auditProfiler.h`: Sistema de profiling de alto rendimiento para write tracking, tiempos de pipeline, trazas de hitches y exportación JSON/CSV.

---

## 2. Compilación y Validación de Tests

### Compilación (Release con Ninja):
```cmd
cmake -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _Build/windows
```

### Ejecución de Tests Unitarios:
```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|stream_buffer_ring|pipeline_cache|cadence)"
```

> [!IMPORTANT]
> **Ejecutables y DLLs**: El emulador debe ejecutarse siempre desde `_Build\windows\kyty_emulator.exe` o mediante scripts que preserven la búsqueda de DLLs (`SDL3.dll`, librerías Qt6). No copie únicamente el ejecutable a la raíz.

---

## 3. Estado de Optimizaciones y Flags (Etapas 5 a 8.1)

| Optimización | Flag de Activación | Flag de Desactivación | Estado por Defecto |
|---|---|---|:---:|
| **Submit Cadence** (Etapa 5) | `--gpu-submit-cadence` | (omitir) | Opcional (1.0 ms) |
| **Partial Buffer Refresh** (Etapa 5) | `--partial-buffer-refresh` | (omitir) | Opcional |
| **Redundant Bind Elision** (Etapa 7) | `--redundant-bind-elision` | `--no-redundant-bind-elision` | **Activado** |
| **Pipeline Cache MRU** (Etapa 7) | `--pipeline-cache-mru` | `--no-pipeline-cache-mru` | **Activado** |
| **Pipeline Key Cache** (Etapa 8) | `--pipeline-key-cache` | `--no-pipeline-key-cache` | **Activado** |
| **Draw State Dirty Check** (Etapa 8) | `--draw-state-dirty-check` | `--no-draw-state-dirty-check` | **Desactivado** (regresión Sky Garden) |
| **Ensure Pipeline Cache** (Etapa 8.1) | `--ensure-pipeline-cache` | `--no-ensure-pipeline-cache` | **Activado** |
| **Descriptor Cache** (Etapa 8.1) | `--descriptor-cache` | `--no-descriptor-cache` | **Activado** |
| **Fast Pending Operations** (Etapa 8.4) | `--fast-pending-ops` | `--no-fast-pending-ops` | **Activado** |
| **Priority Spin Wait** (Etapa 8.4) | `--priority-spin-wait` | `--no-priority-spin-wait` | **Activado** (100 µs) |
| **CP Sequencer** (Etapa 8.4B) | `--cp-sequencer` | `--no-cp-sequencer` | **Desactivado** (experimental) |
| **Extended Stream Buffer** (256 MiB) | `--extended-stream-buffer` | (omitir) | **Desactivado** (64 MiB estándar) |

Para documentación detallada de cada etapa, consulte la carpeta [`docs/performance/`](file:///C:/Users/Recko/Documents/github/kyty/docs/performance/).

---

## 4. Ejecución de Benchmarks en Astro Bot

Para pruebas reproducibles A/B en el juego de referencia (Astro Bot, Title ID `PPSA21567`), utilice los scripts interactivos en la raíz del repositorio:
- `stage8_3_astrobot_configuration_benchmark.cmd`: Benchmarks de presets, launcher, shader cache y calidad original vs parches (Etapa 8.3).
- `stage8_1_camera_and_landing.cmd`: Benchmarks de vista amplia y transición de aterrizaje continua con fases F10 (Etapa 8.1).
- `stage8_camera_ab.cmd`: Benchmarks de cámara de Etapa 8.

Consulte los detalles del protocolo en [`docs/performance/ETAPA8_3_GUIA_COLABORADOR.md`](file:///C:/Users/Recko/Documents/github/kyty/docs/performance/ETAPA8_3_GUIA_COLABORADOR.md) y [`docs/performance/ETAPA8_1_GUIA_COLABORADOR.md`](file:///C:/Users/Recko/Documents/github/kyty/docs/performance/ETAPA8_1_GUIA_COLABORADOR.md).

---

## 5. Reglas y Restricciones Estrictas de Desarrollo

1. **PROHIBICIÓN ESTRICTA DE FALSEAR FPS**:
   - **Bajo ninguna circunstancia** se deben alterar relojes guest (`guest_tsc`), frecuencias virtuales de VBlank, lógica de flip queues o temporización de audio para simular una mayor fluidez o aumentar el contador de FPS.
   - Cualquier mejora debe provenir de optimizaciones reales en el código de traducción, reducción de llamadas a la API gráfica, elisión de trabajo redundante o paralelización multihilo.
2. **DISTINCIÓN OBLIGATORIA ENTRE MEDIDO Y ESTIMADO**:
   - Nunca reporte proyecciones matemáticas o estimaciones teóricas como resultados obtenidos o confirmados. Toda afirmación de ganancia debe estar respaldada por telemetría JSON/CSV real.
3. **MANTENER FLAGS TOGGLEABLES**:
   - Cada nueva optimización introducida en el motor debe disponer de un flag CLI positivo y su correspondiente flag negativo (p. ej. `--mi-opt` / `--no-mi-opt`) para permitir pruebas de control A/B limpias.
4. **INTEGRIDAD DE MEMORIA Y SINCRONIZACIÓN**:
   - Preservar la coherencia de memoria entre CPU y GPU (write tracking y barreras de Vulkan). No omitir barreras ni protecciones de página sin comprobación previa de dirty state o hazards demostrados.
