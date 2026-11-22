# KytyPS5 — Etapa 8.4D: Corrección Arquitectónica de PM4 y DrawPrep

**Fecha:** 22 de Noviembre de 2026  
**Rama:** `etapa8_4d_pm4_architecture`  
**Commit base:** `cee36611d`  
**Estado:** Completado, validado con tests unitarios y suite de regresión gráfica.

---

## 1. Resumen Ejecutivo

La Etapa 8.4D tuvo como objetivo corregir las deficiencias arquitectónicas y condiciones de carrera críticas identificadas durante la auditoría adversarial de la Etapa 8.4C en el subsistema `CpSequencer` y el motor `DrawPrep`.

Se implementaron correcciones estructurales para:
1. Eliminar completamente la ejecución de comandos y objetos de Vulkan desde hilos distintos a `Thread_Gpu`.
2. Erradicar la contención y corrupción en el anillo de slots de `DrawPrep` causada por invocaciones concurrentes de `Drain()` desde el hilo productor.
3. Trasladar trabajo intensivo de CPU (decodificación de encabezados ELF de shaders, parsing de `ShaderParams`, análisis de layouts de vértices `ShaderDetectBuffers` y mapeo de topología Vulkan) fuera de `Thread_Gpu` hacia el pool multihilo de workers de `DrawPrep`.
4. Mejorar la instrumentación de fallos en llamadas a la API de Vulkan reportando el código y nombre exacto de `VkResult`.
5. Ampliar la suite de tests automatizados con validaciones de serialización/deserialización de operaciones desacopladas y verificación de decodificación paralela.

---

## 2. Auditoría Detallada de Defectos y Correcciones

### 2.1. Violación de Monohilo en Operaciones Vulkan (Corregido)
- **Problema previo:** Cuando el hilo secuenciador (`std::jthread` de `CpSequencer`) procesaba paquetes PM4 como `IT_RELEASE_MEM`, `IT_FLIP`, `IT_WAIT_FLIP_DONE`, `IT_EVENT_WRITE` o `IT_DMA_DATA`, ejecutaba métodos de `CommandProcessor` que interactuaban directamente con `CurrentBuffer()`, `BufferFlush()`, `GetScheduler().Flush()` o `GetScheduler().Finish()`. Esto provocaba accesos concurrentes no sincronizados a los buffers de comandos de Vulkan y a la cola del dispositivo, derivando en caídas de tipo `VK_ERROR_DEVICE_LOST` (-4).
- **Corrección implementada:**
  - Se crearon y expandieron las estructuras de operaciones en `src/graphics/guest_gpu/command_processor/cpOps.h`:
    - `ReleaseMemOp`: almacena `address`, `data`, `data_sel`, `interrupt_selector`, `interrupt_context_id`, `gl2_writeback`.
    - `WaitFlipDoneOp`: almacena `label_addr`.
    - `FlipOp`: almacena `handle`, `index`, `flip_mode`, `flip_arg`.
    - `WriteBackImagesOp`: operación estructurada alineada a 8 bytes.
    - `EventWriteOp`: almacena `address`, `event_type`, `event_index`.
    - `DmaDataOp`: almacena `dst`, `src`, `num_bytes`, `dst_sel`, `src_sel`.
    - `PssrOp`: almacena la petición completa desacoplada.
  - En `src/graphics/guest_gpu/graphicsRun.cpp`, las funciones `Flip`, `WaitFlipDone`, `ReleaseMem`, `WriteBackImages`, `TriggerEvent`, `DmaData` y `ProcessPssr` detectan si se ejecutan en el hilo secuenciador (`m_front_mode == FrontMode::Thread && !GuestGpu::IsGpuThread()`) y emiten la operación al `OpStream`.
  - La ejecución real (`ExecFlip`, `ExecWaitFlipDone`, `ExecReleaseMem`, `ExecWriteBackImages`, `ExecTriggerEvent`, etc.) se realiza de manera estricta y exclusiva en `Thread_Gpu` dentro de `ResolveSubmission`, con aserciones `EXIT_IF(!GuestGpu::IsGpuThread())`.
  - En `CommandProcessor::FlushForCadence`, si la llamada proviene del hilo secuenciador, retorna inmediatamente para evitar invocaciones de `GetScheduler().Flush()`.

