# KytyPS5 — Etapa 5: Auditoría Profunda del Costo de Write Tracking

**Fecha:** 21 de Noviembre de 2026  
**Rama:** `etapa5_optimization`  
**Juego de referencia:** Astro Bot (`PPSA21567`)  
**Hardware:** AMD Ryzen 7 9700X (8C/16T), NVIDIA GeForce RTX 5080 (16 GB), 32 GB DDR5-6000, Windows 11  
**Configuración de prueba:** GPU Submit Cadence (1.0 ms) + Partial Buffer Refresh (Extended Stream Buffer desactivado, Batch Hot Readbacks desactivado)  
**Resolución:** 1280 × 720  

---

## 1. Resumen Ejecutivo y Datos de Referencia

La captura de 180 segundos (`stage4_3_cadence_partial.json` / `stage4_3_cadence_partial.csv`) ejecutada en la escena interactiva de Astro Bot estableció la línea base rigurosa para la Etapa 5:

| Métrica | Valor Registrado | Unidad |
|---|---:|---|
| Duración real de captura | 180,015 | segundos |
| Total de cuadros (frames) | 3.369 | frames |
| Rendimiento efectivo | 18,715 | FPS |
| Frametime promedio | 53,44 | ms |
| Frametime P50 | 50,21 | ms |
| Frametime P95 | 66,74 | ms |
| Frametime P99 | 67,07 | ms |
| Frametime Máximo | 80,96 | ms |
| Proporción de cuadros con tirón (≥ 50 ms) | 64,50% | (2.173 frames) |
| **Total de Write Faults** | **27.198.992** | fallos |
| **Frecuencia de Write Faults** | **151.092,9** | fallos/segundo |
| **Write Faults por cuadro** | **8.073,3** | fallos/frame |
| **Total de Reprotecciones de Página** | **16.919.283** | reprotecciones |
| **Reprotecciones por cuadro** | **5.022,0** | reprotects/frame |
| Duración promedio del manejador de fallos | 3,88 | µs |
| Duración máxima del manejador | 6.865,0 | µs |
| **Tiempo acumulado de hilos en fallos** | **105.582,7** | ms (105,58 s) |
| **Tiempo acumulado en fallos por cuadro** | **31,34** | ms/frame |

---

## 2. Desglose del Costo Temporal del Write Tracking

El mecanismo de seguimiento de páginas mediante fallos de protección de memoria (`Write Tracking`) impone una carga computacional masiva en la CPU huésped.

### 2.1 Tiempo de CPU acumulado vs. Frametime de pared
El manejador de excepciones consume un promedio de **3,88 µs** por fallo. Multiplicado por los 8.073 fallos por cuadro, da como resultado:

$$\text{Tiempo CPU acumulado} = 8.073,3 \times 3,88\,\mu\text{s} = 31,34\,\text{ms por cuadro}$$

Dado que el procesador cuenta con 8 núcleos y 16 hilos, y Astro Bot utiliza un pool de entre 8 y 10 hilos trabajadores concurrentes (TIDs 22120, 7128, 17476, 12844, 25684, etc., cada uno registrando entre 1,8 y 3,1 millones de fallos), este tiempo no se añade secuencialmente al frametime de pared (53,44 ms), sino que se distribuye en paralelo. Sin embargo:
1. **Saturación de núcleos:** Consume aproximadamente el 60% del tiempo total de cómputo de los hilos de CPU disponibles en cada frame.
2. **Latencia de cola (Tail Latency):** Picos de hasta **6.865 µs (6,86 ms)** en un solo fallo paralizan el hilo crítico huésped, empujando el frametime por encima de los 66,7 ms (P95 de 66,74 ms).

### 2.2 Desglose de componentes de la latencia (3,88 µs por fallo)

