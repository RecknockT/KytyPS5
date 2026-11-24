# Etapa 8.4F — Auditoría de Telemetría, Arranque B/C y Análisis Estático de Hot Paths

**Commit de referencia:** `045798338028714037f2bd56fafeaa22bea84c14` (rama `main`)  
**Hardware:** AMD Ryzen 7 9700X (8C/16T), NVIDIA GeForce RTX 5080 16 GB, 32 GB DDR5-6000, Windows 11 x64  
**Captura Baseline (Perfil A - Sky Garden):** `_TestLogs/astrobot_profile_a_baseline_20261124_140803.json`

---

## 1. Auditoría de Consistencia de Telemetría (Perfil A — 642 frames / 37.48 s)

### Resolución de las inconsistencias observadas

| Métrica | Valor reportado | Por frame (642) | Naturaleza real de la medición |
|---|---:|---:|---|
| `gpu_thread_wall_clock_ms` | 37.477,30 ms | 58,38 ms | Tiempo real de pared del intervalo capturado. |
| `gpu_thread_busy_ms` | 37.502,82 ms | 58,42 ms | Tiempo **inclusivo de pared** dentro de `GuestGpu::ThreadRun` (`command()` + `Process(submission)`), incluyendo esperas y yields internos. |
| `gpu_thread_cpu_time_ms` | 32.906,25 ms | 51,26 ms | Tiempo de **CPU en el SO** (`GetThreadTimes` en Windows, quantum de 15,625 ms). Excluye ~4.571 ms (7,12 ms/frame) en que `Thread_Gpu` estuvo dormido/esperando. |
| `Pm4Dispatch` (inclusivo) | 35.224,09 ms | 54,87 ms | Tiempo **inclusivo** del loop de despacho PM4: incluye `RenderStatePrep`, `DescriptorPrep`, `BarriersTransitions`, `BufferPrep`, `CommandRecording`, y parte de `Submit`/`SyncWaits`. |
| `Pm4Dispatch` (exclusivo) | 6.707,62 ms | 10,45 ms | `Pm4Dispatch (35.224) - [RenderStatePrep (13.897) + DescriptorPrep (8.862) + Barriers (1.724) + BufferPrep (1.071) + CommandRecording (2.962)]`. |
| `RenderStatePrep` | 13.897,23 ms | 21,65 ms | Incluye `GetGraphicsPrograms` (`PipelineKeyBuild` 0,11 ms + `PipelineLookup` 0,38 ms) y `ResolveRenderColorTarget` / `ResolveRenderDepthTarget`. |
| `DescriptorPrep` | 8.862,18 ms | 13,80 ms | `PrepareBindings` + `PrepareGraphicsBindings` (`FindBuffers`, `RebindImages`, `RebindBuffers`). |
| `BarriersTransitions` | 1.724,14 ms | 2,69 ms | `AcquireRenderTargets` y barreras Vulkan. |
| `CommandRecording` + `End` | 3.021,87 ms | 4,71 ms | `vkCmdDraw*`, binds de pipeline/descriptores y `vkEndCommandBuffer`. |
| `BufferPrep` | 1.071,21 ms | 1,67 ms | `AcquireVertexBuffers` + `PrepareIndexBuffer`. |
| `SyncWaits` (`PriorityOperation` + `BufferReadback`) | 18.067,15 ms | 28,14 ms | **Suma acumulada multi-hilo**: `MasterSemaphore::Wait` es invocado concurrentemente por hilos del juego (CPU guest) y `Thread_Gpu`. Como `Thread_Gpu` solo durmió 7,12 ms/frame, la mayoría de esos 28,14 ms ocurrieron en hilos guest esperando readbacks/priority ops en paralelo. |
| `Submission Queue` | 829.035,26 ms | — | Suma paralela de las latencias de 12.160 submissions apiladas esperando en `m_queues` (~68,2 ms promedio por submission) mientras `Thread_Gpu` estaba saturado al 87,8% CPU. |

