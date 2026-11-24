# Registro Continuo de Optimización — KytyPS5 (Astro Bot PPSA21567)

## Baseline de Referencia (2026-11-24)
- **Commit:** `045798338` (rama `main`)
- **Captura:** `_TestLogs/astrobot_profile_a_baseline_20261124_140803.json`
- **Escena:** Sky Garden (642 frames, 37,48 s, ~5.285 draws/frame)
- **Rendimiento:** **17,12 FPS** (`58,46 ms/frame`)
- **Cuellos de botella principales en `Thread_Gpu`:**
  1. `RenderStatePrep`: `21,65 ms/frame` (37,1%)
  2. `DescriptorPrep`: `13,80 ms/frame` (23,6%)
  3. `Pm4Dispatch` (exclusivo): `10,45 ms/frame` (17,9%)
  4. `CommandRecording` + `End`: `4,71 ms/frame` (8,1%)

---

## Iteración 0 — Auditoría de Telemetría y Validación de Arranque B/C
- **Acciones:**
  - Instrumentación tripartita (`Queue` / `CPU` / `GPU Wait`) verificada en `AuditProfiler`.
  - Validación real de arranque (30 s) en Perfil B (`--cp-sequencer --no-draw-prep`, 1.525 frames, `worldmap` OK) y Perfil C (`--cp-sequencer --draw-prep --draw-prep-workers 4`, 1.564 frames, `worldmap` OK, 0 fallbacks en `DrawPrep`).
  - Confirmado que `DrawPrep` reduce `RenderStatePrep` de `13,3 µs/draw` a `4,6 µs/draw` en escena de arranque/mapa, pero `ResolveRenderColorTarget` / `ResolveRenderDepthTarget` y `DescriptorPrep` siguen corriendo 100% en `Thread_Gpu`.
- **Estado:** Completada. Informe en `docs/performance/etapa8_4f_real_gameplay_ab_analysis.md`.

---

## Iteración 1 — OPT-01 + OPT-02 + OPT-03 (Caché de Render Targets + Caché de 64 entradas `ResolveTexture`)
- **Archivos modificados:**
  - `src/graphics/guest_gpu/hardwareContext.h`: igualdad C++20 `operator==` en registros `RenderTarget`, `DepthRenderTarget`, `RenderControl`, `DepthControl`, `StencilControl`, `StencilMask`.
  - `src/graphics/host_gpu/renderer/cache/textureCache.h/.cpp`: contador `m_structure_generation` en `RegisterImage`/`UnregisterImage` y fast-path `ValidateAndTouchCachedImage(ImageId)`.
  - `src/graphics/host_gpu/renderer/colorRenderTarget.cpp`, `depthRenderTarget.cpp`, `render.h`: caché granular por slot de color (incluyendo ruta segura con `MaterializeColorClear` para DCC/CMASK) y profundidad, controlada por `--render-target-cache` / `--no-render-target-cache`.
  - `src/graphics/host_gpu/renderer/pipeline/descriptors.h/.cpp`: `ImageMemo` por módulo + caché directa de 64 entradas `m_resolved_texture_cache` para `ResolveTexture` independiente de cambios de shader, controlada por `--descriptor-cache`.
  - `src/common/emulatorConfig.h/.cpp`, `src/main.cpp`: flags CLI `--render-target-cache` / `--no-render-target-cache`.
- **Resultados medidos (Sky Garden automatizado, `sky_garden_fast_replay.json`, Perfil A):**
  - Antes (`20261124_144028_173_A`, 482 frames): **16,06 FPS** (`62,41 ms/f`), `RenderStatePrep` = `23,46 ms/f` (`3,97 µs/op`), `DescriptorPrep` = `15,48 ms/f` (`2,62 µs/op`).
  - Después (`20261124_145334_007_A`, 586 frames): **19,51 FPS** (**+21,5%**, `51,34 ms/f`), `RenderStatePrep` = `20,11 ms/f` (`3,66 µs/op`), `DescriptorPrep` = **`9,26 ms/f` (`1,68 µs/op`, -35,9% por draw)**.
  - Tests: `audit_profiler_tests.exe` 42/42 PASSED; `cp_sequencer`, `draw_prep`, `stream_buffer_ring`, `depth_bits`, `texture_cache_layered_image`, `texture_cache_image_views`, `texture_cache_storage_sampled`, `texture_cache_depth_readback`, `color_render_target_1d` PASSED.
