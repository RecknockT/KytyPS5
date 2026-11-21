# KytyPS5 — Etapa 8.1: Corrección Integral de Anomalías del Profiler

## 1. Resumen de Anomalías Identificadas en Etapa 8

Durante la auditoría de las capturas de telemetría de la Etapa 8 en Astro Bot (`stage8_camera_wide.json`, `stage8_camera_ground_normal.json`, etc.), se detectaron **6 inconsistencias críticas en el sistema de profiling**:

| # | Anomalía Detectada en Etapa 8 | Causa Raíz Identificada | Corrección Implementada en Etapa 8.1 |
|---|---|---|---|
| **1** | `gpu_thread_wall_clock_ms` reportaba valores desproporcionados (~504.447.360 ms = uptime del sistema). | Se utilizaba la marca de tiempo absoluta en nanosegundos del sistema sin restar el instante inicial de captura. | Medición precisa del intervalo relativo de captura `[wall_end - wall_start]` en milisegundos con fallback a `capture_real_duration_ms` y medición de alta resolución. |
| **2** | `gpu_thread_cpu_utilization_pct` reportaba prácticamente 0% (0.000%). | Uso de handle no duplicado de hilo en Windows y división por el wall clock inflado de 500 millones de ms. | Duplicación segura del handle mediante `DuplicateHandle` en `Thread_Gpu` y cálculo exacto: `100.0 * (cpu_ms / wall_ms)` clamp a 100%. |
| **3** | `gpu_queries_recorded = 0` / tiempo de ejecución de GPU física en 0. | El emulador no tiene implementada una piscina de consultas de timestamp Vulkan (`vk::QueryPool`) en `context.cpp` (`gpu_timestamps_supported = false`). | Documentación explícita del motivo; adición de campos de soporte en `ProfileSnapshot` y prevención de falsos bloqueos. |
| **4** | Correlación de hitches asignaba bloqueadores minúsculos (~4,25 ms) a frames de >1,3 s y repetía el mismo evento en todos los hitches. | Búsqueda global en el búfer circular sin ventana temporal; tomaba el evento de mayor duración absoluta de toda la sesión. | Filtrado temporal estricto en la ventana `[sf_start - 15 ms, sf_end + 15 ms]`. Si no hay bloqueador coincidente, clasifica como `ComputeOrRasterBound` (confianza Low). |
| **5** | Doble contabilización de etapas de pipeline: `Pm4Dispatch` incluía todas las etapas de draw internas. | `Pm4Dispatch` mide el tiempo global de procesamiento del paquete, dentro del cual se ejecutan `RenderStatePrep`, `DescriptorPrep`, etc. | Desglose dual: `pm4_dispatch_inclusive_ms` y `pm4_dispatch_exclusive_ms` (restando la suma de las 5 etapas internas de draw). |
| **6** | Metadatos de build desactualizados (`1408bfef4-dirty` en lugar del commit base publicado `db2aba1e8`). | Falta de regeneración automática de variables de versión Git en el profiler. | Integración dinámica con `git_revision`, `git_hash` (`db2aba1e8`), `build_timestamp` y metadatos de compilador. |

---

## 2. Detalle Técnico de las Correcciones

