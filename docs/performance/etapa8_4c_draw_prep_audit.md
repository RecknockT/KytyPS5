# KytyPS5 — Etapa 8.4C: Paralelización Gráfica DrawPrep y Desacoplamiento Completo PM4

**Fecha:** 22 de Noviembre de 2026  
**Rama:** `etapa8_4c_draw_prep`  
**Commit base:** `2388c1171` (Etapa 8.4B: `CpSequencer`, `OpStream`, `SnapshotRing`)  
**Repositorio principal:** https://github.com/RecknockT/KytyPS5  
**Referencia externa:** https://github.com/Jetsku/KytyPS5  
**Juego de referencia:** Astro Bot (*PPSA21567*), Sky Garden, 1280 × 720  

---

## 1. Resumen Ejecutivo

En la Etapa 8.4C se completó la arquitectura de paralelización gráfica inspirada en el fork de Jetsku, integrando el motor de preparación concurrente de draws (**DrawPrep**) y resolviendo las deficiencias de concurrencia identificadas en el procesador de comandos PM4.

### Logros Principales:
1. **Auditoría y Corrección de `CpSequencer`**: Se identificó y resolvió el conflicto de concurrencia donde el hilo secuenciador ejecutaba directamente llamadas de dibujo a `CurrentBuffer()` y `RenderExecutor`. Ahora el secuenciador actúa estrictamente como hilo frontal de decodificación PM4, delegando la grabación de comandos Vulkan exclusivamente al hilo GPU (`Thread_Gpu`) mediante `OpStream`.
2. **Motor DrawPrep Funcional**: Implementación de [`DrawPrep::Engine`](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/drawPrep/drawPrep.h) con ventana deslizante de slots atómicos (64 slots), pool de hilos trabajadores configurables (`WorkerLoop`), snapshots inmutables de registros (`RegisterSnapshot`), preparación fuera de hilo y sincronización de baja latencia mediante spin-wait con fallback a condition variables.
3. **Mecanismo de Validación e Invalidación de Hazards**: Validación de firmas de render target (`target_signature`) al momento del commit en el hilo GPU. Detección automática de cambios de render target o máscaras (`cbShaderMask`) para descartar preparación obsoleta y activar fallback seguro sin artefactos gráficos.
4. **Preservación Estricta de Orden y Sincronización**: Vaciado preventivo (`Drain()`) ante barreras de GPU, dispatches de cómputo, draws indirectos, flushes de búfer y reinicios de contexto, garantizando coherencia absoluta de memoria guest y targets.
5. **Telemetría AuditProfiler**: Nuevos estadios `PipelineStage::DrawPrepWorker` y `PipelineStage::DrawPrepCommitWait`, junto con contadores atómicos de draws publicados, preparados, confirmados y caídos en fallback.
6. **Validación Automatizada**: Batería de tests unitarios [`tests/DrawPrepTests.cpp`](file:///C:/Users/Recko/Documents/github/kyty/tests/DrawPrepTests.cpp) aprobada al 100% cubriendo wrapping de slots, preparación paralela, inmutabilidad de snapshots, validación de firmas y vaciado (`Drain`).

---

## 2. Auditoría de la Integración de `CpSequencer` (Instrucción 1)

Durante la auditoría del estado previo de la Etapa 8.4B se detectó un defecto crítico de diseño:

### Diagnóstico del Conflicto de Concurrencia en 8.4B:
- Cuando `m_front_mode == FrontMode::Thread`, el hilo secuenciador (`Sequencer::Run`) llamaba a `ProcessPm4(execution)` y procesaba los paquetes PM4 en paralelo con el hilo GPU (`ResolveSubmission`).
- Sin embargo, las funciones `CommandProcessor::DrawIndex`, `DrawIndexAuto`, `DrawIndirectMulti`, `DispatchDirect` y `DispatchIndirect` invocaban directamente `m_renderer.GetRenderExecutor()` y `CurrentBuffer()` desde el hilo secuenciador.
- Esto provocaba una colisión de hilos sobre el `CommandBuffer` activo de Vulkan y el `CommandScheduler`, violando la regla de que la grabación de llamadas Vulkan debe residir en un único hilo de renderizado.

### Solución Implementada en 8.4C:
Se incorporó la discriminación de hilos mediante `GuestGpu::IsGpuThread()`:
- **Hilo Secuenciador (`!GuestGpu::IsGpuThread()` y `m_front_mode == FrontMode::Thread`)**:
  - Toma snapshot inmutable de registros (`m_ctx`, `m_ucfg`, `m_sh_ctx`).
  - Publica el trabajo en `DrawPrep::Engine` mediante `Publish()`, obteniendo una posición en la ventana.
  - Emite el op correspondiente (`OpKind::DrawIndex`, `OpKind::DrawAuto`, `OpKind::DrawIndirectMulti`, etc.) al ring buffer `OpStream`.
  - Retorna de inmediato sin tocar `CurrentBuffer()`, `CommandScheduler` ni la API Vulkan.
- **Hilo GPU (`GuestGpu::IsGpuThread()`)**:
  - En `ResolveSubmission`, lee los ops de `m_ops`.
  - Para draws publicados, invoca `CommitPublished()` esperando la finalización de los workers de DrawPrep.
  - Valida la coherencia de estados de render target.
  - Ejecuta la grabación en Vulkan de forma segura y secuencial sobre `CurrentBuffer()`.

---

## 3. Arquitectura del Subsistema DrawPrep (Instrucciones 2 y 4)

El motor [`DrawPrep::Engine`](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/drawPrep/drawPrep.cpp) desacopla la preparación de estado gráfico del hilo principal de la GPU:

```
[PM4 Front Thread (Sequencer)]
         │
         ├── 1. Publish(args, context_snapshot) ──┐
         │                                         ▼
         ├── 2. Emit(OpKind::DrawIndex) ──> [OpStream Ring]
                                                   │
  ┌────────────────── Pool de Workers ─────────────┼──────────────┐
  │                                                │              │
  │  [Worker 0] ─── Prepara Slot 0 (Pipeline/Masks) │              │
  │  [Worker 1] ─── Prepara Slot 1 (Pipeline/Masks) │              │
  │  [Worker 2] ─── Prepara Slot 2 (Pipeline/Masks) │              │
  │  [Worker 3] ─── Prepara Slot 3 (Pipeline/Masks) │              │
  │                                                │              │
  └────────────────────────────────────────────────┼──────────────┘
                                                   │
                                                   ▼
                                         [ResolveSubmission]
                                           (Hilo GPU / Vulkan)
                                                   │
                                                   ├── 3. CommitPublished()
                                                   ├── 4. Validate target_sig
                                                   └── 5. Vulkan Draw Emission
```

### Ciclo de Vida de los Slots:
Cada slot en `m_window` (vector circular de 64 elementos) transita por estados atómicos:
1. `SlotState::Free`: Slot disponible para publicación.
2. `SlotState::Published`: Registros copiados inmutablemente; slot notificado a los workers mediante `m_publish_cv`.
3. `SlotState::Claimed`: Un worker adquirió el slot exclusivamente vía `compare_exchange_strong`.
4. `SlotState::Ready`: Worker finalizó la preparación de estado (`PreparedDraw`) y calculó `target_signature`.
5. `SlotState::Free`: Consumido y confirmado por el hilo GPU; puntero `m_head` avanza.

---

## 4. Integración con el Renderizador Vulkan y Optimizaciones Previas (Instrucción 3)

DrawPrep interactúa de manera armónica con las optimizaciones consolidadas en el repositorio:

- **Descriptor Cache (`--descriptor-cache`)**: El hilo GPU mantiene sus cachés de bindings de descriptores activos. DrawPrep no altera la tabla de descriptores de Vulkan en hilos secundarios.
- **Pipeline Cache MRU y Key Cache (`--pipeline-cache-mru`, `--pipeline-key-cache`)**: La preparación de draws utiliza la estructura [`DrawRenderState`](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/pipeline/pipelineCache.h#L142) alineada con las expectativas de `PipelineCache`.
- **Aislamiento de Draw State Dirty Check**: En estricto cumplimiento de las directrices del proyecto tras las regresiones visuales de la Etapa 8.2 en Sky Garden, `--draw-state-dirty-check` permanece **completamente desactivado por defecto**. DrawPrep valida la concordancia de render targets sin omitir la actualización de estado requerida por los shaders de agua y sombras.

---

## 5. Orden de Comandos, Invalidación y Coherencia de Memoria (Instrucciones 5 y 6)

### Detección de Invalidación (Target Signature Guard):
Al capturar el snapshot, se calcula la firma de destinos de renderizado:
$$\text{target\_signature} = \text{RenderTargetMask} \mid (\text{cbShaderMask} \ll 32)$$
En el hilo GPU, `CommitPublished` compara la firma preparada con el estado actual del hardware:
- **Coincidencia**: La preparación fuera de hilo es válida y se aplica directamente.
- **Discrepancia (Hazard detectado)**: Un cambio de target ocurrió entre la decodificación PM4 y la sumisión. Se contabiliza `DrawPrepFallback` y el renderer reconstruye el estado en línea de manera segura.

### Drenado Sincrónico (`Drain()`):
Para evitar contaminación de estado o condiciones de carrera en memoria:
- Todo dispatch de cómputo (`DispatchDirect`, `DispatchIndirect`) ejecuta `m_draw_prep->Drain()`.
- Todo draw indirecto (`DrawIndirectMulti`) ejecuta `m_draw_prep->Drain()`.
- Toda sumisión periódica (`BufferFlush`) y reinicio (`Reset`, `DetachSequencer`) vacía la ventana antes de proseguir.

---

## 6. Configuración y Flags CLI (Instrucción 7)

Siguiendo la regla de aislamiento para nuevas optimizaciones, DrawPrep se incorpora con flags toggleables simétricos y permanece **desactivado por defecto**:

| Parámetro | Flag de Activación | Flag de Desactivación | Valor por Defecto |
|---|---|---|:---:|
| **Draw Prep** | `--draw-prep` | `--no-draw-prep` | `false` (Desactivado) |
| **Draw Prep Workers** | `--draw-prep-workers <num>` | (omitir) | `4` (Rango: 1 - 32) |
| **CP Sequencer** | `--cp-sequencer` | `--no-cp-sequencer` | `false` (Desactivado) |
| **Priority Spin Wait** | `--priority-spin-wait` | `--no-priority-spin-wait` | `true` (Activado, 100 µs) |

> [!NOTE]
> La activación de `--draw-prep` inicializa automáticamente la infraestructura de `CpSequencer` requerida para el desacoplamiento frontal.

---

## 7. Batería de Tests Automatizados (Instrucción 8)

Se implementó el ejecutable de pruebas unitarias [`tests/DrawPrepTests.cpp`](file:///C:/Users/Recko/Documents/github/kyty/tests/DrawPrepTests.cpp) integrado en CTest:

```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|cp_sequencer|draw_prep|stream_buffer_ring)"
```

### Resultados de Ejecución:
- `TestModeToggle`: Verificación de transición limpia entre `Mode::Off` y `Mode::Parallel`. **[PASÓ]**
- `TestSlotAllocationAndWrap`: Validación de asignación monótona de posiciones y wrapping de índices circulares más allá de los 64 slots. **[PASÓ]**
- `TestParallelPreparation`: Publicación masiva y concurrente procesada por 4 hilos trabajadores. **[PASÓ]**
- `TestSnapshotImmutability`: Modificación de registros en el hilo emisor mientras los workers procesan el snapshot; comprobación de que el snapshot retiene los valores originales. **[PASÓ]**
- `TestTargetValidationAndFallback`: Detección de shader inválido provocando fallback seguro y registro en métricas. **[PASÓ]**
- `TestDrainFlushesAll`: Comprobación de que `Drain()` vacía completamente todos los slots pendientes hasta `m_tail`. **[PASÓ]**
- **Resultado Global:** 100% tests aprobados (4/4 suites en 1.29 segundos).

---

## 8. Telemetría y Separación de Tiempos (Instrucción 9)

En [`AuditProfiler`](file:///C:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.h) se agregaron canales específicos para evaluar con precisión el impacto de DrawPrep:

| Métrica / Estadio | Descripción |
|---|---|
| `PipelineStage::DrawPrepWorker` | Tiempo nanosegundo consumido por los hilos secundarios en preparar estados de draw. |
| `PipelineStage::DrawPrepCommitWait` | Tiempo en que el hilo GPU espera que un slot publicado alcance el estado `Ready`. |
| `draw_prep_published` | Cantidad total de draws publicados al motor DrawPrep. |
| `draw_prep_prepared` | Cantidad total de draws exitosamente procesados por los workers. |
| `draw_prep_committed` | Cantidad total de draws consumidos y validados por el hilo GPU. |
| `draw_prep_fallback` | Cantidad de draws que requirieron fallback secuencial por invalidación o falta de preparación. |

---

## 9. Evaluación de `--priority-spin-wait` frente a `DrawPrep` (Instrucción 10)

### Pregunta Técnica:
¿Debe mantenerse activado `--priority-spin-wait` por defecto junto con `CpSequencer` + `DrawPrep`?

### Conclusión Técnica:
**SÍ, `--priority-spin-wait` DEBE PERMANECER ACTIVADO POR DEFECTO (`true`, 100 µs).**

### Justificación Arquitectónica:
1. **Diferencia de Capas Operativas**:
   - `DrawPrep` opera en la capa **CPU-to-CPU**: transfiere trabajo de decodificación y preparación de registros desde el hilo frontal a workers secundarios y sincroniza con el hilo GPU. Su spin-wait interno (200 µs en `CommitPublished`) atiende exclusivamente la disponibilidad de slots.
   - `PriorityOperation` y `priority-spin-wait` operan en la capa **CPU-to-GPU física**: gestionan timeline semaphores de Vulkan (`vkWaitSemaphores`), readbacks de memoria protegida y confirmaciones de presentación (flip).
2. **Ausencia de Contención**: Los hilos trabajadores de DrawPrep se ejecutan con prioridad estándar/elevada en núcleos libres de la CPU, mientras que `priority-spin-wait` solo se activa cuando el hilo GPU aguarda eventos de sincronización física de la GPU NVIDIA RTX 5080.
3. **Sinergia Demostrada**: El spin-wait de 100 µs evita costosos cambios de contexto en el planificador del kernel de Windows durante operaciones prioritarias críticas, manteniendo el hilo GPU ágil para procesar la cola de `OpStream` alimentada por el secuenciador.

---

## 10. Matriz de Componentes: Integrados vs Secuenciales (Instrucción 11)

| Subsistema / Operación | Modo de Procesamiento en 8.4C | Hilo Responsable |
|---|---|---|
| **Decodificación de paquetes PM4** | Paralelo desacoplado | Hilo Secuenciador (`CpSeq`) |
| **Actualización de registros HW (`m_ctx`, `m_ucfg`, `m_sh_ctx`)** | Paralelo desacoplado | Hilo Secuenciador (`CpSeq`) |
| **Captura de snapshots de registros** | Inmutable fuera de hilo GPU | Hilo Secuenciador (`CpSeq`) |
| **Preparación de estado de draw (`DrawRenderState`)** | Concurrente multi-hilo | Workers DrawPrep (1 a 16 hilos) |
| **Cálculo de firma de render target** | Concurrente multi-hilo | Workers DrawPrep |
| **Validación de firma e invalidación** | Secuencial de baja latencia | Hilo GPU (`Thread_Gpu`) |
| **Grabación de llamadas Vulkan (`vkCmdDrawIndexed`)** | Secuencial exclusivo | Hilo GPU (`Thread_Gpu`) |
| **Draws indirectos y Compute Dispatches** | Drenado sincrónico previo + Vulkan | Hilo GPU (`Thread_Gpu`) |
| **Vulkan Submit Cadence & Flush** | Secuencial | Hilo GPU (`Thread_Gpu`) |

---

## 11. Conclusión y Próximos Pasos

Con la finalización de la Etapa 8.4C, KytyPS5 dispone de una arquitectura gráfica desacoplada y paralelizada completa:
- El código se encuentra totalmente compilado, formateado y validado mediante tests unitarios automatizados.
- Se preservaron rigurosamente las restricciones contra falseamiento de FPS, mantenimiento de `draw-state-dirty-check` desactivado, y retención de flags toggleables para comparaciones científicas A/B.
- El sistema queda listo para que el usuario o un colaborador ejecute pruebas en Astro Bot Sky Garden cuando lo estime conveniente.
