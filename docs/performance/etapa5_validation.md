# KytyPS5 — Etapa 5: Reporte de Validación, Implementación y Protocolo de Benchmark

**Fecha:** 21 de Noviembre de 2026  
**Rama:** `etapa5_optimization`  
**Juego de referencia:** Astro Bot (`PPSA21567`)  
**Hardware de referencia:** AMD Ryzen 7 9700X (8C/16T), NVIDIA GeForce RTX 5080 (16 GB), 32 GB DDR5-6000, Windows 11  

---

## 1. Resumen de la Auditoría Profunda (Fases 1 a 5)

La captura de referencia de 180 segundos (`stage4_3_cadence_partial.json`) arrojó las siguientes conclusiones empíricas fundamentales:

### 1.1 El Costo Real del Write Tracking
- **27.198.992 fallos de escritura** (8.073,3 por frame, 151.093/s).
- **16.919.283 reprotecciones de página** (5.022,0 por frame).
- **31,34 ms de tiempo acumulado de CPU por frame** distribuidos entre 10 hilos trabajadores de Astro Bot.
- **Amplificación de fallos por página única:** El manejador desprotegía únicamente 4 KB (`fault_size = 1`). Como consecuencia, cualquier copia en bloque (`memcpy` o generación de mallas) de 32 KB a 64 KB sufría entre 8 y 16 fallos de hardware consecutivos, 16 despachos SEH del kernel y 16 llamadas a `VirtualProtect(PAGE_READWRITE)`, provocando tormentas de TLB shootdowns y serialización en el `WorkingSetMutex` de Windows NT.

### 1.2 Puntos Calientes (Hot RIPs y Hot Pages)
- **Top RIP #1 (`0x0000000920003e55`):** 4.762.160 fallos (17,51% del total). Junto con sus RIPs adyacentes en `libc.prx` (`0x920003db6`, `0x920003fe2`, `0x920003ef0`), el clúster de copias rápidas vectorizadas acumula **6.950.333 fallos (25,55%)**.
- **Clúster del motor (`0x0000000907xxxxxx`):** Generación de matrices de transformación y buffers de vértices dinámicos, acumulando el **29,43%** adicional de los fallos.
- **Triple Buffering detectado:** Las páginas `0x50bd47000`, `0x50d547000` y `0x50ed47000` registran exactamente 1.123 reprotecciones cada una en 3.369 frames (exactamente 1 reprotección cada 3 frames, ratio Fallos/Reprot > 6,6).
- **Caso Multi-Pase (`0x000000057987f000`):** 13.472 reprotecciones y 9.094 fallos (exactamente 4 reprotecciones por frame y 2,7 fallos por frame en cada cuadro).

### 1.3 GPU Timeline Waits y Vulkan Submits
- **`BufferReadback`:** 28.406 detenciones síncronas de CPU (8,43 por frame) con una latencia promedio de 929,4 µs (7,84 ms de CPU bloqueada por cuadro).
- **`SubmitReason::Synchronization`:** 28.408 submits de Vulkan, en correspondencia 1:1 con los readbacks forzados.
- **`ExplicitFlush`:** 47.158 submits (14 por frame) generados al final de paquetes PM4 sin llamadas de dibujo reales.
- **`Cadence` (1.0 ms):** 119.621 submits (35,5 por frame).

---

## 2. Optimizaciones Implementadas

Siguiendo estrictamente la metodología de no comprometer la corrección y aplicar como máximo dos optimizaciones iniciales verificadas:

### 2.1 Optimización 1: Desprotección Coalescida en Write Faults (Coalesced Multi-Page Fault Handling)
- **Archivos modificados:**
  - [bufferCache.h](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/bufferCache.h) / [bufferCache.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/bufferCache.cpp):
    Implementación del método `BufferCache::CoalesceWriteFaultRange(uint64_t address, uint64_t max_bytes) const noexcept`.  
    Localiza el búfer que contiene la dirección de fallo mediante la tabla multinivel `m_page_table`, calcula el resto del buffer hasta un máximo prudente de 64 KB (16 páginas) sin cruzar la frontera de región de 4 MB (`TRACKER_REGION_SIZE`).
  - [renderContext.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderContext.cpp):
    En `RenderContext::HandleFault(PageFaultAccess::Write, fault_vaddr)`, se calcula `extent` vía `CoalesceWriteFaultRange` y se valida con `IsMapped(fault_vaddr, extent)`.
    La invalidación y desprotección se envía en un único bloque contiguo a `m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size)`, `m_texture_cache` y `m_image_heap`.
