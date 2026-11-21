# KytyPS5 — Etapa 5: Matriz de Optimización y Plan de Implementación

**Fecha:** 21 de Noviembre de 2026  
**Rama:** `etapa5_optimization`  
**Juego de referencia:** Astro Bot (`PPSA21567`)  
**Hardware:** AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5080 (16 GB), Windows 11  

---

## 1. Matriz de Evaluación de Cuellos de Botella y Opciones Técnicas

A partir de los datos empíricos de los 180 segundos de la prueba de referencia (18,71 FPS, 53,44 ms frametime, 27,2M write faults, 16,9M reprotecciones, 198,5K submits, 28,4K buffer readbacks), se evaluaron las posibles intervenciones técnicas:

| Opción de Optimización | Cuello de Botella que Ataca | Impacto Estimado | Riesgo de Corrección | Complejidad | Decisión |
|---|---|:---:|:---:|:---:|:---:|
| **A. Desprotección Coalescida en Write Faults (Coalesced Multi-Page Fault Handling)** | 27,2M fallos (31,34 ms CPU/frame), amplificación de fallos por página de 4 KB en `memcpy`/`memset` | **Muy Alto** (reducción potencial de 40% a 70% de fallos recurrentes) | **Muy Bajo** (las páginas del buffer iban a ser escritas de todas formas; se respeta la subida GPU) | Media | **SELECCIONADA (Opción 1)** |
| **B. Elisión de Submits Vacíos/Redundantes (Empty Submit Elision)** | 47,1K flushes explícitos (14/frame) y 1.103 submits/s en colas Vulkan sin trabajo gráfico | **Medio-Alto** (reducción de 20% a 30% de llamadas a `vkQueueSubmit`, menor overhead driver) | **Muy Bajo** (no omite submits con draws o syncs requeridos) | Baja | **SELECCIONADA (Opción 2)** |
| **C. Lecturas Asíncronas Especulativas en `BufferReadback`** | 28,4K paradas síncronas (7,84 ms/frame) por lecturas CPU de datos GPU | Alto | **Alto** (puede entregar datos obsoletos al juego si la lectura es síncrona estricta) | Alta | Descartada para esta etapa |
| **D. Eliminación Global de Write Tracking (Desactivar protección de memoria)** | 27,2M fallos y 16,9M reprotecciones | Extremo | **Inaceptable** (corrupción visual total, pérdida de corrección de buffers GPU) | Baja | **Rechazada categóricamente** |
| **E. Prolongar el intervalo de Cadence (ej. de 1.0 ms a 2.5 ms)** | 119K submits de cadencia | Medio | Medio (puede aumentar la latencia de inicio de la GPU y provocar stuttering) | Muy Baja | Conservar a 1.0 ms como línea base estable |

---

## 2. Detalle de las Dos Optimizaciones Seleccionadas

### 2.1 Optimización 1: Desprotección Coalescida en Write Faults (Coalesced Extent Unprotection)

#### Justificación Técnica
La auditoría de Hot RIPs reveló que el **25,55% de todos los fallos** provienen de instrucciones de copia en bloque vectorizadas (`memcpy`/`memset` en `0x0000000920003xxx`), y otro **29%** de funciones de actualización de mallas dinámicas y transformaciones en `eboot.bin` (`0x0000000907xxxxxx`).
En todos estos casos, el hilo huésped escribe datos contiguos a lo largo de un buffer que abarca varias páginas de 4 KB (por ejemplo, buffers de 32 KB a 64 KB).
Bajo la implementación actual:
- Cada página de 4 KB genera una interrupción de hardware `#PF` independiente.
- Cada página invoca por separado `HostException::ExceptionFilter` -> `RenderContext::HandleFault` con `fault_size = 1`.
- Cada página realiza una llamada a `VirtualProtect(..., PAGE_READWRITE)` que serializa el kernel de Windows NT y emite TLB shootdowns a todos los núcleos del Ryzen 7 9700X.

