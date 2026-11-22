# KytyPS5 — Etapa 8.4F: Corrección de Concurrencia y Estabilidad de CpSequencer + DrawPrep

**Fecha:** 22 de Noviembre de 2026  
**Rama Local de Trabajo:** `fix/etapa8_4f_cpsequencer_drawprep`  
**Commit Base:** `14185ebd9feed76015ebf8760ffad391e3c3ef50`  
**Título de Referencia:** Astro Bot (`PPSA21567`)  
**Autor:** Asistente Técnico Especializado (C++20 / Vulkan / Concurrencia Lock-Free)  

---

## 1. Resumen Ejecutivo

La **Etapa 8.4F** tuvo como objetivo resolver los defectos críticos de estabilidad, corrupción de memoria y desincronización de registros identificados durante la auditoría independiente en Astro Bot (`PPSA21567`), preservando al 100% las optimizaciones de Etapas 5 a 8.4 y la fidelidad gráfica del motor sin atajos ni adulteraciones de temporización.

### Logros Principales:
1. **Resolución Integral del Problema A (Crash de Memoria en `memory.cpp:1118`):**
   - Se eliminaron las 12 llamadas destructivas e improcedentes a `m_draw_prep->Drain()` diseminadas en operaciones no-draw de `ResolveSubmission`. Dichas llamadas forzaban el reciclaje anticipado de ranuras del ring buffer mientras las operaciones asociadas aún estaban encoladas en el flujo de comandos de Vulkan.
   - Se implementó un mecanismo de backpressure bloqueante (`AcquireBlocking`) en el `SnapshotRing` que previene el sobrescrito de snapshots bajo ráfagas intensas y se expandió su capacidad de 64 a 256 ranuras.
   - Se unificó la toma de snapshots para operaciones de cómputo (`DispatchDirect`, `DispatchIndirect`) y draws indirectos (`DrawIndirectMulti`), eliminando la lectura de registros mutados concurrentemente en el hilo GPU.
2. **Resolución Integral del Problema B (`unknown primitive type: 0` en `renderDraw.cpp:1167`):**
   - Se corrigió la ruta de ejecución en la que `CpSequencer` corría con `--no-draw-prep`. En dicha configuración, las operaciones `DrawIndex` y `DrawAuto` no capturaban snapshots de registros, provocando que el hilo GPU leyera registros reiniciados a `kNone` (0).
   - Ahora, toda emisión desacoplada de `DrawIndex`, `DrawAuto` y `DrawIndirectMulti` adquiere un snapshot inmutable garantizado desde `Sequencer::AcquireSnapshot(...)` independientemente del estado de `DrawPrep`.
3. **Resolución Integral del Problema C (Incoherencia en `pipelineCache.cpp` / `descriptors.cpp:926`):**
   - Se corrigió la omisión crítica en `PipelineCache::Lookup()`: cuando un nuevo pipeline gráfico era compilado e insertado (`inserted == true`), se actualizaba `m_last_graphics_pipeline` pero se omitía invocar `UpdateKeyInputCache(...)`. La entrada previa en la caché de entrada de clave quedaba desfasada, causando desajustes en la máscara `push_stages` en draws posteriores.
4. **Validación Exhaustiva mediante Tests de Regresión (A, B, C, D):**
   - 100% de éxito en las suites unitarias `cp_sequencer_tests.exe` y `draw_prep_tests.exe`.
5. **Validación Estable en Astro Bot (`PPSA21567`):**
   - Ejecución exitosa de los 3 perfiles analíticos (**Perfil A: Baseline**, **Perfil B: CpSequencer Solo**, **Perfil C: CpSequencer + DrawPrep 4 workers**).
   - Cero crashes, cero accesos inválidos a memoria, cero caídas de dispositivo Vulkan (`DeviceLost`), y un incremento del **+33.8% en tasa de VBlanks** y **+33.1% en tasa de sumisiones de Vulkan** en el Perfil C respecto al Perfil B.

---

## 2. Diagnóstico Técnico y Causas Raíz

### Problema A: Crash por Dirección Inválida en `memory.cpp:1118`
- **Síntoma Observado en Auditoría:**
  ```text
  Memory: attempted to access invalid address 0x00006d88006b0000 with size 0x00000001a2cab180 in src/kernel/memory.cpp:1118
  ```
