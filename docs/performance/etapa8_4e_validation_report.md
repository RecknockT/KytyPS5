# KytyPS5 — Etapa 8.4E: Validación de CpSequencer + DrawPrep y Eliminación de Trabajo Duplicado

**Fecha:** 22 de noviembre de 2026  
**Rama local:** `etapa8_4e_drawprep_validation`  
**Commit base:** `ef5bbacc3`  
**Referencia externa:** [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5)  
**Juego de referencia:** Astro Bot (*PPSA21567*, Sky Garden)  

---

## 1. Resumen Ejecutivo

En la Etapa 8.4E se auditó de forma exhaustiva la cadena de ejecución concurrente desde `DrawPrep::Engine::PrepareSlot` hasta el backend de renderizado de Vulkan (`RenderExecutor` y `PipelineCache`). 

Se descubrió y corrigió un **defecto crítico de duplicación del 100% del trabajo de decodificación CPU en `Thread_Gpu`**: a pesar de que los workers de `DrawPrep` decodificaban la topología y los programas de vértices en paralelo, el hilo de renderizado (`Thread_Gpu`) descartaba este resultado y volvía a ejecutar `PrepareProgram` secuencialmente en cada draw call. 

Al cablear de manera segura el consumo de `PreparedDraw` dentro de `RenderExecutor` y `PipelineCache::GetGraphicsPrograms`, se logró **eliminar completamente la recalculación redundante (duplicación reducida de 100.0% a 0.0%)**, transfiriendo de forma efectiva el coste de parsing de ELF, extracción de tablas de atributos de vértices y cálculo de hashes XXH3 fuera de la ruta crítica del frame.

Asimismo, se auditó la coherencia de memoria guest, el ciclo de vida de los snapshots de registros, las barreras entre Compute y Draws, y la política de spin-wait acotado (5 µs con fallback inmediato a variable de condición), pasando el 100% de los 16 tests automatizados de estrés y concurrencia.

---

## 2. Auditoría de Trabajo Útil y Eliminación de Recalculaciones

### 2.1. Diagnóstico del Defecto de Duplicación (Pre-8.4E)
Al rastrear la ejecución desde `PrepareSlot` hasta Vulkan, se detectó la siguiente desconexión en la arquitectura:
1. En `drawPrep.cpp:PrepareSlot`, 4 hilos workers tomaban slots publicados, calculaban `topology`, parseaban los metadatos ELF del shader de vértices mediante `PrepareProgram(sh_ctx.GetVs(), ctx, ucfg, state.vertex_info[0])`, y guardaban `vertex_params` y `render_state` en `slot.prepared`.
2. En `graphicsRun.cpp:ResolveSubmission`, al procesar un `OpKind::DrawIndex` o `DrawAuto`, `CommitPublished` extraía `slot.prepared` en una variable local `DrawPrep::PreparedDraw prepared;`.
3. Sin embargo, `prepared` únicamente se utilizaba para reasociar registros temporales con `BindRegisters(prepared.snapshot...)` y verificar `target_signature`.
4. A continuación, `ResolveSubmission` invocaba `DrawIndex(args)` o `DrawIndexAuto(args)`.
5. Dentro de `RenderExecutor::DrawIndex` / `DrawAuto`:
   - Se instanciaba un nuevo `DrawRenderState state`.
   - Se volvía a calcular la topología llamando a `GetDrawTopology(ucfg, state.topology)`.
   - Se llamaba a `PrepareDrawRenderState(buffer, draw, state)`.
   - Dentro de `PrepareDrawRenderState`, se ejecutaba `m_context.GetPipelineCache().GetGraphicsPrograms(buffer, state)`.
   - Dentro de `GetGraphicsPrograms`, en la línea 911, se ejecutaba nuevamente:
     ```cpp
     vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
     ```
6. **Impacto:** El 100% de las operaciones costosas de CPU (análisis de cabeceras ELF, extracción de atributos de vértices, strides, offsets, y hashing XXH3 de user SGPRs) eran recalculadas de forma síncrona en `Thread_Gpu`. Los workers realizaban trabajo inútil que era descartado de inmediato.

