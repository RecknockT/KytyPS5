# KytyPS5 — Etapa 6: Implementación de Instrumentación de Temporización y Simulación

Este documento detalla los cambios de arquitectura y código fuente implementados en KytyPS5 para medir con precisión científica los parámetros de temporización, frecuencia de VBlanks, flips de presentación, latencias de espera y velocidad efectiva de simulación.

---

## 1. Archivos Modificados

| Componente | Archivo | Modificación |
|---|---|---|
| **AuditProfiler Header** | [src/common/auditProfiler.h](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.h) | Nuevos campos en `ProfileSnapshot` y funciones de registro libres de bloqueos (`noexcept`). |
| **AuditProfiler Implementation** | [src/common/auditProfiler.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.cpp) | Contadores atómicos con semántica relajada, cálculo de tasas en `TakeSnapshot()`, serialización en JSON y CSV. |
| **VideoOut Subsystem** | [src/graphics/presentation/videoOut.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/presentation/videoOut.cpp) | Inserción de sondas en `VblankEnd()`, `FlipQueue::Reserve()`, `FlipQueue::Flip()`, y esperas de ranuras. |
| **Audio Subsystem** | [src/libs/audio.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/libs/audio.cpp) | Inserción de sonda en `Audio::AudioOutOutputs()` para contabilizar bloques y muestras de audio. |
| **Test Suite** | [tests/AuditProfilerTests.cpp](file:///c:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp) | Nuevo test unitario exhaustivo validando exactitud, aislamiento de fases y exportación. |

---

## 2. Métricas Implementadas en `ProfileSnapshot`

En [src/common/auditProfiler.h](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.h), se incorporaron los siguientes campos a la estructura de captura:

```cpp
// Timing and simulation metrics (over the captured interval)
uint64_t total_vblanks          = 0;
double   vblank_rate_hz         = 0.0;
uint64_t total_flips_submitted  = 0;
uint64_t total_flips_completed  = 0;
double   flip_rate_hz           = 0.0;
uint64_t total_flip_wait_ns     = 0;
double   total_flip_wait_ms     = 0.0;
double   avg_flip_wait_ms       = 0.0;
uint64_t audio_updates_count    = 0;
uint64_t audio_total_samples    = 0;
double   audio_effective_hz     = 0.0;
double   simulation_speed_ratio = 0.0; // (flips_completed * (1/60s)) / real_duration_s
```

### Funciones de Registro (Lock-Free y Zero-Allocation):
```cpp
void RecordVblank() noexcept;
void RecordFlipSubmitted() noexcept;
void RecordFlipCompleted() noexcept;
void RecordFlipWait(uint64_t wait_ns) noexcept;
void RecordAudioUpdate(uint32_t samples, uint32_t freq) noexcept;
```

---

## 3. Puntos de Inserción en el Emulador

### 3.1. VBlanks de Presentación
En `VideoOutDriver::Impl::VblankEnd()`:
```cpp
void VideoOutDriver::Impl::VblankEnd() {
    Common::AuditProfiler::RecordVblank();
    Common::LockGuard lock(m_mutex);
    ...
```
Registra cada pulso del generador de VBlank en el hilo de pantalla.

### 3.2. Sometimiento y Completitud de Flips
En `FlipQueue::Reserve()`:
```cpp
bool FlipQueue::Reserve(VideoOutConfig& cfg, int index, int64_t flip_arg,
                        FlipRequestSource source, uint64_t& request_id) {
    Common::AuditProfiler::RecordFlipSubmitted();
    Common::LockGuard lock(m_mutex);
    ...
```

En `FlipQueue::Flip()`:
```cpp
    if (due) {
        Common::AuditProfiler::RecordFlipCompleted();
        Graphics::RenderDocOnGuestFlip(m_presenter.Renderer());
        ...
```
Registra la finalización exitosa del cuadro y el momento en que se activa el `FlipEvent` hacia el juego.

### 3.3. Esperas en la Cola de Flips
En `FlipQueue::WaitForSubmitSlot()` y `FlipQueue::Wait()`:
```cpp
const auto wait_begin = std::chrono::steady_clock::now();
m_done_cond_var.Wait(&m_mutex);
const auto wait_ns = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wait_begin)
        .count());
Common::AuditProfiler::RecordFlipWait(wait_ns);
```
Mide con precisión de nanosegundos el tiempo que los hilos del emulador o del juego esperan cuando la cola de presentación se satura.

### 3.4. Emisión de Muestras de Audio
En `Audio::AudioOutOutputs()`:
```cpp
    Common::AuditProfiler::RecordAudioUpdate(first_port.samples_num, first_port.freq);
    return first_port.samples_num;
```
Permite computar la tasa de entrega efectiva de audio en hertzios reales.

---

## 4. Fórmulas de Cálculo en `TakeSnapshot()`

En [src/common/auditProfiler.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.cpp):

1. **Frecuencia de VBlank:**
   $$\text{vblank\_rate\_hz} = \frac{\text{total\_vblanks}}{\Delta t_{\text{real}}}$$

2. **Frecuencia de Flips Efectiva:**
   $$\text{flip\_rate\_hz} = \frac{\text{total\_flips\_completed}}{\Delta t_{\text{real}}}$$

3. **Latencia Promedio de Espera en Flip:**
   $$\text{avg\_flip\_wait\_ms} = \frac{\text{total\_flip\_wait\_ms}}{\text{flip\_wait\_count}}$$

4. **Frecuencia de Muestreo Efectiva de Audio:**
   $$\text{audio\_effective\_hz} = \frac{\text{audio\_total\_samples}}{\Delta t_{\text{real}}}$$

5. **Ratio de Velocidad de Simulación:**
   $$\text{simulation\_speed\_ratio} = \frac{\text{total\_flips\_completed} \times \left(\frac{1}{60.0}\right)}{\Delta t_{\text{real}}}$$
   - Si el juego corre a 60 FPS en tiempo real: $\text{ratio} = 1.0000$ ($100\%$).
   - Si el juego corre a 20 FPS en tiempo real: $\text{ratio} = 0.3333$ ($33.3\%$).

---

## 5. Salidas en JSON y CSV

### Estructura en JSON (`timing_and_simulation`):
```json
{
  "timing_and_simulation": {
    "total_vblanks": 10800,
    "vblank_rate_hz": 60.0,
    "total_flips_submitted": 3634,
    "total_flips_completed": 3634,
    "flip_rate_hz": 20.18,
    "total_flip_wait_ms": 1250.45,
    "avg_flip_wait_ms": 0.34,
    "audio_updates_count": 16875,
    "audio_total_samples": 8640000,
    "audio_effective_hz": 48000.0,
    "simulation_speed_ratio": 0.3364,
    "simulation_speed_percent": 33.64
  }
}
```

### Filas en CSV (`Timing,...`):
```csv
Timing,TotalVblanks,10800
Timing,VblankRateHz,60.00
Timing,TotalFlipsSubmitted,3634
Timing,TotalFlipsCompleted,3634
Timing,FlipRateHz,20.18
Timing,TotalFlipWaitMs,1250.45
Timing,AvgFlipWaitMs,0.34
Timing,AudioUpdatesCount,16875
Timing,AudioTotalSamples,8640000
Timing,AudioEffectiveHz,48000.00
Timing,SimulationSpeedRatio,0.3364
Timing,SimulationSpeedPercent,33.64%
```