### 2.1. Corrección del Reloj de Pared (`gpu_thread_wall_clock_ms`) y Utilización de CPU
En [`src/common/auditProfiler.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.cpp):
```cpp
// Captura precisa de tiempos de hilo en Windows mediante GetThreadTimes
static uint64_t GetThreadCpuTimeNsFromHandle(HANDLE handle) noexcept {
#if defined(_WIN32)
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        FILETIME creation {}, exit_time {}, kernel_time {}, user_time {};
        if (::GetThreadTimes(handle, &creation, &exit_time, &kernel_time, &user_time)) {
            ULARGE_INTEGER kt, ut;
            kt.LowPart = kernel_time.dwLowDateTime; kt.HighPart = kernel_time.dwHighDateTime;
            ut.LowPart = user_time.dwLowDateTime; ut.HighPart = user_time.dwHighDateTime;
            return (kt.QuadPart + ut.QuadPart) * 100ULL; // 100ns units to ns
        }
    }
#endif
    return 0;
}
```
En `TakeSnapshot()`:
```cpp
if (wall_end > wall_start) {
    snapshot.gpu_thread_wall_clock_ms = static_cast<double>(wall_end - wall_start) / 1e6;
} else if (snapshot.capture_real_duration_ms > 0) {
    snapshot.gpu_thread_wall_clock_ms = static_cast<double>(snapshot.capture_real_duration_ms);
} else {
    // Modo continuo o sub-milisegundo
    const auto cap_now = (state == CaptureState::Completed || state == CaptureState::Interrupted)
                             ? g_capture_end_time : Clock::now();
    if (g_capture_start_time.time_since_epoch().count() > 0 && cap_now >= g_capture_start_time) {
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               cap_now - g_capture_start_time).count();
        snapshot.gpu_thread_wall_clock_ms = std::max(0.001, static_cast<double>(nanos) / 1e6);
    }
}
```
La utilización de CPU resultante representa fielmente el porcentaje real de saturación del núcleo (por ejemplo ~96% en vista amplia).

### 2.2. Explicación de Consultas de GPU Física (`gpu_queries_recorded = 0`)
En el renderizador Vulkan actual de KytyPS5, las sumisiones de buffers de comandos utilizan timeline semaphores (`vkWaitSemaphores`), pero no intercalan `vkCmdWriteTimestamp` con un `vk::QueryPool` para cronometrar la ejecución en silicio de la GPU.
- Por tanto, `gpu_timestamps_supported` es `false` y `gpu_queries_recorded` es 0 por diseño.
- El tiempo reportado en `gpu_thread_busy_ms` corresponde a la CPU registrando y traduciendo trabajo para la GPU.

### 2.3. Correlación Temporal de Hitches y Detección de `ComputeOrRasterBound`
En [`auditProfiler.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.cpp):
```cpp
// Ventana temporal de tolerancia para correlacionar eventos que causaron el congelamiento
const int64_t sf_start_ms = static_cast<int64_t>(sf.timestamp_ms) - static_cast<int64_t>(sf.frame_ms);
const int64_t sf_end_ms   = static_cast<int64_t>(sf.timestamp_ms);

const uint64_t start_ns = (sf_start_ms > 15) ? static_cast<uint64_t>(sf_start_ms - 15) * 1'000'000ULL : 0;
const uint64_t end_ns   = static_cast<uint64_t>(sf_end_ms + 15) * 1'000'000ULL;
```
Si ningún evento discreto en el buffer de trazas cae en dicha ventana (por ejemplo, porque el frame fue simplemente lento debido a 5.000 draw calls acumuladas), el profiler ya no asigna falsamente un evento ajeno de 4 ms, sino que reporta:
- `dominant_blocker`: `"ComputeOrRasterBound"`
- `confidence`: `"Low"`
- `dominant_blocker_duration_ms`: `0.0`

### 2.4. Desglose Exclusivo de `Pm4Dispatch`
En [`auditProfiler.h`](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.h) y `auditProfiler.cpp`:
```cpp
const double inner_draw_stages_ms =
    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::RenderStatePrep)].total_ms +
    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::DescriptorPrep)].total_ms +
    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::BarriersTransitions)].total_ms +
    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::BufferPrep)].total_ms +
    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::CommandRecording)].total_ms;

snapshot.pm4_dispatch_inclusive_ms =
    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Dispatch)].total_ms;

snapshot.pm4_dispatch_exclusive_ms =
    std::max(0.0, snapshot.pm4_dispatch_inclusive_ms - inner_draw_stages_ms);
```
Esto elimina la doble contabilización en los gráficos y porcentajes de tiempo acumulado.

---

## 3. Verificación Mediante Tests Unitarios

Todas las correcciones fueron integradas en [`tests/AuditProfilerTests.cpp`](file:///c:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp) y validadas con 27 tests unitarios automatizados que pasaron exitosamente (100% pass):
- `AuditProfilerTest.ExecutionPhaseLandingTransitionsAndAdvancePhase`
- `AuditProfilerTest.Stage8_1CpuGpuSeparationAndExclusiveDispatch`
- `AuditProfilerTest.Stage8_1FrameTimeSeriesExport`
- `AuditProfilerTest.Stage8_1OptimizationFlagsConfig`
- `AuditProfilerTest.CorrelatesHitchesWithTraceEventRing`
