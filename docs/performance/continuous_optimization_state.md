# Estado Actual de Optimización Continua

- **SHA actual:** `6908216ac` + OPT-08 (en commit).
- **Último checkpoint estable:** `6908216ac` (OPT-04, `19,91 FPS` / `50,25 ms/f`)
- **Última optimización aceptada:** **OPT-08** (`ResolvedTextureCacheSize = 512` en `render.h`), reduciendo `DescriptorPrep` de `9,20 ms/f` (`1,69 µs/draw`) a `8,23 ms/f` (`1,48 µs/draw`, `-12,4%` por draw).
- **Cuellos de botella actuales (confirmados por micro-profiler):**
  1. `GetGraphicsPrograms` (`RenderStatePrep`): **~5.554 ns/draw** (~91% de `RenderStatePrep`), con `WalkSrt` en `1.050 ns/call` y `prep+ps` en `4.761 ns/draw`.
  2. `Pm4Dispatch` (exclusivo): `10,01 - 10,19 ms/frame`.
  3. `DescriptorPrep`: `8,23 ms/frame`.
  4. `BufferReadback` wait: `6,26 - 8,23 ms/frame` + `StreamBuffer Ring Waits` (`0 - 511 ms` con 64 MiB).
  5. `CommandRecording`: `4,42 ms/frame`.
- **Resultados de tests:** `ctest` 10/10 suites críticas PASSED.
- **Próxima acción exacta:** Atacar `GetGraphicsPrograms` (`WalkSrt` + `PrepareProgram(VS/PS)`) y validar `--read-buffer-reuse` + `--extended-stream-buffer` (256 MiB) en Sky Garden.
