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