- **Estado:** Validada y aceptada.

---

## Iteración 3 — OPT-05 / OPT-06 / OPT-07 (Descartadas por regresión o sin ganancia) + OPT-08 (Expansión a 512 entradas en `ResolveTexture` cache)
- **Experimentos descartados y revertidos:**
  - `OPT-05` (`ProgramCacheMru` 256 entradas): `RenderStatePrep` subió de `19,28` a `21,22 ms/f` (`18,70 FPS`). Revertida.
  - `OPT-06` (`--srt-resource-cache` 16 entradas): comparar/copiar hasta 32 KiB de `srt_scratch` subió `RenderStatePrep` a `22,53 ms/f` (`18,18 FPS`). Revertida.
  - `OPT-07` (`--async-fast-pump` + `NoteTargetDraw` MRU): sin ganancia neta (`18,17 FPS`). Revertida.
  - `-Transfers Combined` sobre `OPT-04`: `18,75 FPS` (`BufferReadback = 8,05 ms/f`). Sin mejora.
- **Micro-profiling de `PrepareDrawRenderState` (`renderDraw.cpp` / `pipelineCache.cpp` / `ResourceMaterialization.cpp`):**
  - `GetGraphicsPrograms`: **5.554 ns/draw** (~91% de `RenderStatePrep`), con `WalkSrt` en `1.050 ns/call` y `prep+ps` en `4.761 ns/draw`.
  - `Targets`: **466 ns/draw** (~8%).
  - `EnsureGraphicsPipeline`: **86,8 ns/draw** (~1,5%).
- **OPT-08 implementada:**
  - `src/graphics/host_gpu/renderer/render.h`: `ResolvedTextureCacheSize` ampliado de `64` a `512` entradas.
  - **Resultados medidos (`20261124_160012_889_A`, 570 frames, 5.569 draws/f):**
    - `DescriptorPrep` bajó de **`9,20 ms/f` (`1,69 µs/draw`)** a **`8,23 ms/f` (`1,48 µs/draw`, -12,4% por draw)**.
    - `Thread_Gpu CPU` bajó de `26.000 ms` a `24.765 ms` (FPS total `18,99` en esta ventana por `511,85 ms` de `StreamBuffer Ring Waits` y `8,23 ms/f` de `BufferReadback`).
  - Tests: `ctest` 10/10 suites críticas PASSED.
- **Estado:** OPT-08 validada en `DescriptorPrep` y aceptada (revalidada en Iteración 4 con toggle A/B).

---

## Iteración 4 — Revalidación A/B 3+3 de OPT-08 (64 vs 512) y Microprofiling Profundo OPT-09 (`--profile-programs`)
- **Aislamiento de OPT-08 (`--resolve-texture-cache-512` / `--no-resolve-texture-cache-512`):**
  - Mismo ejecutable, 3 pares alternados (64 vs 512) en Sky Garden (`sky_garden_fast_replay.json`, Perfil A, `StreamBuffer Ring Waits = 0,00 ms` en las 6 corridas):
    - **64 entradas:** R1 `18,65 FPS` (`hit=84,92%`, `conflicts=728.874`), R3 `19,79 FPS` (`hit=81,24%`, `conflicts=949.502`), R5 `20,12 FPS` (`hit=84,24%`, `conflicts=807.632`). **Mediana: `19,79 FPS`**, `50,65 ms/f`, `DescriptorPrep = 9,29 ms/f`.
    - **512 entradas:** R2 `20,15 FPS` (`hit=94,61%`, `conflicts=277.769`), R4 `20,25 FPS` (`hit=95,15%`, `conflicts=248.377`), R6 `20,50 FPS` (`hit=95,29%`, `conflicts=244.900`). **Mediana: `20,25 FPS` (`+2,32%`)**, `49,42 ms/f`, `DescriptorPrep = 8,20 ms/f` (`-1,09 ms/f`), **`-69%` conflict misses**.
  - **Decisión:** OPT-08 validada 3/3 en FPS global, frametime y `DescriptorPrep`; activada por defecto (`resolve_texture_cache_512_enabled = true`).