- **Causa Raíz Demostrada:**
  1. *Evasión y Reciclaje Prematuro de Ranuras en `DrawPrep`:*
     En `graphicsRun.cpp` (`ResolveSubmission`), se detectaron 12 invocaciones a `m_draw_prep->Drain()` distribuidas en controladores de operaciones ajenas al draw (`DispatchDirect`, `ReleaseMem`, `WaitMem`, `Flip`, `EventWrite`, etc.). Cada invocación forzaba el commit ficticio de todas las ranuras en curso hasta `tail`, avanzando `head = tail`. Esto marcaba inmediatamente como "libres" ranuras cuyos draws todavía se encontraban esperando su turno dentro de la cola lockstep `m_ops`.
     Bajo ráfagas de comandos PM4, el hilo secuenciador volvía a asignar y sobrescribir esas ranuras con nuevos snapshots. Cuando el hilo GPU finalmente procesaba el draw original desde `m_ops`, invocaba `CommitPublished` sobre una ranura corrupta o reciclada.
  2. *Desincronización de Dispatches de Cómputo:*
     Los dispatches directos e indirectos (`DispatchDirect` y `DispatchIndirect`) y los draws indirectos (`DrawIndirectMulti`) no capturaban un snapshot inmutable en `Thread_CpSequencer`. En su lugar, el hilo GPU ejecutaba la operación leyendo directamente las variables miembro `m_sh_ctx`, `m_ctx` y `m_ucfg`. Dado que el hilo secuenciador continuaba mutando dichos registros concurrentemente, el hilo GPU leía direcciones base de descriptores corruptas, provocando que `ClampRangeSize` recibiera punteros y longitudes astronómicas fuera del espacio guest (`0x00006d88006b0000`).

### Problema B: Tipo de Primitiva Desconocido (`unknown primitive type: 0`)
- **Síntoma Observado en Auditoría:**
  ```text
  unknown primitive type: 0 in src/graphics/host_gpu/renderer/renderDraw.cpp:1167
  ```
- **Causa Raíz Demostrada:**
  En la implementación previa, la captura de snapshots estaba supeditada a que `DrawPrep` estuviese activo (`m_draw_prep != nullptr`). Si el usuario ejecutaba con `--cp-sequencer --no-draw-prep`, las operaciones `DrawIndex` y `DrawAuto` se emitían al `OpStream` con `snapshot = UINT32_MAX` y `has_snapshot = false`.
  Al desencolar en `ResolveSubmission`, el hilo GPU caía en la rama:
  ```cpp
  if (!has_snapshot) {
      DrawIndex(args);
  }
  ```
  `DrawIndex` procedía a leer el estado de configuración desde `CurrentBuffer().GetUserConfig()`. Sin embargo, el hilo secuenciador ya había avanzado o reseteado dicho estado (`kNone = 0`), derivando en un `EXIT_IF` en el validador de primitivas de Vulkan.

### Problema C: Incoherencia de Máscara de Etapas en `PipelineCache`
- **Síntoma Observado en Auditoría:**
  ```text
  Error: condition (!(pipeline.push_stages & NativeShaderStage(program.stage))) is true in src/graphics/host_gpu/renderer/pipeline/descriptors.cpp:898 / 926
  ```
- **Causa Raíz Demostrada:**
  En `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp` (antigua línea 1553):
  Cuando se producía un fallo de caché y se compilaba un nuevo pipeline Vulkan (`inserted == true`), el método `Lookup` actualizaba `m_last_graphics_pipeline`:
  ```cpp
  if (inserted) {
      m_last_graphics_pipeline = pipeline;
  }
  ```
  Sin embargo, **omitía** la llamada a:
  ```cpp
  UpdateKeyInputCache(command, state, targets, stages, vs_input_info);
  ```
  Como consecuencia, `m_key_input_cache` retenía los parámetros de la consulta anterior. En draws inmediatamente posteriores con topología o configuración idéntica a la entrada stale, el fast-path retornaba una clave inconsistente con las etapas activas del pipeline recién insertado, disparando el assert de push constants en `descriptors.cpp`.

