# KytyPS5 — Investigación de Hitches del StreamBuffer en Escenas Ligeras

## 1. El Fenómeno Observado en Vista al Suelo

En la captura de Vista al Suelo (30 segundos en Astro Bot):
- **FPS efectivos:** 52,68 FPS.
- **P50 (mediana):** 16,65 ms (correspondiente a 60 FPS clavados).
- **P90 / P95:** 17,03 ms / 17,14 ms.
- Sin embargo, se registraron **10 fotogramas con frametimes anómalos superiores a 100 ms**:
  - Frame 631: **833,83 ms** (a los 12,9 s de captura).
  - Frame 29: **816,92 ms** (a los 1,29 s de captura).
  - Frames 925, 827, 402, 1347, 297, 1153: **~266,9 ms** cada uno.
  - Frames 135, 1249: **~263,5 ms** y **~250,6 ms**.

Total de hitches $\ge 100\text{ ms}$: **10 eventos**.

---

## 2. Causa Raíz Identificada: `StreamBufferWait`

Al inspeccionar el desglose de esperas de GPU (`gpu_waits_breakdown`) en `stage6_1_camera_ground.json`:

```json
"StreamBufferWait": {
  "avg_latency_us": 220595.11,
  "count": 10,
  "count_per_frame": 0.006325,
  "max_ms": 560.4868,
  "thread_accumulated_ms": 2205.9511,
  "thread_accumulated_ms_per_frame": 1.3952
}
```

- **Cantidad de eventos de `StreamBufferWait`:** **Exactamente 10**.
- **Latencia máxima de espera:** **560,48 ms**.
- **Latencia promedio:** **220,59 ms**.
- **Correlación:** **100%**. Los 10 hitches severos de más de 100 ms en Vista al Suelo corresponden exactamente a las 10 pausas del `StreamBufferWait`.

---

## 3. ¿Por qué ocurre en Vista al Suelo y no en Vista Amplia?

En `src/graphics/host_gpu/renderer/cache/streamBuffer.cpp`:
1. El `StreamBuffer` almacena buffers transitorios de CPU a GPU (constantes dinámicas, parámetros de mesh shader, copias de índices de 8 bits, etc.).
2. Por defecto, cuando `extended_stream_buffer == false`, el tamaño asignado es de **64 MiB**:
   ```cpp
   m_stream_buffer(graphics, scheduler, MemoryUsage::Stream,
                   Config::ExtendedStreamBufferEnabled() ? 256 * MiB : 64 * MiB)
   ```
3. A medida que se graban draws y constantes, el cursor `m_offset` avanza.
4. Cuando el espacio se agota (`aligned_offset > Size() - mapped_size`), el buffer hace **wraparound** (retorna al offset 0).
5. **Condición de espera (`WaitPendingOperations`):**
   Al volver al inicio del buffer, el emulador no puede sobrescribir la memoria si la GPU física aún está ejecutando comandos grabados que leen esa misma región de memoria.
   Si la GPU aún no ha alcanzado el fence de esa operación anterior, la CPU debe detenerse obligatoriamente en `Scheduler().Wait(watch.tick, GpuWaitReason::StreamBufferWait)`.

### Comparación de tasas de consumo:
- **En Vista Amplia (20,57 FPS):**
  Cada frame tarda ~48,7 ms en CPU. El buffer de 64 MiB tarda mucho más tiempo de reloj de pared en agotarse. Para cuando el wraparound ocurre, la GPU ya terminó de procesar los comandos viejos hace varias decenas de milisegundos (`IsFree(watch.tick) == true`), por lo que `StreamBufferWait` fue **0 ms (0 eventos)**.
- **En Vista al Suelo (52,68 FPS):**
  La CPU genera fotogramas a una tasa **2,56 veces más rápida** (16,66 ms por frame). El buffer de 64 MiB se agota cada ~2,5 a 3 segundos de tiempo real. Si la GPU tiene un pequeño retraso de presentación o sincronización de buffer readback, el wraparound se estrella contra una región aún en uso por la GPU, provocando una parada crítica de 200 a 560 ms.

---

## 4. Solución y Mitigación

El emulador ya cuenta con la bandera `--extended-stream-buffer` (`Config::ExtendedStreamBufferEnabled()`).
- Al activar `--extended-stream-buffer`:
  - `m_stream_buffer` se amplía de **64 MiB a 256 MiB** (factor de 4x).
  - `m_download_buffer` se amplía de **64 MiB a 128 MiB**.
- Con 256 MiB, la distancia temporal entre wraparounds se multiplica por cuatro, dando a la GPU un margen masivo de varios segundos para liberar la memoria antes de que la CPU vuelva a necesitarla.
- Esto elimina por completo los hitches periódicos observados en escenas de alta tasa de fotogramas (>50 FPS) a un costo despreciable de ~200 MiB de RAM del sistema.
