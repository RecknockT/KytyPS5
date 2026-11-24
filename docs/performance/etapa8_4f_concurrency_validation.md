# Reconstrucción 8.4F — registro de validación

## Estado inicial (2026-11-23)

- Rama: `main`; SHA: `14185ebd9feed76015ebf8760ffad391e3c3ef50`.
- `git status --short --ignore-submodules`: limpio antes de modificar código.
- No se recuperó ni se utilizó el commit perdido.
- Baseline: `ctest --test-dir _Build/windows --output-on-failure --timeout 90 -R "^(audit_profiler|stream_buffer_ring|pipeline_cache|cadence|cp_sequencer|draw_prep)"`.
- Resultado: 4/4 aprobadas: audit_profiler, cp_sequencer, draw_prep, stream_buffer_ring.
- La expresión no seleccionó pruebas independientes pipeline_cache/cadence en este build.
- Estos resultados iniciales no demuestran integridad de snapshots ni ausencia de bloqueo en el juego.

## Auditoría inicial

- `ResolveSubmission` consume y libera mediante `Drain` preparaciones posteriores a la operación actual; las operaciones draw posteriores conservan sus posiciones en OpStream.
- DrawIndex/DrawAuto publican snapshots solamente cuando DrawPrep está activo; compute e indirect multi no capturan registros.
- La inserción de una nueva pipeline actualiza el MRU sin actualizar los inputs asociados.
- WAIT_REG_MEM se evalúa en el productor, que reintenta dentro de la misma submission. El resolver permanece esperando más operaciones en lugar de devolver el control a otras colas. La relación con el bloqueo real de Astro Bot aún debe verificarse.

## Resultados de la reconstrucción

- Release reconstruido para kyty_emulator, cp_sequencer_tests, draw_prep_tests y shader_recompiler_compute_tests.
- Suite final solicitada: 4/4 aprobadas; cp_sequencer y draw_prep ejecutados también directamente.
- CpSequencer: 10.000 operaciones con snapshots y ring de 4 entradas, cinco clases de consumidor, estado Context/UserConfig/Shader y direcciones indirectas; saturación y cancelación; secuenciador real esperando respuesta durante shutdown; 7.000 operaciones intercaladas por OpStream.
- DrawPrep: saturación real de 64 slots, 2.048 y 8.192 draws entre productor/consumidor con compute/DMA/barreras; tickets inválidos; cancelación; reinicio de metadatos en ranuras recicladas.
- Regresión dirigida: un worker solicita una lectura síncrona al resolver mientras éste espera CommitPublished. El callback atiende la solicitud fuera del mutex y la preparación termina. Sin atenderla existe el ciclo de espera reproducido en el juego.
- Regresión MRU real: --shader-cache-only crea una pipeline A, la reutiliza, inserta B con otras etapas y vuelve a A. También se ejecutan las regresiones de persistencia/corrupción de caché y VertexPushTopCache.
- El límite por defecto del ring se conserva en 64; aumentar su capacidad no soluciona los problemas detectados.
- Se preservan las barreras; WriteData, DMA y writeback no permiten que preparaciones futuras adelanten sus lecturas sobre esas mutaciones.
- Véanse los informes de aceptación y de arranque para resultados reales y límites de estas pruebas.
