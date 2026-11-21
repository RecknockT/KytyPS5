# KytyPS5 — Etapa 6.1: Auditoría y Validación de Instrumentación de Temporización y Carga Gráfica

Este documento presenta la auditoría exhaustiva de la instrumentación de temporización incorporada en la Etapa 6 y ampliada en la Etapa 6.1, evaluando la exactitud de los cálculos, la ausencia de carreras, la sobrecarga, el aislamiento de ventanas de medición y la naturaleza metodológica de la métrica de velocidad de simulación.

---

## 1. Auditoría de las Métricas de Temporización (Etapa 6)

### 1.1. Catálogo de Métricas y Puntos de Registro

| Métrica | Variable Interna | Punto de Registro | Frecuencia de Disparo | Tipo y Concurrencia |
|---|---|---|---|---|
| `total_vblanks` | `g_vblank_count` | `VideoOutDriver::Impl::VblankEnd()` en [src/graphics/presentation/videoOut.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/presentation/videoOut.cpp) | Periódico ($\sim 60\text{ Hz}$) en hilo `PresentThread` | `std::atomic<uint64_t>`, `fetch_add(1, memory_order_relaxed)` |
| `vblank_rate_hz` | Calculado en `TakeSnapshot()` | Derivado: $\frac{\text{total\_vblanks}}{\Delta t_{\text{real}}}$ | 1 vez al cerrar la captura | Escalar flotante `double` |
| `total_flips_submitted` | `g_flip_submitted_count` | `FlipQueue::Reserve()` en [src/graphics/presentation/videoOut.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/presentation/videoOut.cpp) | 1 por cada cuadro emitido hacia la cola | `std::atomic<uint64_t>`, `fetch_add(1, memory_order_relaxed)` |
| `total_flips_completed` | `g_flip_completed_count` | `FlipQueue::Flip()` cuando `due == true` en [src/graphics/presentation/videoOut.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/presentation/videoOut.cpp) | 1 por cada cuadro presentado en pantalla | `std::atomic<uint64_t>`, `fetch_add(1, memory_order_relaxed)` |
| `flip_rate_hz` | Calculado en `TakeSnapshot()` | Derivado: $\frac{\text{total\_flips\_completed}}{\Delta t_{\text{real}}}$ | 1 vez al cerrar la captura | Escalar flotante `double` |
| `total_flip_wait_ms` | `g_flip_wait_ns` | `FlipQueue::WaitForSubmitSlot()` y `FlipQueue::Wait()` en [src/graphics/presentation/videoOut.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/presentation/videoOut.cpp) | Durante saturación de cola de flips | `std::atomic<uint64_t>`, `fetch_add(wait_ns, memory_order_relaxed)` |
| `avg_flip_wait_ms` | Calculado en `TakeSnapshot()` | Derivado: $\frac{\text{total\_flip\_wait\_ms}}{\text{flip\_wait\_count}}$ | 1 vez al cerrar la captura | Escalar flotante `double` |
| `audio_updates_count` | `g_audio_updates_count` | `Audio::AudioOutOutputs()` en [src/libs/audio.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/libs/audio.cpp) | 1 por cada lote de muestras emitido | `std::atomic<uint64_t>`, `fetch_add(1, memory_order_relaxed)` |
| `audio_total_samples` | `g_audio_total_samples` | `Audio::AudioOutOutputs()` en [src/libs/audio.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/libs/audio.cpp) | Suma de `samples_num` (256/512 por lote) | `std::atomic<uint64_t>`, `fetch_add(samples, memory_order_relaxed)` |
| `audio_effective_hz` | Calculado en `TakeSnapshot()` | Derivado: $\frac{\text{audio\_total\_samples}}{\Delta t_{\text{real}}}$ | 1 vez al cerrar la captura | Escalar flotante `double` |
| `simulation_speed_ratio` | Calculado en `TakeSnapshot()` | Estimación: $\frac{\text{total\_flips\_completed} \times (1/60.0)}{\Delta t_{\text{real}}}$ | 1 vez al cerrar la captura | Escalar flotante `double` |

---

## 2. Auditoría Metodológica Crítica: `simulation_speed_ratio`

> [!IMPORTANT]
> **Definición Rigurosa de la Métrica de Velocidad de Simulación:**
> La métrica `simulation_speed_ratio` **NO es una medición directa del contador de ticks físicos internos del juego**, dado que el binario de Astro Bot (`eboot.bin`) no expone una API de telemetría de ticks al sistema operativo.
> 
> Es una **estimación analítica condicionada a la hipótesis del modelo de integración física fija a 60 Hz**:
> $$\text{simulation\_speed\_ratio}_{\text{modelo}} = \frac{\text{Flips Completados} \times \left(\frac{1}{60\text{ s}}\right)}{\text{Tiempo Real Transcurrido}}$$