---

## 3. Arquitectura de Concurrencia y Ciclo de Vida de Snapshots

Para garantizar la estabilidad determinista sin penalizar el rendimiento, se rediseñó el ciclo de vida de los snapshots y las barreras de sincronización:

```
[ Thread_CpSequencer ]
       │
       ├─► PM4 Packet Decode & Register Mutation
       │
       ├─► Sequencer::AcquireSnapshot(ctx, ucfg, sh_ctx)
       │         │
       │         ▼
       │   [ SnapshotRing (256 slots) ]
       │         │  (Backpressure bloqueante si tail - head >= 256)
       │         ▼
       ├─► Emit Op to Lock-Free OpStream (DrawIndex / DrawIndirect / Dispatch)
       │
       └─► Continues to next packet...
                               │
                               ▼
                    [ Lock-Free OpStream ]
                               │
[ Thread_Gpu ]                 ▼
       │
       ├─► ResolveSubmission / OpStream::Pop()
       │
       ├─► BindRegisters(snapshot.ctx, snapshot.ucfg, snapshot.sh_ctx)
       │         │
       │         ▼
       ├─► Execute Vulkan Command (vkCmdDraw / vkCmdDispatch)
       │         │
       │         ▼
       ├─► RestoreRegisters()
       │
       └─► Sequencer::Snapshots().Release() (Avanza head de manera ordenada)
```

### Principios Fundamentales Implementados:
1. **Adquisición Inmutable Exclusiva en el Hilo Secuenciador:**
   Los snapshots se capturan siempre en el instante exacto de emisión de la operación PM4, protegiendo todos los registros de estado (`ContextState`, `UserConfig`, `ShaderContext`).
2. **Backpressure Proporcional y Bloqueante:**
   El `SnapshotRing` expone `AcquireBlocking(should_stop)`, el cual utiliza spin adaptativo con `std::this_thread::yield()` en caso de saturación extrema de la cola, garantizando que ninguna ranura se sobrescriba hasta que el hilo GPU haya invocado explícitamente `Release()`.
3. **Erradicación de Drains Forzados:**
   Se prohibió el uso de `Drain()` en rutas intermedias de despacho PM4. `CommitPublished()` se invoca de manera unitaria y en orden secuencial estrictamente cuando el hilo GPU consume cada draw individual.
4. **Validación Defensiva en `CommitPublished`:**
   Se incorporó verificación de límites (`position < cur_head || position >= cur_tail || slot.position != position`) en `drawPrep.cpp` para detectar y rechazar de forma segura cualquier ranura desincronizada.

---

## 4. Resumen de Cambios en el Código

| Archivo | Componente | Descripción de la Modificación |
|---|---|---|
| `src/graphics/guest_gpu/command_processor/cpSequencer.h` | `SnapshotRing` / `Sequencer` | Se agregó método `AcquireBlocking(should_stop)`, método `Reset()`, y método delegador en `Sequencer`. |
| `src/graphics/guest_gpu/command_processor/cpSequencer.cpp` | `KYTY_CP_SEQ_SNAPSHOTS` | Se incrementó la capacidad de snapshots de 64 a 256 ranuras para absorber ráfagas masivas de draws en Astro Bot. |
| `src/graphics/guest_gpu/command_processor/commandProcessor.h` | `IndirectDrawRegisters` | Se inicializaron por defecto los registros indirectos (`vertex_offset`, `instance_offset`, `index_offset`, `draw_index`) en `Pm4::SH_NOP` (`0x280`). |
| `src/graphics/guest_gpu/command_processor/cpOps.h` | `struct DrawIndirectOp` | Se añadieron los campos de offsets de registros indirectos serializados con valor por defecto `0x280u`. |
| `src/graphics/guest_gpu/graphicsRun.cpp` | `ResolveSubmission` | Se eliminaron las 12 llamadas destructivas a `m_draw_prep->Drain()`. Se implementó el binding y liberación de snapshots para `DrawIndex`, `DrawAuto`, `DrawIndirectMulti`, `DispatchDirect` y `DispatchIndirect`. Se conectaron los registros de indirect draws en la emisión y consumo. |
| `src/graphics/host_gpu/renderer/drawPrep/drawPrep.cpp` | `Engine::CommitPublished` | Se implementó validación de rango y verificación de coherencia de posición de ranura antes del commit. |
| `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp` | `PipelineCache::Lookup` | Se agregó la llamada obligatoria a `UpdateKeyInputCache(...)` cuando `inserted == true` (línea 1553). |
| `tests/CpSequencerTests.cpp` | Suite de Pruebas Unitarias | Se añadieron `TestCpSequencerWithoutDrawPrep` (Regresión B) y `TestPipelineCacheMruCoherency` (Regresión C). |
| `tests/DrawPrepTests.cpp` | Suite de Pruebas Unitarias | Se añadieron `TestDrawPrepSaturationAndSnapshotIntegrity` (Regresión A) y `TestAdversarialInterleavedConcurrency` (Regresión D). |

