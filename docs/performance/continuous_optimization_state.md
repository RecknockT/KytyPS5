# Estado Actual de Optimización Continua

- **SHA actual:** `045798338` + checkpoint local OPT-01/02/03 (pendiente de commit).
- **Último checkpoint estable:** `045798338`
- **Última optimización aceptada:** **OPT-03** (RenderTarget cache con soporte DCC/CMASK + caché de 64 entradas para `ResolveTexture`). Sky Garden subió de **16,06 FPS** (`62,41 ms/f`) a **19,51 FPS** (`51,34 ms/f`).
- **Optimización en curso:** **OPT-04** — Investigación de los restantes **20,11 ms/frame en `RenderStatePrep`** y **9,26 ms/frame en `DescriptorPrep`** + **10,04 ms/frame en `Pm4Dispatch`**.
- **Cuellos de botella actuales (sobre 51,34 ms/frame):**
  1. `RenderStatePrep`: 20,11 ms/frame (39,2%)
  2. `Pm4Dispatch` (exclusivo): 10,04 ms/frame (19,6%)
  3. `DescriptorPrep`: 9,26 ms/frame (18,0%)
  4. `CommandRecording`: 4,47 ms/frame (8,7%)
- **Resultados de tests:** `audit_profiler_tests.exe` 42/42 PASSED; `ctest` 9/9 suites críticas PASSED.
- **Próxima acción exacta:** Inspeccionar por qué `RenderStatePrep` sigue costando `3,66 µs/draw` (20,11 ms/f) tras `OPT-03` (probablemente `GetGraphicsPrograms` y/o `ValidateAndTouchCachedImage` lock contention / `BindRenderTarget`) y optimizar el mayor costo restante.
