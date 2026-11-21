# KytyPS5 — Etapa 5: Análisis de GPU Timeline Waits y Sincronización CPU/GPU

**Fecha:** 21 de Noviembre de 2026  
**Rama:** `etapa5_optimization`  
**Juego de referencia:** Astro Bot (`PPSA21567`)  
**Hardware:** AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5080, 32 GB DDR5-6000  

---

## 1. Resumen de Métricas de Sincronización GPU

Durante los 180 segundos de la prueba de referencia, se registraron las siguientes esperas de GPU mediante el `AuditProfiler`:

| Razón de Espera GPU | Eventos Totales | Eventos / Cuadro | Latencia Promedio | Latencia Máxima | Tiempo Acumulado | Tiempo / Cuadro | Impacto |
|---|---:|---:|---:|---:|---:|---:|---|
| **`PriorityOperation`** | **75.552** | **22,43** | **974,2 µs** | 5,26 ms | **73.600,9 ms** | **21,85 ms** | Paralelo (Hilo de prioridades) |
| **`BufferReadback`** | **28.406** | **8,43** | **929,4 µs** | 5,93 ms | **26.399,3 ms** | **7,84 ms** | **Bloqueante síncrono (CPU)** |
| `TextureWriteBack` | 0 | 0,00 | 0,0 µs | 0,00 ms | 0,0 ms | 0,00 ms | Inactivo |
| `BufferSuspendFlush` | 0 | 0,00 | 0,0 µs | 0,00 ms | 0,0 ms | 0,00 ms | Inactivo |
| `BufferGarbageCollection` | 0 | 0,00 | 0,0 µs | 0,00 ms | 0,0 ms | 0,00 ms | Inactivo |
| `CommandSchedulerDrain` | 0 | 0,00 | 0,0 µs | 0,00 ms | 0,0 ms | 0,00 ms | Inactivo |
| `Swapchain` / `Presentation` | 0 | 0,00 | 0,0 µs | 0,00 ms | 0,0 ms | 0,00 ms | Inactivo |
| **Total de Esperas GPU** | **103.958** | **30,86** | **961,9 µs** | **5,93 ms** | **100.000,2 ms** | **29,68 ms** | — |

---

## 2. Anatomía de las Esperas de GPU

### 2.1 `PriorityOperation` (75.552 eventos, 21,85 ms acumulados por cuadro)

