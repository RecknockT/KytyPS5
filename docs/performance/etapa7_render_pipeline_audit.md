# KytyPS5 — Auditoría del Pipeline de Renderizado y Flujo PM4 → Vulkan

## 1. Resumen Ejecutivo

Esta auditoría describe el camino de ejecución completo que sigue un comando gráfico en KytyPS5, desde su origen como paquete PM4 emitido por el juego Astro Bot (`PPSA21567`) en la cola guest, hasta su grabación y despacho en la API Vulkan del host.

La auditoría se basa en los datos empíricos de la Etapa 6.1:
- **Vista Amplia:** 20,57 FPS, 48,68 ms frametime, 5.031,81 draws/frame, 234,62 dispatches/frame, 50,05 submits/frame.
- **Vista al Suelo:** 52,68 FPS, 18,99 ms frametime, 1.200,57 draws/frame, 110,01 dispatches/frame, 18,33 submits/frame.

---

## 2. Arquitectura del Flujo Gráfico

```
[Hilo Guest / Juego]
       │
       ▼  Escribe paquetes PM4 en memoria mapeada
[Guest Ring Buffer / Command List]
       │
       ▼  SubmissionType::Commands
[GuestGpu::Run() — Hilo de GPU de Kyty]
       │
       ├─► Common::DebugCounters::ScopedTimer (GpuThreadBusyNs)
       │
       ├─► CommandProcessor::ProcessPm4()
       │        │
       │        ├─► Decodificación de cabeceras PM4 (g_cp_op_func[opcode])
       │        │
       │        ├─► Handlers (CpOpDrawIndex, CpOpDrawIndexAuto, etc.)
       │        │        │
       │        │        └─► CommandProcessor::DrawIndex()
       │        │                 │
       │        │                 ├─► CountDraw() -> Flush condicional si m_flush_draws > 0
       │        │                 │
       │        │                 └─► RenderExecutor::DrawIndex()
       │        │
       │        └─► FlushForCadence() (evalúa si transcurrió el intervalo de 1 ms)
       │
       ▼
[RenderExecutor — Hilo Host GPU]
       │
       ├─► Common::LockGuard lock(m_context.GetMutex())
       │
       ├─► Consumo de operaciones pendientes (MetadataColor, DepthStencilCopy, ResolveColorTargets)
       │
       ├─► PrepareDrawRenderState()
       │        ├─► ImageHeap::BeginDraw()
       │        ├─► PipelineCache::GetGraphicsPrograms() (resolución de módulos de shader)
       │        ├─► ResolveRenderColorTarget() (para cada slot de MRT)
       │        ├─► ResolveRenderDepthTarget()
       │        └─► PipelineCache::EnsureGraphicsPipeline() (evaluación asíncrona)
       │
       ├─► ExecutePreparedDraw()
       │        ├─► PrepareBindings() (para cada etapa de shader activa)
       │        ├─► PrepareGraphicsBindings()
       │        ├─► AcquireVertexBuffers() (preparación de buffers de vértices)
       │        ├─► PrepareIndexBuffer() (resolución o copia en StreamBuffer)
       │        ├─► PipelineCache::GetGraphicsPipeline() (construcción de clave y hash lookup)
       │        ├─► AcquireRenderTargets()
       │        ├─► CommitVertexBuffers() (vkCmdBindVertexBuffers2)
       │        ├─► CommitBindings() (actualización de descriptores / push descriptors)
       │        ├─► CommitIndexBuffer() (vkCmdBindIndexBuffer)
       │        ├─► PushConstants() (ObjectId o parámetros de Mesh Shader)
       │        ├─► UpdateGraphicsDynamicState() (cálculo de 16 viewports/scissors y estados dinámicos)
       │        ├─► DynamicState::Commit() (vkCmdSet*)
       │        ├─► CommandBuffer::BeginRendering() (si el render pass cambió)
       │        ├─► vkCmdBindPipeline() (vinculación del pipeline Vulkan)
       │        ├─► vkCmdDrawIndexed() / vkCmdDraw()
       │        └─► ResetBindings()
       │
       ▼
[CommandScheduler — Pacing y Submits]
       │
       ├─► SubmitReason::Cadence (pacing periódico cada 1 ms si hay trabajo grabado)
       ├─► SubmitReason::ExplicitFlush (al finalizar cada lista de comandos guest)
       └─► SubmitReason::Synchronization (forzado por BufferReadback o esperas sincrónicas)
```

---

## 3. Puntos Críticos y Operaciones Repetitivas por Draw

En una escena con 5.031 draws por frame, cada uno de los siguientes pasos se ejecuta 5.031 veces por fotograma:

1. **Adquisición del Mutex del Contexto (`m_context.GetMutex()`):**
   - Se toma y libera en cada llamada a `DrawIndex` / `DrawAuto`.
2. **Construcción y Búsqueda de Clave de Pipeline (`PipelineCache::GetGraphicsPipeline`):**
   - Construye `GraphicsPipelineKey` (~128 bytes).
   - Ejecuta `XXH3_64bits` sobre la clave.
   - Realiza búsqueda en `std::unordered_map`.
   - Aunque los shaders y el estado MRT no cambien entre draws consecutivos, este lookup se repite incondicionalmente.
3. **Llamadas a Vulkan de Vinculación de Estado:**
   - `vkCmdBindPipeline`: se invoca en cada draw, incluso si el pipeline activo es exactamente el mismo que en el draw anterior.
   - `vkCmdBindVertexBuffers2`: se invoca en cada draw si hay buffers de vértices, aun cuando los handles, offsets y rangos sean idénticos.
   - `vkCmdBindIndexBuffer`: se invoca en cada draw indexado, aun cuando el buffer de índices y el offset no hayan variado.
4. **Cálculo y Actualización de Estado Dinámico (`UpdateGraphicsDynamicState`):**
   - Recalcula los arrays de 16 viewports y 16 scissors con `calc_final_scissor`.
   - Verifica individualmente cada componente dinámico.
5. **Preparación y Comprobación de Descriptores (`CommitBindings`):**
   - Itera por todas las etapas activas y recursos de textura/buffer.
   - Realiza comprobaciones de transición de layout de imágenes (`image.Transit`).

---

## 4. Diagnóstico de la Carga de CPU

En la Vista Amplia:
- Frametime total: **48,68 ms**.
- Tiempo del hilo de GPU (`gpu_thread_busy_ms_per_frame`): **48,59 ms** (99,8% de ocupación).
- Tiempo de espera en `BufferReadback`: **6,86 ms**.
- Tiempo restante del hilo: **~41,7 ms** dedicados exclusivamente al bucle de procesamiento PM4, preparación de draw states y llamadas a la API Vulkan.
- Costo promedio por draw: **~8,2 µs**.

Este costo de 8,2 µs por draw call se compone de la suma del procesamiento de comandos guest en host CPU y las transiciones del driver Vulkan. Reducir las operaciones redundantes por draw es la vía prioritaria para reducir el frametime en escenas complejas.
