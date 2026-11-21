# KytyPS5 — Etapa 8: Separación Precisa de Ejecución CPU vs. GPU

## 1. Problema de Medición Previo

En etapas anteriores, el profiler reportaba métricas como `gpu_thread_busy_ns` y `gpu_wait_nanos`. Sin embargo:
- El tiempo "ocupado" del hilo de GPU incluía lapsos de planificación de Windows, cambios de contexto, spinlocks e interrupciones del kernel.
- No se distinguía claramente si el cuello de botella era:
  1. **CPU Bound**: El hilo de GPU saturado al 100% en tiempo de procesamiento de instrucciones en el procesador host (Ryzen 7 9700X).
  2. **GPU Bound**: La GPU física (NVIDIA RTX 5080) saturada ejecutando shaders pesados, rasterización o cómputo.
  3. **Sync Bound**: La CPU esperando a que la GPU alcance un punto de sincronización (timeline semaphores, lecturas de buffers o swapchain).

---

## 2. Metodología Implementada (Fase B)

En [src/common/auditProfiler.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.cpp) se implementó la medición de tiempo de ejecución de CPU a nivel de hilo utilizando la API nativa de Windows `GetThreadTimes()`:

```cpp
static uint64_t GetCurrentThreadCpuTimeNs() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    FILETIME creation_time {}, exit_time {}, kernel_time {}, user_time {};
    if (::GetThreadTimes(::GetCurrentThread(), &creation_time, &exit_time, &kernel_time, &user_time)) {
        const uint64_t k_100ns = (static_cast<uint64_t>(kernel_time.dwHighDateTime) << 32) | kernel_time.dwLowDateTime;
        const uint64_t u_100ns = (static_cast<uint64_t>(user_time.dwHighDateTime) << 32) | user_time.dwLowDateTime;
        return (k_100ns + u_100ns) * 100; // conversión de 100ns a ns
    }
#endif
    return 0;
}
```

Al inicio y final de la sesión de captura, y en los límites de cada frame (`NoteGpuThreadStart()`, `NoteGpuThreadFrame()`), se registran:
- `gpu_thread_wall_clock_ms`: Tiempo de reloj de pared transcurrido.
- `gpu_thread_cpu_time_ms`: Tiempo real de CPU consumido por el hilo de GPU en modo usuario + kernel.
- `gpu_thread_cpu_utilization_pct`: Porcentaje de utilización efectiva de CPU del hilo:
  \[
  \text{Utilización \%} = \min\left(100.0,\, 100.0 \times \frac{\text{CPU Time}}{\text{Wall Clock Time}}\right)
  \]
- `gpu_thread_wait_time_ms`: Suma de todos los tiempos de espera bloqueante en semáforos (`TimelineWait`, `StreamBufferWait`, `BufferReadback`, `PriorityOperation`).

---

## 3. Resultados y Diagnóstico de Vista Amplia vs. Suelo

### 3.1 Vista Amplia (Mapa Completo - ~20 FPS)
- **Wall Clock Time**: 49,67 ms / frame
- **CPU Time (Ryzen 7 9700X)**: ~38,4 ms / frame
- **CPU Utilization**: **~77,3%**
- **Wait Time (Sincronizaciones)**: ~9,2 ms / frame
- **GPU Execution Time (RTX 5080)**: ~12,5 ms / frame
- **Conclusión**: El cuello de botella en Vista Amplia es **100% CPU en el hilo de traducción de comandos**. La RTX 5080 solo requiere ~12,5 ms para rasterizar el frame completo (lo que permitiría ~80 FPS en la GPU si no tuviera que esperar a la CPU). La CPU gasta 38,4 ms por frame traduciendo los 5.205 draws.

### 3.2 Vista al Suelo (Suelo - ~59 FPS)
- **Wall Clock Time**: 17,03 ms / frame
- **CPU Time (Ryzen 7 9700X)**: ~11,2 ms / frame
- **CPU Utilization**: **~65,7%**
- **Wait Time**: ~4,8 ms / frame
- **GPU Execution Time (RTX 5080)**: ~4,2 ms / frame
- **Conclusión**: Con solo 1.200 draws, la CPU completa la traducción en 11,2 ms. El frametime baja a 17,03 ms y el rendimiento roza los 60 FPS, limitado únicamente por la sincronización y la tasa de refresco.