#### Diseño de la Solución
1. En [renderContext.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderContext.cpp#L69), `RenderContext::HandleFault` recibe la dirección de fallo `fault_vaddr`.
2. Al identificar que el fallo es de tipo `PageFaultAccess::Write`, en lugar de asumir ciegamente `fault_size = 1`, consulta a `BufferCache` o `MemoryTracker` si `fault_vaddr` reside dentro de un buffer conocido.
3. Si la dirección pertenece a un buffer registrado, calcula el rango contiguo protegido dentro del buffer (o un límite prudente de hasta, por ejemplo, 16 páginas contiguas = 64 KB, alineado a la frontera del buffer y de la región de 2 MB de `RegionManager`).
4. Se ejecuta la invalidación y desprotección para ese rango contiguo completo en **una sola llamada a `VirtualProtect`**:
   - `m_memory_tracker.InvalidateRegion(extent_vaddr, extent_size, ...)`
   - Se marca el rango contiguo como `CpuDirty`.
   - Se desprotege el rango continuo completo en una única syscall del sistema operativo.
5. **Garantía de corrección:**
   - La CPU del juego puede completar la totalidad de su bucle de copia sin volver a fallar en cada frontera de 4 KB.
   - Las páginas desprotegidas quedan marcadas como `CpuDirty`.
   - Cuando la GPU posteriormente vaya a consumir el buffer, `SynchronizeBuffer` iterará sobre todas las páginas modificadas (`ForEachUploadRange`), subirá los datos correctos a la GPU y restaurará la protección contra escritura de manera consistente.

---

### 2.2 Optimización 2: Elisión de Submits Vacíos en `CommandScheduler` / `BufferFlush` (Empty Submit Elision)

#### Justificación Técnica
La captura registró **47.158 submits de tipo `ExplicitFlush`** (14 submits por cuadro), además de los 35,5 submits por cuadro generados por `Cadence`.
En [graphicsRun.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/graphicsRun.cpp#L581):
```cpp
if (submission.execution.MadeProgress()) {
    cp.BufferFlush();
}
```
Cuando un paquete de comandos PM4 procesa únicamente registros o banderas de configuración sin grabar draws ni dispatches de cómputo en el `CommandBuffer` de Vulkan, `cp.BufferFlush()` invoca forzosamente `GetScheduler().Flush()`.
Esto obliga a cerrar el `CommandBuffer` actual y realizar una llamada completa a `vkQueueSubmit` con un búfer que no contiene trabajo de ejecución de GPU.

#### Diseño de la Solución
1. En `CommandBuffer` / `CommandScheduler`, rastrear si el `CommandBuffer` activo contiene comandos de ejecución grabados (`m_has_work` o `m_draw_count > 0` o transferencias de buffers/imágenes).
2. En `CommandScheduler::Flush(SubmitInfo& submit, SubmitReason reason)`:
   - Si `submit` no contiene semáforos obligatorios de sincronización externa, y el command buffer actual está vacío de comandos ejecutables (no se han grabado draws, dispatches ni copias de memoria desde el último submit), **eludir la llamada costosa a `vkQueueSubmit`**.
   - Mantener el `CommandBuffer` activo abierto para el siguiente lote de comandos, evitando la finalización y el reinicio innecesario del command buffer.
3. **Garantía de corrección:**
   - Si el submit incluye semáforos de señalización que otros hilos esperan explícitamente (`submit.num_signal_semaphores > 0`), el submit se ejecuta normalmente.
   - Los draws, dispatches y transferencias reales nunca se eliden.

---

## 3. Plan de Verificación y Criterios de Éxito

1. **Compilación limpia en Release y Debug:**
   - Ninja compila sin advertencias ni errores en `_Build\windows`.
2. **Ejecución y aprobación del 100% de la suite de pruebas:**
   - `page_manager_tests.exe`
   - `memory_tracker_tests.exe`
   - `resource_tracking_tests.exe`
   - `virtual_memory_allocation_tests.exe`
   - `audit_profiler_tests.exe`
   - `shader_recompiler_compute_tests.exe`
   - `win_read_efault_tests.exe`
   - `shader_cfg_tests.exe`
3. **Preservación total de la corrección funcional:**
   - Sin corrupción visual, sin cuelgues ni accesos inválidos.