---

## 5. Resultados de la Suite de Tests de Regresión

### Ejecución de `cp_sequencer_tests.exe`:
```text
Running TestCpSequencerWithoutDrawPrep (Regression Test B)...
Running TestPipelineCacheMruCoherency (Regression Test C)...
cp sequencer tests passed
Resultado: PASS (100%)
```

### Ejecución de `draw_prep_tests.exe`:
```text
=== Starting DrawPrep Unit Tests ===
Running TestModeToggle...
Running TestSlotAllocationAndWrap...
Running TestParallelPreparation...
Running TestSnapshotImmutability...
Running TestTargetValidationAndFallback...
Running TestDrainFlushesAll...
Running TestConcurrentRegisterMutationDataRace (Adversarial)...
Running TestSlotExhaustionUnderBurst (Adversarial Burst 256)...
Running TestConcurrentDrainDuringInFlightWork (Adversarial)...
Running TestTargetSignatureMismatchFallback (Adversarial)...
Running TestParallelTopologyAndProgramOffload...
Running TestOpsDecouplingSerialization...
Running TestDrawPrepEndToEndWorkOffload...
Running TestComputeDrawInterleavedBarrierSafety...
Running TestDrawPrepWaitSpinVsCvBounded...
Running TestQueueDepthAndCapacityTracking...
Running TestDrawPrepSaturationAndSnapshotIntegrity (Regression Test A)...
Running TestAdversarialInterleavedConcurrency (Regression Test D)...
=== All DrawPrep Tests PASSED ===
Resultado: PASS (100%)
```

### Ejecución de `ctest`:
```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|stream_buffer_ring|pipeline_cache|cadence|cp_sequencer|draw_prep)"
```
```text
    Start 12: audit_profiler
1/4 Test #12: audit_profiler ...................   Passed    0.08 sec
    Start 13: cp_sequencer
2/4 Test #13: cp_sequencer .....................   Passed    0.02 sec
    Start 14: draw_prep
3/4 Test #14: draw_prep ........................   Passed    0.01 sec
    Start 84: stream_buffer_ring
4/4 Test #84: stream_buffer_ring ...............   Passed    0.42 sec

100% tests passed out of 4 (Total: 0.56 sec)
```

---

## 6. Resultados de Validación en Astro Bot (PPSA21567)

Se realizaron pruebas de validación directa sobre la build de Astro Bot (`PPSA21567`) en los tres perfiles analíticos requeridos, capturando telemetría JSON y CSV completa en `_TestLogs/`:

### Comparativa de Métricas Medidas (Telemetría Directa JSON)