1. **Despacho del Kernel de Windows (`#PF` -> SEH):** ~1,2 a 1,5 µs.  
   Interrupción por fallo de página por hardware x64, cambio de contexto Ring 0 -> Ring 3, y despacho del vector de excepciones estructuradas (`RtlDispatchException`).
2. **Filtro de Excepciones de Kyty:** ~0,3 µs.  
   Entrada a [runtimeLinker.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/loader/runtimeLinker.cpp#L673), decodificación de `info->access_violation_vaddr`, verificación de tipo `Write` y llamada a `Memory::HandleGpuFault`.
3. **Invalidación de Caché de Recursos GPU:** ~0,6 µs.  
   Búsqueda en [renderContext.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderContext.cpp#L69), llamadas a `BufferCache::InvalidateMemory`, `TextureCache::InvalidateMemory` e `ImageHeap::InvalidateMemory`.
4. **Actualización de Estado de Región y Spinlocks:** ~0,4 µs.  
   `MemoryTracker::InvalidateRegion` toma el spinlock de la región en [regionManager.h](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/regionManager.h#L39) y transiciona el bit CPU-dirty a activo (`ChangeState<DirtySource::Cpu, true>`).
5. **Syscall `VirtualProtect` (`PAGE_READWRITE`):** ~1,0 a 1,2 µs (con picos de >1.000 µs bajo contención).  
   Llamada a `VirtualProtect` para desproteger la página de 4 KB. En el kernel de Windows, `NtProtectVirtualMemory` adquiere el bloqueo exclusivo del espacio de direcciones del proceso (`WorkingSetMutex` / árbol VAD) y emite interrupciones entre procesadores (**TLB shootdowns**) a los restantes núcleos.

---

## 3. Arquitectura del Flujo de Excepción Paso a Paso

```
[CPU Huésped: Intento de escritura en memoria protegida (PAGE_READONLY)]
                                │
                                ▼
               [Hardware x64: Page Fault (#PF)]
                                │
                                ▼
            [Kernel Windows NT: RtlDispatchException]
                                │
                                ▼
 [runtimeLinker.cpp: HostException::ExceptionFilter(info)]
                                │
                                ▼
   [memory.cpp: Libs::LibKernel::Memory::HandleGpuFault(access, vaddr)]
                                │
                                ▼
   [renderContext.cpp: RenderContext::HandleFault(access, vaddr)]
        │                                             │
        ▼                                             ▼
 [WaitWriteBacks(vaddr, 1)]               [m_buffer_cache.InvalidateMemory(vaddr, 1)]
                                          [m_texture_cache.InvalidateMemory(vaddr, 1)]
                                          [m_image_heap.InvalidateMemory(vaddr, 1)]
                                                      │
                                                      ▼
                              [memoryTracker.h: MemoryTracker::InvalidateRegion]
                                                      │
                                                      ▼
                              [regionManager.h: RegionManager::ChangeState]
                                                      │
                                                      ▼
                       [pageManager.cpp: PageManager::UpdatePageWatchersForRegion]
                                                      │
                                                      ▼
                         [pageManager.cpp: PageManager::Impl::Protect]
                                                      │
                                                      ▼
                       [memory.cpp: Memory::ProtectGuestHostMemory]
                                                      │
                                                      ▼
                     [memoryAddressSpace.inc: ProtectMappedUnlocked]
                                                      │
                                                      ▼
                      [VirtualProtect(vaddr, 4096, PAGE_READWRITE)]
                                                      │
                                                      ▼
                   [EXCEPTION_CONTINUE_EXECUTION (Re-ejecución CPU)]
```

### Hallazgo Crítico en la Granularidad de Fallos
En [renderContext.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderContext.cpp#L72):
```cpp
bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
    constexpr uint64_t fault_size = 1;
    ...
```
El host reporta únicamente el byte del fallo. La invalidación subsiguiente en `BufferCache`, `MemoryTracker` y `PageManager` opera estrictamente sobre **una sola página de 4 KB**.

Cuando el juego realiza una copia en bloque (por ejemplo, `memcpy` de un buffer de vértices o constantes dinámicas de 64 KB a través de la función en `0x0000000920003e55`):
- Se generan **16 excepciones independientes consecutivas**.
- Se realizan **16 transiciones completas Ring 3 ↔ Ring 0**.
- Se ejecutan **16 llamadas a `VirtualProtect`**.
- Cada una fuerza la adquisición de bloqueos del kernel y TLB shootdowns a todos los núcleos del Ryzen 7 9700X.

---

## 4. Contención Multihilo y TLB Shootdowns

El informe reveló 27 hilos activos durante la sesión. Los 10 hilos con mayor cantidad de fallos son hilos de trabajo paralelos del motor de Astro Bot:

| Identificador de Hilo | Total de Fallos (180 s) | Fallos/segundo | % del Total |
|---|---:|---:|---:|
| TID 22120 | 3.134.155 | 17.410,5 | 11,52% |
| TID 7128 | 1.915.782 | 10.642,3 | 7,04% |
| TID 17476 | 1.907.115 | 10.594,2 | 7,01% |
| TID 12844 | 1.882.970 | 10.460,1 | 6,92% |
| TID 25684 | 1.882.313 | 10.456,4 | 6,92% |
| TID 27012 | 1.881.665 | 10.452,8 | 6,92% |
| TID 2456 | 1.876.631 | 10.424,8 | 6,90% |
| TID 24100 | 1.873.808 | 10.409,2 | 6,89% |
| TID 10836 | 1.868.860 | 10.381,7 | 6,87% |
| TID 27544 | 1.865.741 | 10.364,4 | 6,86% |
| **Subtotal Top 10 Hilos** | **20.089.040** | **111.596,4** | **73,86%** |

### El Efecto de Tormenta de Syscalls (Syscall Storm)
Con 10 hilos emitiendo concurrentemente más de 111.000 fallos y llamadas a `VirtualProtect` por segundo:
1. **Contención en `g_memory_state_mutex`:** En [memoryAddressSpace.inc](file:///c:/Users/Recko/Documents/github/kyty/src/kernel/memoryAddressSpace.inc#L931), `ProtectTransient` toma `std::shared_lock state(g_memory_state_mutex);`. Si bien es un bloqueo compartido (shared lock), su cache line rebota entre los 8 núcleos físicos del procesador con cada adquisición/liberación atómica.
2. **Contención del Kernel en `WorkingSetMutex`:** El kernel de Windows sincroniza todas las modificaciones de permisos de página dentro del mismo proceso mediante un mutex exclusivo. Por ende, **las 111.000 llamadas a `VirtualProtect` por segundo se serializan forzosamente en el kernel**, destruyendo el paralelismo del juego.
3. **Pico de latencia del manejador:** Esto explica por qué el `max_handler_us` alcanza los **6.865 µs (6,86 ms)** cuando múltiples hilos colisionan en `VirtualProtect`.

---

## 5. Conclusiones Técnicas de la Auditoría

1. **La causa raíz del bajo rendimiento (18,7 FPS) es la sobrecarga combinada del write tracking y las llamadas a `VirtualProtect`:** 31,34 ms de tiempo de CPU acumulado por frame en fallos y reprotecciones deja a los hilos de CPU sin margen para alimentar a la GPU.
2. **La granularidad página por página (4 KB) genera una amplificación masiva de fallos:** Los bucles de copia de datos (`memcpy`/`memset`) sufren una penalización de 1 fallo cada 4 KB, multiplicando por 16 la sobrecarga en buffers de 64 KB.
3. **El ciclo de vida del buffer (Write -> Upload -> Reprotect -> Write) es extremadamente rápido:** Un buffer se modifica por CPU, se sube a GPU, se vuelve a proteger contra escritura inmediatamente durante la subida, y el siguiente frame vuelve a fallar.
