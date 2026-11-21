# KytyPS5 — Etapa 5: Análisis de Vulkan Submits y GPU Submit Cadence

**Fecha:** 21 de Noviembre de 2026  
**Rama:** `etapa5_optimization`  
**Juego de referencia:** Astro Bot (`PPSA21567`)  
**Hardware:** AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5080 (16 GB), Windows 11  

---

## 1. Resumen de Vulkan Submits

Durante la captura de referencia de 180 segundos (`stage4_3_cadence_partial.json`), se registraron un total de **198.556 llamadas a `vkQueueSubmit`**, desglosadas por su motivo de origen:

| Motivo de Submit (`SubmitReason`) | Submits Totales | % del Total | Submits / Cuadro | Submits / Segundo |
|---|---:|---:|---:|---:|
| **`Cadence` (1.0 ms)** | **119.621** | **60,25%** | **35,51** | **664,5** |
| **`ExplicitFlush`** | **47.158** | **23,75%** | **14,00** | **262,0** |
| **`Synchronization`** | **28.408** | **14,31%** | **8,43** | **157,8** |
| **`Presentation`** | **3.369** | **1,70%** | **1,00** | **18,7** |
| `Drain` / `Other` | 0 | 0,00% | 0,00 | 0,0 |
| **Total General** | **198.556** | **100,00%** | **58,94** | **1.103,0** |

---

## 2. Diagnóstico del Ritmo de Envío (Cadence vs. Flushes)

### 2.1 La Frecuencia de 1.103 Submits por Segundo
Una tasa de **58,94 submits por cuadro** (más de 1.100 por segundo) representa una sobrecarga sustancial para el subsistema gráfico:
1. **Llamadas al Driver Vulkan (`nvldumdx.dll` / `nvoglv64.dll`):** Cada `vkQueueSubmit` requiere tomar el cerrojo del thread del scheduler (`queue_mutex` en [commandScheduler.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.cpp#L384)), construir estructuras internas del driver, validar semáforos y transicionar al kernel de Windows (`dxgkrnl.sys`).
2. **Latencia acumulada:** Si bien una GPU de gama ultra-alta como la RTX 5080 procesa lotes de comandos con enorme velocidad, cada submit individual tiene un costo base de CPU de entre 5 y 15 µs. Multiplicado por 59 submits por cuadro, esto representa entre **0,3 y 0,9 ms de tiempo de CPU por frame** dedicados exclusivamente al protocolo de submission.

### 2.2 Evaluación de `SubmitCadence` (119.621 submits)
El mecanismo de cadencia de envío está configurado con:
- `SubmitCadenceInterval = 1000 µs` (1,0 ms).
- `SubmitCadenceBoundaryStride = 32` paquetes PM4.
- `SubmitCadenceMaxGpuLag = 2` submits pendientes.

#### ¿Envía Cadence buffers vacíos?
Inspeccionando [submitCadence.h](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/submitCadence.h#L38):
```cpp
void NoteWork() {
    if (!m_enabled || m_due) return;
    const auto now = m_now();
    if (m_work == 0) m_start = now;
    m_work++;
    m_due = now - m_start >= m_interval;
}

bool DueAtBoundary() {
    if (m_work == 0) return false;
    ...
}
```
- **Conclusión positiva:** Cadence **no** emite submits vacíos de trabajo gráfico. Exige que al menos un draw o dispatch haya sido registrado (`m_work > 0`) antes de habilitar el submit.
- **Efecto de fragmentación:** Al forzar el corte del command buffer cada 1.0 ms, y dado que el frametime promedio es de 53,44 ms, Cadence fragmenta cada frame en un promedio de **35 pequeños command buffers**. Esto favorece que la GPU comience a trabajar tempranamente en lugar de esperar 50 ms a que la CPU termine de grabar, pero genera una gran cantidad de submits.

### 2.3 Evaluación de `ExplicitFlush` (47.158 submits, 14 por frame)
El 23,8% de los submits proviene de `ExplicitFlush`.
En [graphicsRun.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/graphicsRun.cpp#L580):
```cpp
case SubmissionType::Commands:
    complete = cp.Process(submission.execution, submission.commands) == Pm4ProcessResult::Complete;
    if (submission.execution.MadeProgress()) {
        cp.BufferFlush();
    }
    break;
```
Y en `BufferFlush()`:
```cpp
void CommandProcessor::BufferFlush() {
    m_renderer.GetBufferCache().PublishReadHot();
    GetScheduler().Flush();
    m_renderer.GetOcclusionQueries().Publish();
}
```
#### El Problema del Flush Redundante
Cuando un bloque de comandos PM4 procesa únicamente cambios de estado (como actualizaciones de constantes de sombreadores, viewports o banderas de rasterizado) sin emitir ningún comando de renderizado (`DrawIndex`, `DispatchDirect` o transferencias), `submission.execution.MadeProgress()` sigue siendo verdadero.
- Como consecuencia, se ejecuta `GetScheduler().Flush()`.
- Esto cierra el `CommandBuffer` actual y llama a `vkQueueSubmit` con un command buffer que no contiene llamadas de dibujo.
- Coexistiendo con Cadence, estos 14 flushes explícitos por frame a menudo interrumpen el buffer de comandos apenas unos microsegundos después de haberse abierto, agregando submits innecesarios.

### 2.4 Evaluación de `Synchronization` (28.408 submits, 8,43 por frame)
Como se demostró en el informe de sincronización GPU, estos 28.408 submits son provocados por:
```cpp
if (tick == CurrentTick()) {
    const auto submitted_tick =
        Submit({}, Common::AuditProfiler::SubmitReason::Synchronization);
    ...
}
```
Cada vez que el hilo CPU huésped requiere leer un buffer que fue modificado por GPU (`BufferReadback`), Kyty debe enviar el command buffer actual para que el timeline semaphore de Vulkan avance y señale el tick esperado.

---

## 3. Oportunidades de Optimización

1. **Evitar submits vacíos o triviales en `ExplicitFlush`:**
   Si el command buffer actual no tiene comandos de dibujo (`m_work == 0` o ningún draw/dispatch/transfer grabado desde el último submit), omitir el `vkQueueSubmit` y mantener el command buffer abierto. Esto podría eliminar una fracción considerable de los 47.158 flushes explícitos sin alterar la semántica de la emulación.
2. **Coalescencia de Cadence y ExplicitFlush:**
   Al unificar la comprobación de trabajo grabado, se reduce la contención sobre `queue_mutex` y se alivia la sobrecarga de submission en el driver Vulkan.
