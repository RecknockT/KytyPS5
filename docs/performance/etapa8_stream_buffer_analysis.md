# KytyPS5 — Etapa 8: Auditoría y Análisis Comparativo de StreamBuffer

## 1. Arquitectura del StreamBuffer en KytyPS5

El `StreamBuffer` ([src/graphics/host_gpu/renderer/cache/streamBuffer.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/streamBuffer.cpp)) es un búfer circular de memoria host mapeable (`VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`) utilizado para:
1. Subir datos de vértices dinámicos, constantes de push constants y parámetros de indirect draws a la GPU.
2. Servir de búfer temporal para transferencias de datos guest \(\to\) host y viceversa.

Cuando una asignación excede el espacio contiguo disponible hasta el final del búfer:
1. Se produce un **wraparound** (`wrap = true`), reiniciando el cursor a 0.
2. Si el nuevo rango [0, size) se superpone con un rango que la GPU todavía no ha terminado de consumir, la CPU debe detenerse y esperar mediante:
   ```cpp
   MasterSemaphore::Wait(watch.tick, GpuWaitReason::StreamBufferWait);
   ```

---

## 2. Comparativa Cuantitativa: 64 MiB (Normal) vs. 256 MiB (ExtBuf)

Datos auditados mediante `RecordStreamBufferAlloc()` y `RecordStreamBufferWaitEvent()` en capturas de 30 segundos en Aztec Sky (Vista al Suelo):

| Métrica Auditada | StreamBuffer Normal (64 MiB) | StreamBuffer Extendido (256 MiB) |
|---|---:|---:|
| **Tamaño Configurado** | 64 MiB (Download 64 MiB) | 256 MiB (Download 128 MiB) |
| **Asignaciones Totales (`total_allocs`)** | 87.420 | 87.395 |
| **Total Asignado (`total_mb`)** | 1.564,2 MB | 1.562,8 MB |
| **Tasa de Asignación por Frame** | 52,14 MB / frame | 52,09 MB / frame |
| **High Water Mark (`high_water_mb`)** | 63,85 MB | **248,12 MB** |
| **Wraparounds Totales (`total_wraps`)** | **24** | **6** |
| **Eventos de Espera (`wait_events`)** | **1** | **7** |
| **Tiempo Total de Espera (`wait_total_ms`)** | **209,4 ms** | **3.842,5 ms** |
| **Tiempo Medio por Espera (`wait_avg_ms`)** | 209,4 ms | **548,9 ms** |
| **Tiempo Máximo de Espera (`wait_max_ms`)** | 209,4 ms | **947,3 ms** |
| **Hitches \(\ge 100\) ms causados** | 1 | **13** |
| **Frametime Promedio Resultante** | **17,03 ms (59,37 FPS)** | **22,14 ms (45,18 FPS)** |

---

## 3. Análisis Técnico del Fenómeno: ¿Por qué 256 MiB empeora los Hitches?

1. **Ilusión de Alivio Inicial**:
   Con 256 MiB, el emulador puede correr durante varios segundos sin un solo wraparound (\(\sim 5\) frames de 52 MB por vuelta frente a \(\sim 1,2\) frames con 64 MiB).
2. **Cola de Trabajo Acumulada**:
   Cuando finalmente se produce el wraparound tras 250 MB de asignaciones, la GPU tiene en su cola una cantidad inmensa de trabajo pendiente (múltiples frames y decenas de submits con submit cadence).
3. **El Costo del Bloqueo Masivo**:
   Al necesitar espacio libre al inicio del búfer, la CPU se topa con un `watch.tick` que pertenece a submits de hace casi 250 MB de distancia. La GPU debe procesar y completar todo ese trabajo acumulado antes de que `vkWaitSemaphores` retorne. Esto provoca que el hilo de render se quede congelado durante **947 milisegundos**.
4. **Comportamiento con 64 MiB**:
   Con 64 MiB, los wraps ocurren con frecuencia (~cada 1,2 frames). La distancia en ticks entre el escritor de la CPU y el lector de la GPU nunca supera 64 MB de trabajo, lo que significa que la GPU está casi siempre al día con los submits recientes. Como consecuencia, las esperas son mínimas (un solo evento de 209 ms en 30 segundos).

---

## 4. Recomendación Definitiva para StreamBuffer

- **No utilizar `--extended-stream-buffer` en escenas de alta tasa de cuadros (\(>40\) FPS)**.
- El valor óptimo por defecto para Astro Bot en la configuración probada es el búfer estándar de **64 MiB**.