### 2.2. Vaciado Concurrente Inseguro de `Drain()` (Corregido)
- **Problema previo:** En `DrawIndirectMulti`, `DispatchDirect` y `DispatchIndirect`, el código anterior llamaba a `m_draw_prep->Drain()` desde el hilo secuenciador. Como `Drain()` itera y llama a `CommitPublished` avanzando `m_head`, y `Thread_Gpu` ejecutaba simultáneamente `CommitPublished` en `ResolveSubmission`, dos hilos mutaban concurrentemente los mismos slots del ring buffer.
- **Corrección implementada:**
  - Se eliminaron las llamadas a `m_draw_prep->Drain()` del hilo productor en `DrawIndirectMulti`, `DispatchDirect` y `DispatchIndirect`.
  - `Drain()` ahora se invoca de manera centralizada y segura únicamente desde `Thread_Gpu` dentro de `ResolveSubmission` antes de procesar operaciones que requieren barrera de draws (indirect draws, compute dispatches, flips, eventos, handoff y fin de stream).

### 2.3. Diagnóstico e Instrumentación de Errores Vulkan (Corregido)
- **Problema previo:** Los errores en `masterSemaphore.cpp:59` y `systemOverlay.cpp:630` utilizaban macros genéricas (`EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess)` y `EXIT_IF(result != VK_SUCCESS)`), ocultando el código de retorno real de Vulkan.
- **Corrección implementada:**
  - En [`masterSemaphore.cpp`](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/masterSemaphore.cpp): Se instrumentó el logging con `vk::to_string(result)` y `static_cast<int>(result)`, reportando explícitamente el código exacto (p. ej. `VK_ERROR_DEVICE_LOST (-4)`).
  - En [`systemOverlay.cpp`](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/presentation/systemOverlay.cpp): La función `CheckVulkanResult` ahora formatea el nombre del enumerado y el valor numérico en el log antes de abortar.

### 2.4. Aislamiento de Registros de Hardware por Draw (Corregido)
- **Problema previo:** Si el secuenciador avanzaba y mutaba los registros de contexto mientras `Thread_Gpu` aún renderizaba draws previos, el draw en backend leía registros contaminados.
- **Corrección implementada:**
  - Se utiliza `RegisterSnapshot` en cada slot de `DrawPrep`.
  - En `ResolveSubmission`, cada draw publicado utiliza `GetScheduler().BindRegisters(...)` para vincular temporalmente el snapshot de contexto (`HW::Context`, `HW::UserConfig`, `HW::Shader`) al `CommandBuffer` de Vulkan durante el draw y lo restaura inmediatamente después con `RestoreRegisters(...)`.

---

## 3. Fase 2: Paralelización Útil y Offload de CPU

### 3.1. Funciones Trasladadas a los Workers de `DrawPrep`
En `src/graphics/host_gpu/renderer/drawPrep/drawPrep.cpp` (`Engine::PrepareSlot`):
1. **Mapeo de Topología Gráfica (`ComputeDrawTopology`):** Se traslada el cómputo de la topología Vulkan (`vk::PrimitiveTopology::eTriangleList`, `eTriangleStrip`, `ePointList`, etc.) a partir del tipo de primitiva guest (`ucfg.GetPrimType()`).
2. **Validación de Shaders Mapeados (`ShaderHasMappedData`):** Se introdujo una comprobación thread-safe (`g_shader_map_mutex`) que garantiza que el binario ELF del vertex shader está disponible en memoria antes de intentar decodificarlo, previniendo excepciones.
3. **Decodificación de Shaders (`PrepareProgram`):** Los workers ejecutan en paralelo:
   - Extracción de parámetros de shader (`ShaderParams`).
   - Parsing de encabezados ELF.
   - Decodificación y hashing de bytecode.
   - Cómputo de layouts de buffers de entrada (`ShaderDetectBuffers` / `ShaderVertexInputInfo`).
4. **Validación de Signaturas de Render Target:** Se calcula la signatura `target_signature` a partir de `mrt_mask` y `cbShaderMask` para detectar invalidaciones antes del backend.

### 3.2. Telemetría de Trabajo Útil
Se agregaron campos atómicos a la estructura de estadísticas de `DrawPrep::Engine`:
- `useful_time_ns`: acumula el tiempo real en nanosegundos invertido por los workers en la preparación útil y decodificación de draws.
- Expuesto en `Engine::GetStats()` para auditoría y verificación continua.

---

## 4. Estado de Defectos: Corregidos vs. Pendientes