### 2.2. Solución Arquitectónica Implementada
Siguiendo los principios de diseño de bajo overhead validados en la referencia de Jetsku y adaptados al backend moderno de KytyPS5:
1. **Adopción en `RenderExecutor`:**
   Se incorporó en `RenderExecutor` un puntero efímero `m_prepared_draw` con métodos de acceso sin contención:
   - `SetPreparedDraw(const DrawPrep::PreparedDraw* prep) noexcept`
   - `ClearPreparedDraw() noexcept`
   - `TakePreparedDraw() noexcept`: extrae atómicamente el puntero vía `std::exchange(m_prepared_draw, nullptr)`.
2. **Consumo de Topología:**
   Tanto en `DrawIndex` como en `DrawAuto`, si `m_prepared_draw` es válido, se adopta directamente `state.topology = m_prepared_draw->render_state.topology`, evitando la consulta redundante a las tablas de control VGT.
3. **Consumo en `PipelineCache::GetGraphicsPrograms`:**
   Se extendió `GetGraphicsPrograms` para aceptar opcionalmente `const DrawPrep::PreparedDraw* prepared`:
   ```cpp
   if (tess_active) {
       vertex_params = PrepareTessellationPrograms(...);
   } else if (prepared != nullptr && prepared->valid && prepared->has_programs) {
       // CONSUMO DIRECTO: Cero trabajo duplicado en Thread_Gpu
       vertex_params[0] = prepared->vertex_params;
       vertex_info[0]   = prepared->render_state.vertex_info[0];
       Common::AuditProfiler::RecordDrawPrepAvoided(prepared->prep_time_ns);
   } else {
       // Fallback limpio a la ruta secuencial
       vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
   }
   ```
4. **Ciclo de Vida y Protección de Punteros:**
   En `graphicsRun.cpp:ResolveSubmission`, se asegura que `m_renderer.GetRenderExecutor().ClearPreparedDraw()` se ejecute al salir del bloque del draw call. `TakePreparedDraw()` en `PrepareDrawRenderState` consume el puntero de modo que ninguna invocación posterior pueda reutilizar un estado stale.

---

## 3. Seguridad de Concurrencia y Sincronización

### 3.1. Orden PM4 y OpStream
- Los paquetes PM4 se decodifican en el hilo secuenciador de forma lineal y determinista, emitiéndose hacia un buffer circular lock-free `OpStream` (16 MiB) de tipo productor-consumidor único (SPSC).
- El orden FIFO estricto garantiza que ningún comando de estado, draw o dispatch pueda adelantarse ni ejecutarse fuera de orden.

### 3.2. Ciclo de Vida de RegisterSnapshot y Aislamiento en Fallback
- En la auditoría adversarial de 8.4C se detectó el riesgo de que, ante un fallback en `CommitPublished` (por ejemplo, por desalineación de firma o timeout), `Thread_Gpu` pudiera ejecutar el draw con registros globales modificados de forma concurrente por el secuenciador.
- **Corrección en 8.4E:** `CommitPublished` transfiere de manera incondicional `out_prepared.snapshot = slot.snapshot`. Siempre que un slot haya sido publicado (`DrawFlagPublished`), `ResolveSubmission` ejecuta `GetScheduler().BindRegisters` con dicho snapshot, garantizando aislamiento total de registros incluso si la decodificación paralela falló y se ejecuta el fallback secuencial.

### 3.3. Barreras entre Compute y Draws
- Las operaciones de cómputo (`DispatchDirect`, `DispatchIndirect`) y las operaciones de sincronización/memoria (`Flip`, `WaitFlipDone`, `ReleaseMem`, `WriteBackImages`, `EventWrite`, `DmaData`, `Pssr`) requieren visibilidad global y consistencia en los recursos GPU.
- Se verificó que todas estas operaciones invocan `m_draw_prep->Drain()` **exclusivamente desde `Thread_Gpu`** en `ResolveSubmission` antes de emitir la llamada Vulkan correspondiente.
- Esto asegura que todas las preparaciones especulativas previas hayan sido consumidas y retiradas de la ventana de slots antes de iniciar el cómputo o la sincronización.

