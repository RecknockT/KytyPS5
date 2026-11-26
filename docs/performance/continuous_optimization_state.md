# Estado Actual de OptimizaciÃƒÂ³n Continua

- **SHA actual / ÃƒÂºltimo checkpoint estable:** `2f76cc50d` (`main`, ÃƒÂ¡rbol de trabajo limpio salvo este estado).
- **ÃƒÅ¡ltima optimizaciÃƒÂ³n aceptada:** **OPT-10** (`--vertex-input-cache` / `--no-vertex-input-cache`, default `true`): en 3+3 corridas alternadas con el mismo ejecutable en Sky Garden (`StreamBuffer Ring Waits = 0,00 ms`), la mediana subiÃƒÂ³ de `19,65 FPS` (`50,93 ms/f`, `RenderStatePrep = 20,70 ms/f`) a **`20,23 FPS`** (`+2,95%`, `49,48 ms/f`, `RenderStatePrep = 19,35 ms/f`, `-1,35 ms/f`, `p95 <= 50,86 ms` en 3/3 corridas vs `66,5-66,7 ms` en 2/3 OFF).
- **Flags y experimentos evaluados sobre `2f76cc50d` y descartados por no mejorar en Sky Garden:**
  - `--read-buffer-reuse`: `19,82 FPS` (`DescriptorPrep` subiÃƒÂ³ de `8,28` a `9,21 ms/f`).
  - `--extended-stream-buffer`: `18,55 FPS` (`StreamBuffer Ring Waits = 0,00 ms`, `BufferReadback = 10,37 ms/f`).
  - `--resource-prep-reuse`: `20,06 FPS` (`RenderStatePrep = 19,27 ms/f`, sin ganancia neta).
  - **OPT-11 (`--srt-walk-cache`, revertida):** `17,40 FPS` vs `18,26 FPS` (`RenderStatePrep` subiÃƒÂ³ `+1,96 ms/f`).
- **Comparativa A / B / C en Sky Garden (`2f76cc50d`, `sky_garden_fast_replay.json`):**
  - **Perfil A** (`--no-cp-sequencer --no-draw-prep`): **20,23 FPS** (`49,48 ms/f`).
  - **Perfil B** (`--cp-sequencer --no-draw-prep`): **15,44 FPS** (`64,87 ms/f`, `RenderStatePrep = 31,85 ms/f`, `BufferReadback = 20,21 ms/f`, `PriorityOperation = 51,89 ms/f`, `snapshot-backpressure` cap 64).
  - **Perfil C** (`--cp-sequencer --draw-prep --draw-prep-workers 4`): **15,37 FPS** (`65,18 ms/f`, `RenderStatePrep = 17,55 ms/f`, `Pm4Dispatch excl = 27,80 ms/f` por `commit-wait` y `snapshot-backpressure`).
- **Cuellos de botella actuales (`Thread_Gpu`, Perfil A, ~5.260 draws/frame):**
  1. `RenderStatePrep`: `19,35 ms/frame`.
  2. `Pm4Dispatch` (exclusivo): `10,25 ms/frame`.
  3. `DescriptorPrep`: `8,28 ms/frame`.
  4. `BufferReadback` wait: `6,44 ms/frame`.
  5. `CommandRecording`: `4,34 ms/frame`.
- **Resultados de tests:** `ctest` 10/10 suites crÃƒÂ­ticas PASSED en `2f76cc50d`.
- **PrÃƒÂ³xima acciÃƒÂ³n exacta:** Binding-reuse validado en seis capturas manuales OFF/ON sobre OPT-10 a 2560x1440: mediana 15,9190 FPS OFF frente a 15,8958 ON (-0,15 %, sin mejora demostrada); mantener default OFF. Informe: `docs/performance/binding_reuse_opt10_manual_2026_11_24.md`. Siguiente foco: identificar el camino crÃƒÂ­tico de las operaciones prioritarias, separando cola, CPU y GPU sin sumar tiempos de hilos solapados. Juego del usuario PID 3948 permanece abierto, ÃƒÂºltima muestra ON; no enviar controles ni cerrarlo. Conservar los cambios de _SaveData de su partida.