- **OPT-09 (`--profile-programs`, `20261124_162305_182_A`, 7.381.173 draws medidos):**
  - `GetGraphicsPrograms`: **8.636 ns/draw**
    - `PrepareProgramVS`: **5.189 ns/draw** -> **`ShaderApplyAttribSemantics` = `4.932 ns/draw` (`6.403 - 9.610 ns/call`)** (¡~57% de todo `GetGraphicsPrograms`!).
    - `GetProgramVS + GetProgramPS`: **2.919 ns/draw** -> `FindModule = 3.017 ns/draw` -> `MaterializeResources = 2.805 ns/draw` -> **`WalkSrt = 2.146 ns/draw` (`1.167 ns/call`)**.
    - `ShaderGetMappedData`: `49,28 ns/call` (`ShaderMapTLS` hit rate `89,29% - 90,31%`).
    - Resto (`PumpAsync`, `PrepareProgramPS`, `ShaderStaticInputPS`, `GetShaderParams`, `BuildStageStaticKey`, `ProgramCacheFind`, `BuildResourceSpec`, `PixelRawParamMask`, `NoteTargetDraw`, `ShaderDetectBuffers`): < `400 ns/draw` combinados.
- **Estado:** OPT-08 aceptada por defecto; OPT-09 completado y listo para atacar `ShaderApplyAttribSemantics` en OPT-10.


---

## Iteración 2 — OPT-04 (Lock-Free `ShaderGetMappedData` TLS Cache + Eliminación de `TouchImage` redundante)
- **Archivos modificados:**
  - `src/graphics/shader/shader.cpp`: `g_shader_map_generation` + caché `thread_local` lock-free de 64 entradas en `ShaderGetMappedData` (evita ~11.000 bloqueos de `g_shader_map_mutex` y `unordered_map::find` por frame).
  - `src/graphics/host_gpu/renderer/cache/textureCache.cpp`: eliminación de `TouchImage(*image)` redundante en `ValidateAndTouchCachedImage` (los callers inmediatamente llaman `BindImage`/`BindRenderTarget`/`FindTexture`, que ya actualizan el LRU).
- **Resultados medidos (Sky Garden automatizado, `sky_garden_fast_replay.json`, Perfil A):**
  - Antes (`OPT-03`, `20261124_145334_007_A`, 586 frames): **19,51 FPS** (`51,34 ms/f`), `RenderStatePrep` = `20,11 ms/f`, `DescriptorPrep` = `9,26 ms/f`.
  - Después (`OPT-04`, `20261124_150509_983_A`, 598 frames): **19,91 FPS** (**+2,05%**, `50,25 ms/f`), `RenderStatePrep` = **`19,28 ms/f` (-0,83 ms/f)**, `DescriptorPrep` = `9,20 ms/f`, `Pm4Dispatch (incl)` = `47,10 ms/f` (`-1,03 ms/f`).
  - Tests: `ctest` 10/10 suites críticas PASSED.
- **Estado:** Validada y aceptada.

---

## Iteración 3 — OPT-05 / OPT-06 / OPT-07 (Descartadas por regresión o sin ganancia) + OPT-08 (Expansión a 512 entradas en `ResolveTexture` cache)
- **Experimentos descartados y revertidos:**
  - `OPT-05` (`ProgramCacheMru` 256 entradas): `RenderStatePrep` subió de `19,28` a `21,22 ms/f` (`18,70 FPS`). Revertida.
  - `OPT-06` (`--srt-resource-cache` 16 entradas): comparar/copiar hasta 32 KiB de `srt_scratch` subió `RenderStatePrep` a `22,53 ms/f` (`18,18 FPS`). Revertida.
  - `OPT-07` (`--async-fast-pump` + `NoteTargetDraw` MRU): sin ganancia neta (`18,17 FPS`). Revertida.
  - `-Transfers Combined` sobre `OPT-04`: `18,75 FPS` (`BufferReadback = 8,05 ms/f`). Sin mejora.
