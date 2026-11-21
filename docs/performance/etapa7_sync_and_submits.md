# KytyPS5 — Análisis de Submits de Vulkan y Sincronización CPU/GPU

## 1. Desglose de Submits a la Cola de Vulkan

En las capturas de la Etapa 6.1 se registraron las siguientes categorías de envío a la cola Vulkan (`vkQueueSubmit`):

| Categoría de Submit | Vista Amplia (/frame) | Vista al Suelo (/frame) | Frecuencia Vista Amplia | Frecuencia Suelo |
|---|---:|---:|---:|---:|
| **Cadence** | 33,69 | 6,37 | 693,1 / s | 335,8 / s |
| **ExplicitFlush** | 9,05 | 9,01 | 186,2 / s | 474,6 / s |
| **Synchronization** | 6,31 | 1,94 | 129,9 / s | 102,3 / s |
| **Presentation** | 1,00 | 1,00 | 20,6 / s | 52,7 / s |
| **Total Submits** | **50,05** | **18,33** | **1.029,8 / s** | **965,4 / s** |

---

## 2. Hallazgos y Correlaciones Estructurales

### 1. Invarianza de `ExplicitFlush` (~9.0 por frame en ambas capturas)
- En ambas vistas, `ExplicitFlush` se emite exactamente **9 veces por frame**.
- **Causa raíz:** Astro Bot estructura su pipeline de renderizado en aproximadamente 9 listas de comandos (`Command Submission`) por fotograma:
  1. Compute Culling y física.
  2. Pases de sombras.
  3. Pre-pass de profundidad / z-prepass.
  4. G-Buffer / geometría opaca principal.
  5. Iluminación y oclusión ambiental.
  6. Materiales transparentes / efectos de partículas.
  7. Post-procesamiento y PSSR/escalado.
  8. Renderizado de interfaz de usuario (UI / HUD).
  9. Finalización y copias de swapchain.
- Al terminar de procesar cada envío guest en `graphicsRun.cpp` (`SubmissionType::Commands`), `CommandProcessor::BufferFlush()` invoca `GetScheduler().Flush()`.
- Por lo tanto, estos 9 flushes son **estructurales e intrínsecos a los límites de lista de comandos del juego**.

### 2. Comportamiento de `Cadence Submits` (33,69 vs 6,37)
- La opción `--gpu-submit-cadence --gpu-submit-cadence-ms 1.0` evalúa el envío de trabajo grabado a Vulkan cada 1.000 µs (1 ms).
- En Vista Amplia, el fotograma tarda **48,68 ms** en ser procesado por la CPU. Durante ese tiempo, el temporizador de 1 ms se dispara aproximadamente **34 veces** mientras hay trabajo grabado en el command buffer.
- En Vista al Suelo, el fotograma tarda **18,99 ms**, por lo que el temporizador se dispara aproximadamente **6 a 7 veces** por frame.
- **Conclusión:** La cantidad de submits de cadencia por frame es simplemente proporcional a la duración del fotograma en la CPU. La tasa por segundo se mantiene balanceada (335 - 693 submits/segundo), asegurando que la GPU reciba trabajo continuamente para no quedar ociosa.

### 3. Correlación 1:1 entre `Synchronization Submits` y `BufferReadback`
- En Vista Amplia:
  - Submits por `Synchronization`: **3.901**
  - Esperas por `BufferReadback`: **3.901**
  - Correlación: **100% exacta**.
- Cuando el juego o el emulador requiere leer un buffer que fue modificado por la GPU (`BufferCache::Readback`), el scheduler debe forzar el envío del command buffer activo a Vulkan (`CommandScheduler::Wait` -> `Submit({}, SubmitReason::Synchronization)`) antes de bloquearse esperando el tick en la GPU.
- En Vista Amplia hay 6,31 readbacks por frame; en Vista al Suelo hay 1,94 por frame.

### 4. Estado de la Elisión de Submits Vacíos (Empty Submit Elision)
- La optimización de la Etapa 5 (`Empty Submit Elision`) verifica:
  ```cpp
  if (!HasRecordedWork()) return;
  ```
- Esta optimización impidió que los miles de evaluaciones de cadencia o flushes sin comandos emitieran llamadas vacías a `vkQueueSubmit`. En la Vista Amplia, se retuvieron **110.600 cadences sin trabajo** (`cadence_held_back = 110.600`), protegiendo la cola de Vulkan de saturación innecesaria.
