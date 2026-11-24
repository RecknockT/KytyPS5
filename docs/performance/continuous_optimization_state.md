# Estado Actual de Optimización Continua

- **SHA actual:** `180420631` + OPT-04 (en commit).
- **Último checkpoint estable:** `180420631` (OPT-01/02/03)
- **Última optimización aceptada:** **OPT-04** (Lock-free TLS cache en `ShaderGetMappedData` + eliminación de `TouchImage` redundante en `ValidateAndTouchCachedImage`). Sky Garden subió de **19,51 FPS** (`51,34 ms/f`) a **19,91 FPS** (`50,25 ms/f`).
- **Optimización en curso:** **OPT-05** — Reducción de `RenderStatePrep` (**19,28 ms/frame**) y `BufferReadback` / `PriorityOperation` (**6,26 ms/f** + **21,83 ms/f** waits).
- **Cuellos de botella actuales (sobre 50,25 ms/frame):**
  1. `RenderStatePrep`: 19,28 ms/frame (38,4%)
  2. `Pm4Dispatch` (exclusivo): 10,01 ms/frame (19,9%)
  3. `DescriptorPrep`: 9,20 ms/frame (18,3%)
  4. `CommandRecording`: 4,37 ms/frame (8,7%)
- **Resultados de tests:** `ctest` 10/10 suites críticas PASSED.
- **Próxima acción exacta:** Optimizar `GetGraphicsPrograms` / `PrepareProgram` (cableando `ProgramCacheMruEnabled()` con fast-path de programas para evitar `BuildStageStaticKey` y `m_program_cache.find` por draw) y reducir contention de `m_lock` en `MaterializeColorClear` / `FindTexture`.