- **Micro-profiling de `PrepareDrawRenderState` (`renderDraw.cpp` / `pipelineCache.cpp` / `ResourceMaterialization.cpp`):**
  - `GetGraphicsPrograms`: **5.554 ns/draw** (~91% de `RenderStatePrep`), con `WalkSrt` en `1.050 ns/call` y `prep+ps` en `4.761 ns/draw`.
  - `Targets`: **466 ns/draw** (~8%).
  - `EnsureGraphicsPipeline`: **86,8 ns/draw** (~1,5%).
- **OPT-08 implementada:**
  - `src/graphics/host_gpu/renderer/render.h`: `ResolvedTextureCacheSize` ampliado de `64` a `512` entradas.
  - **Resultados medidos (`20261124_160012_889_A`, 570 frames, 5.569 draws/f):**
    - `DescriptorPrep` bajó de **`9,20 ms/f` (`1,69 µs/draw`)** a **`8,23 ms/f` (`1,48 µs/draw`, -12,4% por draw)**.
    - `Thread_Gpu CPU` bajó de `26.000 ms` a `24.765 ms` (FPS total `18,99` en esta ventana por `511,85 ms` de `StreamBuffer Ring Waits` y `8,23 ms/f` de `BufferReadback`).
  - Tests: `ctest` 10/10 suites críticas PASSED.
- **Estado:** OPT-08 validada en `DescriptorPrep` y aceptada (revalidada en Iteración 4 con toggle A/B).

---

## Iteración 4 — Revalidación A/B 3+3 de OPT-08 (64 vs 512) y Microprofiling Profundo OPT-09 (`--profile-programs`)
- **Aislamiento de OPT-08 (`--resolve-texture-cache-512` / `--no-resolve-texture-cache-512`):**
  - Mismo ejecutable, 3 pares alternados (64 vs 512) en Sky Garden (`sky_garden_fast_replay.json`, Perfil A, `StreamBuffer Ring Waits = 0,00 ms` en las 6 corridas):
    - **64 entradas:** R1 `18,65 FPS` (`hit=84,92%`, `conflicts=728.874`), R3 `19,79 FPS` (`hit=81,24%`, `conflicts=949.502`), R5 `20,12 FPS` (`hit=84,24%`, `conflicts=807.632`). **Mediana: `19,79 FPS`**, `50,65 ms/f`, `DescriptorPrep = 9,29 ms/f`.
    - **512 entradas:** R2 `20,15 FPS` (`hit=94,61%`, `conflicts=277.769`), R4 `20,25 FPS` (`hit=95,15%`, `conflicts=248.377`), R6 `20,50 FPS` (`hit=95,29%`, `conflicts=244.900`). **Mediana: `20,25 FPS` (`+2,32%`)**, `49,42 ms/f`, `DescriptorPrep = 8,20 ms/f` (`-1,09 ms/f`), **`-69%` conflict misses**.
  - **Decisión:** OPT-08 validada 3/3 en FPS global, frametime y `DescriptorPrep`; activada por defecto (`resolve_texture_cache_512_enabled = true`).
- **OPT-09 (`--profile-programs`, `20261124_162305_182_A`, 7.381.173 draws medidos):**
  - `GetGraphicsPrograms`: **8.636 ns/draw**
    - `PrepareProgramVS`: **5.189 ns/draw** -> **`ShaderApplyAttribSemantics` = `4.932 ns/draw` (`6.403 - 9.610 ns/call`)** (¡~57% de todo `GetGraphicsPrograms`!).
    - `GetProgramVS + GetProgramPS`: **2.919 ns/draw** -> `FindModule = 3.017 ns/draw` -> `MaterializeResources = 2.805 ns/draw` -> **`WalkSrt = 2.146 ns/draw` (`1.167 ns/call`)**.
    - `ShaderGetMappedData`: `49,28 ns/call` (`ShaderMapTLS` hit rate `89,29% - 90,31%`).
    - Resto (`PumpAsync`, `PrepareProgramPS`, `ShaderStaticInputPS`, `GetShaderParams`, `BuildStageStaticKey`, `ProgramCacheFind`, `BuildResourceSpec`, `PixelRawParamMask`, `NoteTargetDraw`, `ShaderDetectBuffers`): < `400 ns/draw` combinados.
- **Estado:** OPT-08 aceptada por defecto; OPT-09 completado y listo para atacar `ShaderApplyAttribSemantics` en OPT-10.