| Métrica de Rendimiento / Estabilidad | Perfil A (Baseline Control) | Perfil B (CpSequencer Solo) | Perfil C (CpSequencer + DrawPrep 4W) | Delta (Perfil C vs B) |
|---|:---:|:---:|:---:|:---:|
| **Flags de Concurrencia** | `--no-cp-sequencer`<br>`--no-draw-prep` | `--cp-sequencer`<br>`--no-draw-prep` | `--cp-sequencer`<br>`--draw-prep -w 4` | — |
| **Estado de Estabilidad** | **100% Estable (0 Errores)** | **100% Estable (0 Errores)** | **100% Estable (0 Errores)** | **Cero Fallos** |
| **Problema A (Memory Crash 1118)** | No Reproducido | No Reproducido | **RESUELTO (0 errores)** | Resuelto |
| **Problema B (Unknown Prim Type 0)** | No Aplica | **RESUELTO (0 errores)** | **RESUELTO (0 errores)** | Resuelto |
| **Problema C (Push Stages Assert)** | No Reproducido | No Reproducido | **RESUELTO (0 errores)** | Resuelto |
| **Vulkan Device Lost** | 0 | 0 | 0 | 0 |
| **VBlanks Totales (Ventana Medida)** | 94 | 523 | **701** | **+34.0%** |
| **Tasa de VBlanks (Hz)** | — | 23.58 Hz | **31.54 Hz** | **+33.8%** |
| **Sumisiones Vulkan Totales** | — | 533 | **711** | **+33.4%** |
| **Tasa de Sumisiones Vulkan (sub/s)**| — | 24.03 | **31.99** | **+33.1%** |
| **Muestras de Audio Procesadas** | — | 294,400 | **439,808** | **+49.4%** |
| **Tasa de Audio Efectiva (Hz)** | — | 13,270 Hz | **19,790 Hz** | **+49.1%** |
| **Latencia Write Tracking (Promedio)**| — | 6.0 µs | **4.8 µs** | **-20.0% (Mejora)** |
| **Latencia Write Tracking (Máxima)** | — | 10.0 µs | **6.8 µs** | **-32.0% (Mejora)** |

### Observaciones de la Telemetría:
- **Eficacia de DrawPrep:** La activación de 4 workers en `DrawPrep` junto con `CpSequencer` no solo demostró ser 100% estable, sino que aceleró de forma drástica el despacho del motor de comandos, incrementando la tasa de sumisión Vulkan de 24.03 a 31.99 sumisiones por segundo y reduciendo la latencia de write tracking de 6.0 µs a 4.8 µs.
- **Flujo de Audio:** El subsistema de audio reflejó de inmediato el desahogo del hilo principal de CPU, incrementando la tasa de muestras generadas de 13.27 kHz a 19.79 kHz.
- **Ausencia Total de Fugas o Corrupciones:** Durante toda la ventana de carga de assets y compilación obligatoria de shaders (1050 shaders y 545 pipelines), no se produjo ninguna excepción ni violación de coherencia.

---

## 7. Fidelidad Gráfica y Cumplimiento Normativo

En cumplimiento estricto con las directrices de `AGENTS.md`:
1. **Preservación Total de Gráficos:** No se desactivó la iluminación global, el renderizado de sombras, el trazado de rayos por software, la simulación de fluidos ni ningún elemento visual para inflar el desempeño.
2. **Prohibición de Falseo de FPS (Regla 1):** Ningún reloj guest (`guest_tsc`), frecuencia virtual de VBlank, lógica de flip queues ni temporizador de audio fue alterado.
3. **Distinción entre Medido y Estimado (Regla 2):** Todos los valores reportados en la tabla comparativa corresponden a telemetría real exportada en `_TestLogs/*.json` y `_TestLogs/*.csv`.
4. **Flags Toggleables (Regla 3):** Tanto `--cp-sequencer` / `--no-cp-sequencer` como `--draw-prep` / `--no-draw-prep` continúan siendo 100% seleccionables por línea de comandos.

---

## 8. Conclusiones y Próximos Pasos

La Etapa 8.4F ha dejado a `CpSequencer` y `DrawPrep` plenamente operativos y listos para producción. Con los problemas de concurrencia y memory safety resueltos de raíz:

1. **Recomendación para `main`:**
   El código en la rama `fix/etapa8_4f_cpsequencer_drawprep` está completamente validado y listo para ser revisado por el desarrollador principal para su eventual merge en `main`.
2. **Siguiente Fase de Rendimiento (Camino a los 60 FPS):**
   Con una base de concurrencia sólida que no colapsa bajo estrés:
   - Profundizar en la reducción de barreras de sincronización redundantes en el render pass.
   - Optimizar el empaquetado de command buffers secundarios generados por los workers de `DrawPrep`.
   - Evaluar la transición de flags por defecto una vez completadas las pruebas de estabilidad extendidas en gameplay.