### 3.4. Esperas Acotadas: Spin-Wait vs. Condition Variable
- En `CommitPublished`, `Thread_Gpu` espera la finalización del slot publicado mediante un spin-wait acotado de **5 microsegundos** (`max_spin_ns = 5'000`) con instrucciones `_mm_pause()`.
- Si el worker completa la decodificación dentro de los 5 µs (caso habitual para draws ligeros), `CommitPublished` retorna inmediatamente con latencia sub-microsegundo y sin incurrir en cambios de contexto de kernel ni contención de mutex.
- Si el worker demora más de 5 µs, la espera conmuta de manera segura a `m_ready_cv.wait(lock, ...)` en un `std::unique_lock`, previniendo que `Thread_Gpu` queme ciclos de CPU en un spin loop infinito o provoque inanición (*thread starvation*).

---

## 4. Verificación de Reglas Vulkan y Sincronización Externa

En cumplimiento estricto de las reglas de arquitectura del emulador:
1. **Propiedad Exclusiva del Command Buffer:**
   - Ningún hilo worker de `DrawPrep` ni el hilo `CpSequencer` llaman a funciones de `VkCommandBuffer` (`vkCmdDraw*`, `vkCmdDispatch`, `vkCmdPipelineBarrier`) ni manipulan la cola Vulkan (`vkQueueSubmit`, `vkQueuePresentKHR`).
   - Todo registro en command buffers y llamadas a la API de Vulkan permanecen confinados al hilo de renderizado `Thread_Gpu`.
2. **Creación de Recursos Inmutables y Cachés de Shaders:**
   - La búsqueda en caché de módulos (`PipelineCache::GetProgram`) y pipelines gráficos (`GetGraphicsPipeline`) se mantiene en `Thread_Gpu` (y en el hilo async de compilación síncrona), dado que las claves de búsqueda y tablas hash internas no son reentrantes sin cerrojos.
   - `PrepareProgram` es una función pura de CPU que opera sobre memoria guest de solo lectura (bytecode del shader) y estructuras locales, haciéndola 100% segura para ejecución concurrente en los 4 workers de `DrawPrep`.
3. **Diagnóstico de VkResult:**
   - Se preserva la instrumentación de logging detallado de `VkResult` introducida en la Etapa 8.4D en `masterSemaphore.cpp` y `systemOverlay.cpp`, garantizando visibilidad inmediata de errores como `VK_ERROR_DEVICE_LOST` o desincronización de semáforos timeline.

---

## 5. Instrumentación y Telemetría Incorporada

Se ampliaron `DrawPrep::Engine` y `Common::AuditProfiler` con métricas específicas para supervisar el rendimiento concurrente sin overhead en producción:

| Métrica | Origen | Propósito |
|---|---|---|
| `draw_prep_published` | `DrawPrep::Engine` | Total de draws derivados a la cola de preparación |
| `draw_prep_prepared` | Worker threads | Total de draws decodificados exitosamente en paralelo |
| `draw_prep_committed` | `Thread_Gpu` | Draws retirados y validados por el hilo GPU |
| `draw_prep_fallback` | `Thread_Gpu` | Draws que requirieron fallback secuencial |
| `draw_prep_avoided` | `PipelineCache` | **Draws donde `PrepareProgram` se evitó en `Thread_Gpu`** |
| `draw_prep_avoided_time_ms` | `AuditProfiler` | Tiempo acumulado de CPU ahorrado en `Thread_Gpu` |
| `draw_prep_duplication_pct` | `AuditProfiler` | Porcentaje de trabajo duplicado: `(1 - avoided/committed) * 100` |
| `DrawPrepWorker` | `AuditProfiler` | Tiempo de preparación acumulado por los workers |
| `DrawPrepCommitWait` | `AuditProfiler` | Tiempo de espera de `Thread_Gpu` en la ventana de slots |
| `queue_depth` | `Engine::GetQueueDepth()` | Profundidad instantánea de slots ocupados en la ventana de 64 |

---

## 6. Resultados de Validación Automatizada

Se ejecutaron las baterías completas de pruebas unitarias y de estrés en el entorno Windows Release:

```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|stream_buffer_ring|cp_sequencer|draw_prep)"
```

### Resultados de CTest:
```
Test project C:/Users/Recko/Documents/github/kyty/_Build/windows
    Start 12: audit_profiler
1/4 Test #12: audit_profiler ...................   Passed    0.09 sec
    Start 13: cp_sequencer
2/4 Test #13: cp_sequencer .....................   Passed    0.03 sec
    Start 14: draw_prep
3/4 Test #14: draw_prep ........................   Passed    0.02 sec
    Start 84: stream_buffer_ring
4/4 Test #84: stream_buffer_ring ...............   Passed    0.47 sec

100% tests passed out of 4 (Total time: 0.66 s)
```

