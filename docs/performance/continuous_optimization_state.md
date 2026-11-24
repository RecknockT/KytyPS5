# Estado Actual de Optimización Continua

- **SHA actual:** `086e7e80f` + **OPT-10** (`--vertex-input-cache` default `true`, pendiente de commit).
- **Último checkpoint estable:** `086e7e80f` (**20,25 FPS** mediana con OPT-08).
- **Última optimización aceptada:** **OPT-10** (`--vertex-input-cache` / `--no-vertex-input-cache`, default `true`): en 3+3 corridas alternadas con el mismo ejecutable en Sky Garden (`StreamBuffer Ring Waits = 0,00 ms`), la mediana subió de `19,65 FPS` (`50,93 ms/f`, `RenderStatePrep = 20,70 ms/f`) a **`20,23 FPS`** (`+2,95%`, `49,48 ms/f`, `RenderStatePrep = 19,35 ms/f`, `-1,35 ms/f`, `p95 <= 50,86 ms` en 3/3 corridas vs `66,5-66,7 ms` en 2/3 OFF).
- **Cuellos de botella actuales (`Thread_Gpu`, ~5.260 draws/frame):**
  1. `RenderStatePrep`: `19,35 ms/frame` (`GetGraphicsPrograms` + `Targets` + `EnsureGraphicsPipeline`).
  2. `Pm4Dispatch` (exclusivo): `10,25 ms/frame`.
  3. `DescriptorPrep`: `8,28 ms/frame`.
  4. `BufferReadback` wait: `6,44 ms/frame`.
  5. `CommandRecording`: `4,34 ms/frame`.
- **Resultados de tests:** `ctest` 10/10 suites críticas PASSED.
- **Próxima acción exacta:** Evaluar **OPT-12 (`--read-buffer-reuse` y `--extended-stream-buffer`)** en Sky Garden A/B, atacar **OPT-11 (`WalkSrt` / `ResourceMaterialization`)** y completar la comparativa A/B/C en Sky Garden.