#### Origen y Mecanismo
En [commandScheduler.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.cpp#L276), Kyty dispone de un hilo trabajador dedicado (`m_priority_thread`):
```cpp
void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
    while (!stop.stop_requested()) {
        PendingOperation operation;
        {
            std::unique_lock lock(m_operation_mutex);
            m_operation_available.wait(lock, [this, &stop] {
                return stop.stop_requested() || !m_priority_operations.empty();
            });
            ...
            operation = std::move(m_priority_operations.front());
            m_priority_operations.pop();
            m_priority_active_tick = operation.tick;
        }
        m_master.Wait(operation.tick, Common::AuditProfiler::GpuWaitReason::PriorityOperation);
        if (!stop.stop_requested()) {
            RunOperation(std::move(operation.callback));
        }
        ...
    }
}
```

#### Operaciones Encoladas
Las operaciones diferidas a esta cola corresponden a:
1. **Completado de Flip de Video:** En [graphicsRun.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/graphicsRun.cpp#L1153), la llamada a `CompleteFlip` se agenda como operación prioritaria una vez que el cuadro presentado ha sido procesado por la GPU.
2. **Interrupciones de `release_mem`:** En [graphicsRun.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/graphicsRun.cpp#L1050), cuando un comando PM4 solicita emitir una interrupción de finalización de evento.
3. **Descargas Asíncronas de Buffers:** En [bufferCache.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/bufferCache.cpp#L312), `PublishDownload` transfiere los datos volcados por GPU a la memoria física del juego.

#### Naturaleza del Costo
El hilo `m_priority_thread` corre de manera continua en paralelo con el resto de la emulación. Cuando espera en `m_master.Wait(operation.tick)`, permanece bloqueado en el timeline semaphore de Vulkan esperando que la GPU alcance el tick correspondiente (latencia promedio de 974,2 µs).
- **Impacto directo:** No detiene los hilos de CPU del juego a menos que se llame a `WaitPriorityOperations(tick)` (por ejemplo, ante una lectura síncrona).
- **Sobrecarga de concurrencia:** El hilo compite por el lock de operaciones (`m_operation_mutex`) y despierta frecuentemente para procesar callbacks.

---

### 2.2 `BufferReadback` (28.406 eventos, 7,84 ms por cuadro)

#### Origen y Mecanismo
A diferencia de `PriorityOperation`, las esperas por `BufferReadback` son **estrictamente bloqueantes y síncronas para el hilo que realiza el acceso**.

En [bufferCache.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/bufferCache.cpp#L597), cuando la CPU huésped accede a un buffer marcado como modificado por la GPU (`IsRegionGpuModified(vaddr, size)`):
```cpp
m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write] {
    ...
    if (IsRegionGpuModified(vaddr, size)) {
        ...
        const bool downloaded =
            m_batch_hot_readbacks
                ? DownloadWithHotBuffers(buffer_id, buffer, start, end - start)
                : DownloadBufferMemory<false>(buffer, start, end - start);
        ...
    }
});
```

Dentro de `DownloadBufferMemory<false>` y `DownloadWithHotBuffers`:
```cpp
const auto tick = m_scheduler.CurrentTick();
m_scheduler.Wait(tick, Common::AuditProfiler::GpuWaitReason::BufferReadback);
m_scheduler.WaitPriorityOperations(tick);
PublishDownload(pending);
```

#### La Conexión con los Vulkan Submits
Cuando `m_scheduler.Wait(tick)` se ejecuta con `tick == CurrentTick()` en [commandScheduler.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.cpp#L168):
```cpp
if (tick == CurrentTick()) {
    const auto submitted_tick =
        Submit({}, Common::AuditProfiler::SubmitReason::Synchronization);
    m_master.Wait(tick, reason);
    BeginNext();
}
```
**Efecto en cadena:**
1. El buffer actual de comandos Vulkan no se puede seguir acumulando; **se fuerza un submit inmediato a la GPU** (`SubmitReason::Synchronization`).
2. Esto explica la correlación exacta observada en los benchmarks:
   - **28.406 esperas de `BufferReadback`**
   - **28.408 submits de `SubmitReason::Synchronization`**
3. El hilo que requería leer los datos queda congelado durante un promedio de **929,4 µs** esperando a que la GPU ejecute los comandos y termine la copia.
4. Con 8,43 esperas por cuadro, esto impone **7,84 ms de detención directa de CPU por cuadro**.

---

## 3. Conclusiones y Oportunidades de Optimización

1. **Las lecturas síncronas de buffers (`BufferReadback`) son un cuello de botella de latencia crítico:** Cada frame sufre más de 8 paradas síncronas de casi 1 ms cada una, fragmentando la ejecución y generando submits forzados.
2. **La combinación de GPU Submit Cadence a 1.0 ms y BufferReadback mitiga parte de la latencia:** Al enviar comandos cada 1.0 ms, cuando ocurre un `BufferReadback`, la GPU ya ha avanzado en el trabajo previo, evitando esperas de 5 o 10 ms. Sin embargo, las 28.406 paradas siguen sumando casi 8 ms por cuadro.
3. **Optimización potencial:**
   - Evitar envíos síncronos forzados de `SubmitReason::Synchronization` cuando no hay comandos de copia pendientes o cuando el buffer de comandos no contiene trabajo de escritura relevante para el rango requerido.
   - En situaciones donde el búfer ya ha sido transferido o está en cola de descarga, evitar re-descargas redundantes.
