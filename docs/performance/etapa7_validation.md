# KytyPS5 — Validación Técnica y Protocolo de Benchmark (Etapa 7)

## 1. Resumen de Implementación

En la Etapa 7 se implementaron las siguientes optimizaciones de CPU y renderizado host:

1. **Elisión de Bindings Redundantes (`CommandBuffer`):**
   - Rastrear el pipeline gráfico activo (`m_bound_graphics_pipeline`).
   - Rastrear el buffer de índices activo (`m_bound_index_buffer`).
   - Rastrear los buffers de vértices activos (`m_bound_vertex_buffers`).
   - Eludir llamadas repetidas a `vkCmdBindPipeline`, `vkCmdBindIndexBuffer` y `vkCmdBindVertexBuffers2` cuando los parámetros son idénticos dentro del mismo command buffer.
   - Contadores de diagnóstico: `RedundantPipelineBinds`, `RedundantVertexBinds`, `RedundantIndexBinds`.
2. **Fast-Path MRU en `PipelineCache`:**
   - Rastrear `m_last_graphics_key` y `m_last_graphics_pipeline`.
   - Si la clave construida coincide byte a byte con la anterior (`operator==`), retornar el puntero cacheado directamente sin invocar `XXH3_64bits` ni consultar `std::unordered_map`.
   - Contadores de diagnóstico: `PipelineCacheLookups`, `PipelineCacheHits`, `PipelineCacheHitPct`.
3. **Soporte de Toggles en CLI y Configuración:**
   - `--redundant-bind-elision` / `--no-redundant-bind-elision`
   - `--pipeline-cache-mru` / `--no-pipeline-cache-mru`
   - Ambos activos por defecto (`true`).

---

## 2. Validación de Pruebas Automatizadas

Se compilaron y ejecutaron las 8 suites de pruebas unitarias y de integración del emulador, con **100% de éxito**:

| Suite de Pruebas | Resultado | Detalles |
|---|---|---|
| **`audit_profiler_tests.exe`** | **19/19 PASSED** | Valida métricas de carga gráfica, detección de binds redundantes, hits de pipeline cache MRU, exportación JSON/CSV y modelo de temporización. |
| **`audio_out_timing_tests.exe`** | **PASSED** | Pacing constante de audio a 48 kHz. |
| **`event_queue_lifetime_tests.exe`** | **PASSED** | Desbloqueo y ciclo de vida de colas de eventos guest. |
| **`memory_tracker_tests.exe`** | **PASSED** | Integridad de write tracking y coalescencia de páginas. |
| **`page_manager_tests.exe`** | **PASSED** | Reprotección virtual y gestión de memoria guest. |
| **`resource_tracking_tests.exe`** | **PASSED** | Tracking de buffers y texturas Vulkan. |
| **`virtual_memory_allocation_tests.exe`** | **PASSED** | Asignación y mapeo virtual. |
| **`sync_on_address_tests.exe`** | **PASSED** | Primitivas de sincronización rápida. |

---

## 3. Protocolo Experimental para el Usuario

Para evaluar el impacto de las optimizaciones en Astro Bot bajo condiciones controladas:

1. Ejecutar el script:
   ```cmd
   stage7_camera_ab.cmd
   ```
2. **Opción [1] — Captura A (Etapa 7, Vista Amplia):**
   - Inicia con las optimizaciones activas (`stage7_camera_wide.json` / `.csv`).
   - En *Aztec Sky*, dejar a Astro Bot quieto mirando hacia el horizonte abierto.
   - Esperar 20-30 segundos para estabilizar shaders.
   - Presionar **F9**. Se capturarán 30 segundos de gameplay estable y se exportará el informe.
3. **Opción [2] — Captura B (Etapa 7, Vista al Suelo):**
   - En la misma posición exacta, inclinar la cámara verticalmente hacia abajo.
   - Presionar **F9** para capturar 30 segundos (`stage7_camera_ground.json` / `.csv`).
4. **Opción [3] — Control (Vista Amplia SIN Etapa 7):**
   - Corre con `--no-redundant-bind-elision --no-pipeline-cache-mru` (`stage7_camera_wide_baseline.json` / `.csv`).
   - Permite verificar la ganancia neta en frametime y llamadas al driver bajo el mismo punto de vista.
5. **Opción [4] — Validación de Hitches (Vista al Suelo con Extended Stream Buffer):**
   - Corre con `--extended-stream-buffer` (`stage7_camera_ground_extbuf.json` / `.csv`).
   - Permite verificar la desaparición de los hitches de 560 ms observados en la Etapa 6.1.
