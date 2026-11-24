# Estado Actual de Optimización Continua

- **SHA actual:** `f5c460adc` + toggle OPT-08 (`--resolve-texture-cache-512` default `true`) + `--profile-programs` (en commit).
- **Último checkpoint estable:** `f5c460adc` (OPT-08 revalidada: **20,25 FPS** mediana vs `19,79 FPS` con 64 entradas).
- **Última optimización aceptada:** **OPT-08** (`--resolve-texture-cache-512` / `--no-resolve-texture-cache-512`, default `true`): en 3+3 corridas alternadas con el mismo ejecutable en Sky Garden (`StreamBuffer Ring Waits = 0,00 ms`), la mediana subió de `19,79 FPS` (`50,65 ms/f`, `DescriptorPrep = 9,29 ms/f`, `hit = 84,24%`) a **`20,25 FPS`** (`+2,32%`, `49,42 ms/f`, `DescriptorPrep = 8,20 ms/f`, `hit = 95,15%`, `-69%` conflict misses).
- **Hallazgo crítico de OPT-09 (`--profile-programs`, 7,38M draws):**
  1. `ShaderApplyAttribSemantics` (`src/graphics/shader/shader.cpp:194`): **`4.932 - 7.084 ns/draw`** (`6.403 - 9.610 ns/call`, ~57% de todo `GetGraphicsPrograms` — lee `attrib[in.semantic]` 5 veces y `sharp[0..3]` 4 veces por atributo desde memoria guest/WC).
  2. `WalkSrt` (`src/graphics/shader/recompiler/ir/passes/SrtWalker.cpp:1452`): **`2.146 ns/draw`** (`1.167 ns/call`).
  3. `Pm4Dispatch` (exclusivo): `10,30 ms/frame`.
  4. `DescriptorPrep`: `8,20 ms/frame`.
  5. `BufferReadback` wait: `6,39 ms/frame`.
  6. `CommandRecording`: `4,29 ms/frame`.
- **Resultados de tests:** `ctest` 10/10 suites críticas PASSED.
- **Próxima acción exacta:** Atacar **OPT-10 (`ShaderApplyAttribSemantics`)** reduciendo lecturas WC/guest redundantes y separando/cacheando estado estático de vértices con invalidación segura, seguido de **OPT-11 (`WalkSrt`)**, **OPT-12 (`--read-buffer-reuse` / `--extended-stream-buffer`)** y comparación completa A/B/C en Sky Garden.