- **Efecto técnico:**
  - Cuando un bucle de `memcpy` o ensamblado de vértices escribe a lo largo de un búfer de 32 a 64 KB, el primer fallo desprotege las páginas contiguas del búfer en una única llamada a `VirtualProtect(PAGE_READWRITE)`.
  - Las siguientes 7 a 15 páginas ya tienen permisos `PAGE_READWRITE`, eliminando de raíz las excepciones `#PF` subsiguientes y las tormentas de TLB shootdowns.
  - Corrección garantizada: las páginas se marcan `CpuDirty`, y al preparar el draw en GPU, `BufferCache::SynchronizeBuffer` sube todos los datos válidos y restaura la protección contra escritura.

### 2.2 Optimización 2: Elisión de Submits Vacíos en `CommandScheduler` / `BufferFlush`
- **Archivos modificados:**
  - [commandScheduler.h](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.h) / [commandScheduler.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.cpp):
    Adición de la bandera `m_has_recorded_work`. Se activa con `NoteRecordedWork()` y se reinicia en `BeginCommand()`.
    En `CommandScheduler::Flush(SubmitInfo& submit, SubmitReason reason)`:
    ```cpp
    if (reason == Common::AuditProfiler::SubmitReason::ExplicitFlush && !m_has_recorded_work &&
        submit.num_wait_semaphores == 0 && submit.num_signal_semaphores == 0) {
        // Omite la llamada a vkQueueSubmit si no hay trabajo de GPU grabado ni semaforos pendientes
        return;
    }
    ```
  - [bufferCache.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/bufferCache.cpp), [streamBuffer.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/streamBuffer.cpp), [textureCache.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/textureCache.cpp):
    Llamadas a `m_scheduler.NoteRecordedWork()` en operaciones de copia y transferencia para garantizar que cualquier comando de GPU emitido marque el búfer como no vacío.
- **Efecto técnico:**
  - Elimina las llamadas espurias a `vkQueueSubmit` originadas por paquetes PM4 de solo configuración de registros de estado.
  - Reduce la sobrecarga del driver Vulkan de NVIDIA y alivia la contención sobre `queue_mutex`.

---

## 3. Verificación de Compilación y Suites de Pruebas

### 3.1 Compilación
- `kyty_emulator.exe` compilado y enlazado con éxito en arquitectura x64 mediante LLVM lld-link y Clang/MSVC 17.14.

### 3.2 Suites de Pruebas Ejecutadas
| Suite de Pruebas | Resultado | Estado |
|---|:---:|:---:|
| `page_manager_tests.exe` | 100% de casos aprobados | **PASSED** |
| `memory_tracker_tests.exe` | 100% de casos aprobados | **PASSED** |
| `audit_profiler_tests.exe` (17 pruebas completas) | 100% de casos aprobados | **PASSED** |
| `virtual_memory_allocation_tests.exe` (90+ pruebas) | 100% de casos aprobados | **PASSED** |
| `win_read_efault_tests.exe` | 100% de casos aprobados | **PASSED** |

---

## 4. Script y Protocolo de Benchmark A/B para el Usuario

Se ha generado el script automático [stage5_cadence_partial.cmd](file:///c:/Users/Recko/Documents/github/kyty/stage5_cadence_partial.cmd) para comparar directamente contra la línea base de la Etapa 4.3.

### Parámetros del Benchmark:
- **Ejecutable:** `_Build\windows\kyty_emulator.exe`
- **Comando:** `--game ... --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --audit-faults --audit-manual-trigger --audit-capture-duration 180 --audit-export stage5_cadence_partial.json`

### Instrucciones para la Ejecución:
1. Ejecutar `stage5_cadence_partial.cmd` en la raíz del repositorio.
2. El emulador iniciará en modo **WARMUP** (el profiler no graba al arranque).
3. Aguardar a que el juego cargue y situarse en la **misma escena interactiva** de Astro Bot utilizada en la prueba de la Etapa 4.3.
4. Mantener la cámara fija en la misma posición.
5. Esperar entre 20 y 30 segundos para estabilizar la carga de recursos y shaders en memoria.
6. Presionar **F9** para iniciar la captura de 180 segundos. El overlay indicará `[F9] Audit: RECORDING` en rojo.
7. Al cumplirse los 180 segundos, la exportación se completará automáticamente (`stage5_cadence_partial.json` y `.csv`), y el overlay indicará `COMPLETED` en verde.
8. Cerrar el emulador.