### Estado de Timestamps Físicos de GPU
- `gpu_timestamps_supported = false`, `gpu_queries_recorded = 0`.
- `Common::AuditProfiler::RecordGpuExecutionTime` está declarado e implementado en `auditProfiler.cpp`, pero **no tiene ningún llamador activo en el renderer Vulkan**.
- Por lo tanto, no existen aún timestamps de hardware GPU; sin embargo, el hecho de que `Thread_Gpu` consuma `87,8%` de un núcleo físico de CPU, `StreamBufferWait = 0,00 ms` y `Swapchain = 0,00 ms` confirma que **`Thread_Gpu` CPU-bound es el cuello de botella inmediato** que frena el drenaje de la cola.

---

## 2. Validación de Arranque de los Perfiles Experimentales (B y C)

Ejecución de 30 segundos en `045798338` mediante `stage8_4f_run.ps1 -BootSeconds 30`:

| Perfil | Flags | Estado de Arranque | Frames (30 s) | Draws/frame | `RenderStatePrep` | `DescriptorPrep` |
|---|---|---|---:|---:|---:|---:|
| **B** | `--cp-sequencer --no-draw-prep` | **OK** (superó ambos logos, cargó `worldmap`) | 1.525 (50,81 FPS eff.) | 147,7 | 1,971 ms/frame (13,3 µs/draw) | 0,818 ms/frame |
| **C** | `--cp-sequencer --draw-prep --draw-prep-workers 4` | **OK** (superó ambos logos, cargó `worldmap`) | 1.564 (52,13 FPS eff.) | 205,6 | 0,951 ms/frame (**4,6 µs/draw**) | 0,818 ms/frame |

- El congelamiento histórico de arranque con `CpSequencer` **está resuelto** en `045798338`.
- En Perfil C, `DrawPrep` procesó `333.280` draws con **0 fallbacks** (`committed = 333.279`), reduciendo el costo por draw de `RenderStatePrep` de **13,3 µs a 4,6 µs** (~2,9x menos).
- Nota pendiente: Las capturas comparativas A/B/C en la escena pesada de **Sky Garden (~5.285 draws/frame)** deben realizarse manualmente con cámara fija (F9) usando `stage8_4f_gameplay_benchmark.cmd` porque requieren posicionar la cámara del juego.
- Se observó `snapshot-backpressure` en los logs de diagnóstico porque `SnapshotCount()` sigue en **64 ranuras** por defecto (`capacity: 64`).

---

## 3. Auditoría Estática de Hot Paths (5.285 draws/frame en Sky Garden)

### A. `RenderStatePrep` (`21,65 ms/frame` = 37,1% de `Thread_Gpu`)
Ubicación: `src/graphics/host_gpu/renderer/renderDraw.cpp:898` (`RenderExecutor::PrepareDrawRenderState`).
- Lo único que `DrawPrep` descarga actualmente es `GetGraphicsPrograms()` (resolución de programas VS/PS y vertex input info).
- Con `DrawStateDirtyCheckEnabled() == false` (desactivado por regresiones en Sky Garden), en **cada uno de los 5.285 draws/frame** `Thread_Gpu` ejecuta síncronamente:
  - `ResolveRenderColorTarget(buffer, targets.colors[...], slot)` para cada MRT activo.
  - `ResolveRenderDepthTarget(buffer, targets.depth)`.
  - Verificación de `pass_samples` y `BindRenderTarget(...)`.
- Incluso con `DrawPrep` activado, `ResolveRenderColorTarget` y `ResolveRenderDepthTarget` siguen corriendo íntegramente en `Thread_Gpu`.

### B. `DescriptorPrep` (`13,80 ms/frame` = 23,6% de `Thread_Gpu`)
Ubicación: `src/graphics/host_gpu/renderer/pipeline/descriptors.cpp:700` (`PrepareBindings`) y `:954` (`PrepareGraphicsBindings`).
- **0% descargado por `DrawPrep`**. Se ejecuta 100% en `Thread_Gpu` en los 5.285 draws/frame (`3.393.010` llamadas en 642 frames).
- En `PrepareBindings`:
  - `if (Config::DescriptorCacheEnabled() && prepared.cached_module == runtime.module && prepared.cached_resources == runtime.resources ...)` falla cuando `runtime.resources` cambia de puntero/snapshot, forzando `ResolveTexture()` + `BindImage()` + `NativeSampler()` en cada draw.
  - Además, `--binding-reuse` estaba desactivado por defecto en la línea base (`stage8_4f_run.ps1` usa `--no-binding-reuse`).
