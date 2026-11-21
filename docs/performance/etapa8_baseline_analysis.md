# KytyPS5 — Etapa 8: Análisis Baseline y Caracterización de Carga

## 1. Contexto y Objetivos

En la Etapa 7 se introdujeron optimizaciones al renderizador de KytyPS5:
- **Redundant Bind Elision**: Omisión de `vkCmdBindPipeline`, `vkCmdBindVertexBuffers` y `vkCmdBindIndexBuffer` idénticos.
- **PipelineCache Fast-Path MRU**: Acceso en \(O(1)\) al último pipeline utilizado antes del hash XXH3 y búsqueda en tabla.
- **StreamBuffer Extended** (opcional, 256 MiB vs 64 MiB).

A pesar de estas mejoras, persistieron dos fenómenos críticos que motivan la **Etapa 8**:
1. **Cuello de botella en Vista Amplia**: El frametime ronda los ~49,67 ms (~20,14 FPS) en Aztec Sky con más de 5.200 draw calls por frame.
2. **Hitches severos en escenas ligeras (Vista al Suelo)**: Mientras que la tasa media sube a ~50-60 FPS, se observan congelamientos de hasta 1,48 segundos (1483 ms) asociados al mecanismo de StreamBuffer y sincronización con GPU.

---

## 2. Comparativa Cuantitativa de Datos Experimentales (Etapa 7)

A partir de los archivos de telemetría capturados en Aztec Sky (`stage7_camera_wide.json`, `stage7_camera_ground.json` y `stage7_camera_ground_extbuf.json`):

| Métrica | Vista Amplia (64 MiB) | Vista Suelo Normal (64 MiB) | Vista Suelo ExtBuf (256 MiB) |
|---|---:|---:|---:|
| **FPS Efectivos** | **20,14** | **59,37** | **45,18** |
| **Frametime Promedio** | 49,67 ms | 17,03 ms | 22,14 ms |
| **P50 / P95 Frametime** | 49,60 / 51,40 ms | 15,90 / 17,30 ms | 16,10 / 19,80 ms |
| **P99 / Max Frametime** | 56,10 / 121,40 ms | 24,10 / 232,50 ms | 46,20 / 1.483,20 ms |
| **Draw Calls / frame** | **5.205,72** | 1.200,57 | 1.198,40 |
| **Instancias / frame** | 20.771,62 | 9.078,77 | 9.062,10 |
| **Dispatches / frame** | 234,62 | 110,01 | 110,05 |
| **Vulkan Submits / frame** | 50,05 | 18,33 | 18,29 |
| **Write Faults / frame** | 1.273 | 593 | 591 |
| **Hitches \(\ge 100\) ms** | 1 | 2 | **13** |
| **Hitches \(\ge 500\) ms** | 0 | 0 | **4** |
| **Peor Hitch** | 121,4 ms | 232,5 ms | **1.483,2 ms** |
| **Eventos StreamBufferWait** | 0 | 1 (209 ms) | **7 (máx 947 ms)** |
| **MRU Hit Rate (Pipelines)** | 77,76% | 76,40% | 76,38% |
| **CPU Translation Time / draw** | ~8,2 µs | ~7,9 µs | ~8,0 µs |

---

## 3. Diagnóstico de los Dos Cuellos de Botella

### 3.1 Cuello de Botella de Vista Amplia (~20 FPS)
- **Densidad de Draws**: Con 5.205 draws por frame a 8,2 µs por draw, el hilo de render gasta:
  \[
  5.205 \times 8,2\,\mu\text{s} \approx 42,68\,\text{ms}
  \]
  únicamente en el overhead de CPU de traducción guest \(\to\) host (preparación de estados dinámicos, cálculo de firmas de render targets, hashing y construcción de pipeline keys, commit de descriptors y grabación de comandos Vulkan).
- El tiempo total disponible en un frame de 60 FPS es de **16,66 ms**. Gastar 42,68 ms en CPU en la traducción de draws hace matemáticamente imposible alcanzar 60 FPS sin optimizaciones adicionales que elidan trabajo redundante.

### 3.2 Cuello de Botella de Hitches en Vista al Suelo (Congelamientos de hasta 1,5 s)
- En Vista al Suelo, el hilo CPU corre a ~60 FPS, llenando el buffer de streaming a una velocidad mucho mayor (\(\sim 52\) MB/frame en menos de 17 ms \(\to > 3\) GB/s).
- Al expandir el buffer a 256 MiB (`--extended-stream-buffer`), se posterga el wraparound, acumulando hasta 250 MB de comandos y escrituras en vuelo sin sincronizar.
- Cuando finalmente el buffer se llena y necesita reusar espacio que aún está siendo consumido por la GPU, `StreamBuffer::WaitPendingOperations` emite un bloqueo en el `MasterSemaphore` que dura hasta **947 ms** esperando a que la GPU drene una cola gigantesca.
- Con el buffer estándar de 64 MiB, los wraps ocurren con mayor frecuencia pero con cantidades de trabajo pendientes mucho menores, resultando en un único hitch de 209 ms frente a 13 hitches (acumulando 3,8 segundos perdidos) con el buffer de 256 MiB.

---

## 4. Archivos Modificados e Instrumentación

- Instrumentación de etapas y buffers en [src/common/auditProfiler.h](file:///C:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.h) y [src/common/auditProfiler.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.cpp).
- Script de reproducción y menú de pruebas A/B en [stage8_camera_ab.cmd](file:///C:/Users/Recko/Documents/github/kyty/stage8_camera_ab.cmd).