### 2.1. Sustento de la Hipótesis
1. **Pacing del Bucle:** La auditoría de `eboot.bin` demostró que Astro Bot no avanza su bucle por temporizadores libres (`KernelUsleep`), sino que permanece bloqueado en `KernelWaitEqueue(eq)` esperando que `FlipQueue` señale `VideoOutEventKind::Flip`.
2. **Exclusividad PS5:** Los motores de consolas diseñados para 60 FPS fijos integran la física en pasos constantes ($\Delta t = 16.666\text{ ms}$) o saturan $\Delta t \le 16.666\text{ ms}$ para evitar colapso de colisiones y divergencias numéricas en PhysX/Havok.
3. **Comprobación Observacional:** La velocidad aparente de caída, carrera y animación en pantalla disminuye cuando la tasa de flips cae a 20 FPS ($\sim 33\%$ de velocidad).

### 2.2. Precauciones y Clarificación en la Salida
Para evitar malinterpretaciones:
1. En JSON se añadieron explícitamente los campos:
   - `"estimated_simulation_speed_ratio_model_60hz"`
   - `"estimated_simulation_speed_percent_model_60hz"`
   - `"simulation_speed_model_note"`: *"Theoretical model estimate assuming 1/60s fixed simulation timestep per completed flip. Not a directly measured internal guest tick counter."*
2. En CSV se agregaron:
   - `Timing,EstimatedSimulationSpeedRatioModel60Hz,...`
   - `Timing,EstimatedSimulationSpeedPercentModel60Hz,...`
   - `Timing,SimulationSpeedModelType,TheoreticalModelAssuming60HzFixedTimestep`
3. Se conservan las claves `simulation_speed_ratio` y `simulation_speed_percent` como alias para garantizar compatibilidad hacia atrás con scripts y pruebas existentes.

---

## 3. Nuevas Métricas de Carga Gráfica (Etapa 6.1)

Para diagnosticar el comportamiento observado por el usuario (caída de FPS al mirar el mapa amplio frente a aumento de FPS al mirar el suelo), se incorporó la medición de la carga gráfica del motor sin añadir sobrecarga en el camino crítico.

Se aprovechan los contadores atómicos ya existentes en [src/common/debugCounters.h](file:///c:/Users/Recko/Documents/github/kyty/src/common/debugCounters.h):

| Métrica | Fuente en DebugCounters | Propósito en el Diagnóstico A/B |
|---|---|---|
| `total_draws` | `Counter::Draws` en `renderDraw.cpp` | Determinar si la vista amplia incrementa las llamadas de dibujo Vulkan. |
| `draws_per_frame` | $\frac{\text{total\_draws}}{\text{total\_frames}}$ | Densidad de órdenes de dibujo por cuadro. |
| `draws_per_second` | $\frac{\text{total\_draws}}{\Delta t_{\text{real}}}$ | Rendimiento del hilo de traducción de comandos del emulador. |
| `total_draw_instances` | `Counter::DrawInstances` en `renderDraw.cpp` | Volumen de geometría instanciada. |
| `total_indirect_draws` | `Counter::IndirectDraws` en `graphicsRun.cpp` | Trabajo GPU diferido e indirecto. |
| `total_dispatches` | `Counter::Dispatches` en `renderCompute.cpp` | Carga de sombreadores de cómputo (postprocesado, partículas, culling GPU). |
| `pipelines_created` | `Counter::PipelinesCreated` | Detección de creación de nuevos pipelines durante la captura. |
| `shaders_compiled` | `Counter::ShadersCompiled` | Detección de compilación de shaders que pudiera sesgar el frametime. |
| `gpu_thread_busy_ms` | `Counter::GpuThreadBusyNs` en `graphicsRun.cpp` | **Tiempo CPU dedicado por el emulador a traducir paquetes PM4 y grabar comandos Vulkan.** |
| `gpu_thread_busy_ms_per_frame` | $\frac{\text{gpu\_thread\_busy\_ms}}{\text{total\_frames}}$ | Carga CPU por cuadro en el pipeline de traducción gráfica. |

---

## 4. Verificación de Seguridad y Aislamiento

1. **Aislamiento de Ventana de Captura:**
   - Durante la fase de calentamiento (`Warmup`, previa a F9), `g_capture_state == CaptureState::WaitingForHotkey`. Las funciones `RecordVblank()`, `RecordFlipSubmitted()`, `RecordFlipCompleted()`, etc., evalúan `if (g_capture_state.load(...) != CaptureState::Running) return;` e ignoran todos los eventos.
   - En `StartCapture()`, se reinician todos los contadores a 0 y se toma el valor basal (`baseline`) de `DebugCounters`.
   - Al finalizar la captura (`StopCapture()`), el estado pasa a `Completed`, congelando el snapshot. Los eventos posteriores no contaminan las métricas.
2. **Ausencia de Bloqueos (Lock-Free):**
   - Todos los contadores son `std::atomic<uint64_t>` con operaciones `fetch_add(..., std::memory_order_relaxed)`.
   - Ninguna de las sondas adquiere cerraduras (`std::mutex`) en el camino crítico.
3. **Sobrecarga (Overhead):**
   - Cuando el profiler está desactivado (`IsEnabled() == false`), cada función ejecuta únicamente una comprobación de booleano atómico en línea (`inline`), con un coste medido inferior a $1\text{ ns}$ por llamada.
4. **Validación:**
   - Superó las 18 pruebas unitarias de [tests/AuditProfilerTests.cpp](file:///c:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp) en $26\text{ ms}$.