- En `PrepareGraphicsBindings`: ejecuta `FindBuffers(stages)`, `RebindImages(*stage)` y `RebindBuffers(*stage)` por cada draw.

### C. `Pm4Dispatch` exclusivo (`10,45 ms/frame` = 17,9% de `Thread_Gpu`)
Ubicación: `src/graphics/guest_gpu/graphicsRun.cpp` + `pm4Handlers.cpp`.
- En Perfil A (monohilo), `Thread_Gpu` decodifica y despacha `17.275.145` paquetes PM4 (~26.908 paquetes/frame).
- Con `CpSequencer` (Perfiles B/C), la decodificación PM4 y escritura de registros pasa a `Thread_CpSequencer`, dejando en `Thread_Gpu` el despacho de `OpStream`.

### D. `PriorityOperation` (`14.924` eventos / `21,80 ms/frame` acumulados) y `BufferReadback` (`5.330` eventos / `6,34 ms/frame` acumulados)
Ubicación: `src/graphics/host_gpu/renderer/cache/bufferCache.cpp:320,524,655,1384`, `commandScheduler.cpp:338,366`, `renderContext.cpp:110,244`.
- Cada `BufferReadback` espera el timeline (`BufferReadback`) y luego llama a `WaitPriorityOperations(tick)` (`PriorityOperation`).
- 8,3 readbacks/frame y 23,2 priority waits/frame en hilos guest generan esperas de ~764–938 µs por evento.

---

## 4. Ranking de Propuestas de Optimización (Ordenadas por Impacto / Riesgo)

1. **Prioridad 1 — Caché segura de Render Targets (`ResolveRenderColorTarget` / `ResolveRenderDepthTarget`) con invalidación exacta por registros HW** (`RenderStatePrep`, potencial sobre `21,65 ms/frame`):
   - A diferencia del antiguo `draw-state-dirty-check` (que omitía preparación completa de estado y causaba regresiones), cachear únicamente la resolución de `RenderTarget` / `DepthTarget` invalidando cuando cambien los registros exactos de `CB_COLOR*_BASE`/`PITCH`/`VIEW`/`INFO` y `DB_Z_*`/`HTILE` (o cuando `TextureCache` invalida/recrea imágenes).
   - **Riesgo**: Bajo-medio (validable comparando 1:1 los `RenderColorInfo` / `RenderDepthInfo` generados en tests y capturas).
2. **Prioridad 2 — Optimización de `DescriptorPrep` (`PrepareBindings` / `PrepareGraphicsBindings` + activación segura de `BindingReuse` y caché de `ResolveTexture`/samplers)** (potencial sobre `13,80 ms/frame`):
   - Evaluar `--binding-reuse` por defecto o caché por hash de descriptores/samplers en `PrepareBindings`, reduciendo asignaciones (`std::vector` clears/resizes por draw) y lookups en `TextureCache`/`BufferCache`.
   - **Riesgo**: Bajo (ya existe infraestructura probada de `BindingReuse`).
3. **Prioridad 3 — Validación y ajuste de `CpSequencer` + `DrawPrep` (`SnapshotCount` 64 -> 256 para eliminar `snapshot-backpressure`)**:
   - En Perfil B/C se detectó `snapshot-backpressure` con capacidad 64. Ampliar `SnapshotCount` (y evaluar traslado de más preparación de `RenderStatePrep` a `DrawPrep`).
   - **Riesgo**: Bajo-medio (requiere benchmark comparativo en Sky Garden).
4. **Prioridad 4 — Cablear `RecordGpuExecutionTime` con Vulkan Timestamp Queries**:
   - Para medir el tiempo real de ejecución en la RTX 5080 y verificar cuánto margen GPU real existe hacia 60 FPS.
   - **Riesgo**: Nulo (solo telemetría).