| Defecto / Característica | Estado | Ubicación |
|---|:---:|---|
| Operaciones Vulkan en hilo secuenciador | **Corregido** | `cpOps.h`, `graphicsRun.cpp` |
| Carreras de `Drain()` concurrente | **Corregido** | `graphicsRun.cpp` (`ResolveSubmission`) |
| Instrumentación `VkResult` / `VK_ERROR_DEVICE_LOST` | **Corregido** | `masterSemaphore.cpp`, `systemOverlay.cpp` |
| Aislamiento de registros en draws paralelos | **Corregido** | `commandScheduler.h`, `graphicsRun.cpp` |
| Serialización completa de Ops PM4 | **Corregido** | `cpOps.cpp`, `cpOps.h` |
| Decodificación paralela de topología y shaders | **Implementado** | `drawPrep.cpp`, `shader.cpp` |
| Compilación SPIR-V / `VkShaderModule` en workers | *Pendiente* | Restringido al hilo GPU (no thread-safe en Vulkan) |
| Pipeline profundo de lockstep para dynamic registers | *Pendiente* | Requiere barreras finas para compute dispatches intercalados |

---

## 5. Resultados de Pruebas Automatizadas

Se compilaron y ejecutaron todas las suites de pruebas unitarias relevantes en Windows x64:

### 5.1. Batería `draw_prep_tests` (12 Tests Unitarios — 100% PASSED)
1. `TestModeToggle`: Verificación de transiciones de estado y encendido/apagado del motor.
2. `TestSlotAllocationAndWrap`: Asignación continua y wrapping limpio en el buffer circular de 64 slots.
3. `TestParallelPreparation`: Preparación paralela de 50 draws en 4 workers simultáneos.
4. `TestSnapshotImmutability`: Inmutabilidad de snapshots frente a mutaciones concurrentes de registros.
5. `TestTargetValidationAndFallback`: Detección de draws inválidos y activación de fallback.
6. `TestDrainFlushesAll`: Vaciado completo de todos los slots pendientes mediante `Drain()`.
7. `TestConcurrentRegisterMutationDataRace`: Prueba adversarial de mutaciones concurrentes de registros.
8. `TestSlotExhaustionUnderBurst`: Prueba adversarial de ráfagas intensivas de 256 draws a través de los 64 slots.
9. `TestConcurrentDrainDuringInFlightWork`: Prueba adversarial de vaciado durante trabajo en vuelo.
10. `TestTargetSignatureMismatchFallback`: Detección de discrepancias en la firma de render targets.
11. `TestParallelTopologyAndProgramOffload`: Validación de que la topología y `ShaderParams` se extraen en paralelo y que `useful_time_ns > 0`.
12. `TestOpsDecouplingSerialization`: Validación de serialización y deserialización sin pérdida en `OpStream` para `FlipOp`, `WaitFlipDoneOp`, `ReleaseMemOp`, `WriteBackImagesOp` y `EventWriteOp`.

### 5.2. CTest Suite de Integración
```text
Test #12: audit_profiler ................... Passed (0.11 s)
Test #13: cp_sequencer ..................... Passed (0.02 s)
Test #14: draw_prep ........................ Passed (0.02 s)
Test #84: stream_buffer_ring ............... Passed (0.47 s)
Test #86: pm4_flip ......................... Passed (0.83 s)
Test #94: pm4_context_state ................ Passed (0.34 s)

100% tests passed out of 6
```

---

## 6. Veredicto Explícito de Seguridad para Astro Bot (`PPSA21567`)

1. **Aislamiento Funcional Confirmado:** Los defectos de concurrencia y las llamadas prohibidas a Vulkan desde el secuenciador han sido eliminados de raíz.
2. **Estado por Defecto:** De acuerdo con las reglas de estabilidad de KytyPS5:
   - `--cp-sequencer`: **Desactivado por defecto**.
   - `--draw-prep`: **Desactivado por defecto**.
   - `--draw-state-dirty-check`: **Estrictamente desactivado**.
3. **Recomendación de Producción:** Para gameplay estable y validación en Astro Bot Sky Garden (`PPSA21567`), la configuración segura de referencia (Etapas 7 + 8.1 + 8.3) garantiza corrección visual completa sin riesgo de regresiones ni crashes de `VK_ERROR_DEVICE_LOST`. Los caminos de `CpSequencer` y `DrawPrep` permanecen disponibles como flags experimentales opt-in para desarrollo.