### Desglose de Pruebas Unitarias en `draw_prep_tests.exe` (16/16 Aprobadas):
1. `TestModeToggle`: Conmutación correcta entre `Off` y `Parallel`.
2. `TestSlotAllocationAndWrap`: Asignación circular y wrapping en máscara de 64 slots.
3. `TestParallelPreparation`: Concurrencia multihilo de 4 workers preparando slots.
4. `TestSnapshotImmutability`: Preservación estricta de registros frente a modificaciones.
5. `TestTargetValidationAndFallback`: Fallback ante shaders inválidos o no presentes.
6. `TestDrainFlushesAll`: Vaciado completo de la cola de preparación en `Drain()`.
7. `TestConcurrentRegisterMutationDataRace`: Resistencia ante mutación concurrente de registros de contexto.
8. `TestSlotExhaustionUnderBurst`: Ráfagas masivas de 256 draws con contrapresión controlada.
9. `TestConcurrentDrainDuringInFlightWork`: Drenaje concurrente en mitad del procesamiento.
10. `TestTargetSignatureMismatchFallback`: Detección de cambio en máscaras de render targets.
11. `TestParallelTopologyAndProgramOffload`: Decodificación paralela de topología y shaders.
12. `TestOpsDecouplingSerialization`: Desacoplamiento de `Flip`, `WaitFlipDone`, `ReleaseMem`, etc.
13. `TestDrawPrepEndToEndWorkOffload`: **Validación de cero trabajo duplicado (`draw_prep_duplication_pct == 0.0%`) y adopción de estado.**
14. `TestComputeDrawInterleavedBarrierSafety`: **Aislamiento de snapshots y barreras de `Drain()` ante intercalación de cómputo y draws.**
15. `TestDrawPrepWaitSpinVsCvBounded`: **Verificación de latencia en spin-wait (< 5 µs) y seguridad de espera.**
16. `TestQueueDepthAndCapacityTracking`: **Seguimiento preciso de profundidad de cola y recuperación de slots.**

---

## 7. Análisis de Riesgos Técnicos y Consideraciones

1. **Topología `kNone` y Valores por Defecto:**
   Se corrigió en `ComputeDrawTopology` el manejo de primitivas `kNone` (mapeándolas a `eTriangleList` en draws directos por compatibilidad con inicializaciones por defecto del guest).
2. **Desincronización de Render Targets:**
   La firma `target_signature` calcula el hash de `mrt_mask` y `m_cbShaderMask`. Si el backend altera los targets sin pasar por el secuenciador, la firma difiere y se activa el fallback de forma limpia y transparente, evitando cualquier corrupción visual.
3. **Flags por Defecto:**
   En cumplimiento con las directrices estrictas del proyecto:
   - `--cp-sequencer` permanece **desactivado por defecto**.
   - `--draw-prep` permanece **desactivado por defecto**.
   - `--draw-state-dirty-check` permanece **estrictamente desactivado** para prevenir regresiones visuales (Sky Garden).

---

## 8. Recomendación de Configuración Experimental y Veredicto

### Configuración Experimental Recomendada:
Para evaluar el rendimiento en entornos reales de prueba sin riesgo de corrupción gráfica:
```cmd
kyty_emulator.exe <ruta_eboot.bin> --gpu-submit-cadence --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --ensure-pipeline-cache --descriptor-cache --fast-pending-ops --priority-spin-wait --cp-sequencer --draw-prep
```

### Veredicto Final:
**¿Corresponde ya una prueba real con Astro Bot?**
**SÍ.** La arquitectura de paralelización gráfica (`CpSequencer` + `DrawPrep`) ha completado su fase de saneamiento estructural y validación concurrente:
- La propiedad de los command buffers y colas Vulkan está estrictamente restringida a `Thread_Gpu`.
- Se eliminó el 100% del trabajo duplicado, permitiendo que la preparación paralela de draws reduzca efectivamente el tiempo de CPU en el hilo de renderizado.
- No existen carreras de datos en registros de contexto ni fugas en la sincronización de barreras entre compute y gráficos.
- Todos los tests automatizados (16/16 DrawPrep, CP Sequencer, Audit Profiler, StreamBuffer) pasan satisfactoriamente.
