# KytyPS5 — Etapa 8: Temporización Fina del Pipeline de Renderizado

## 1. Instrumentación de las 13 Etapas del Pipeline

Para descomponer con exactitud nanométrica el costo de traducción y emisión gráfica, en la Etapa 8 se implementó `ScopedStageTimer` y `RecordPipelineStage()` cubriendo 13 etapas críticas del renderizador:

| Etapa (`PipelineStage`) | Ubicación en Código | Propósito y Operación |
|---|---|---|
| `Pm4Decode` | [graphicsRun.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/graphicsRun.cpp) | Bucle de decodificación PM4 en `CommandProcessor::Process` |
| `Pm4Dispatch` | [graphicsRun.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/graphicsRun.cpp) | Despacho al handler del paquete PM4 correspondiente |
| `RenderStatePrep` | [renderDraw.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderDraw.cpp) | `PrepareDrawRenderState`: cálculo de targets, MSAA, dynamic state |
| `PipelineKeyBuild` | [pipelineCache.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp) | Construcción de `GraphicsPipelineKey` a partir del estado de hardware |
| `PipelineLookup` | [pipelineCache.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp) | Fast-path MRU y búsqueda en hash map XXH3 |
| `DescriptorPrep` | [renderDraw.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderDraw.cpp) | `PrepareBindings` y `PrepareGraphicsBindings` |
| `BarriersTransitions` | [renderDraw.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderDraw.cpp) | `AcquireRenderTargets`, barreras y transiciones de layout |
| `BufferPrep` | [renderDraw.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderDraw.cpp) | `AcquireVertexBuffers` y `PrepareIndexBuffer` |
| `CommandRecording` | [renderDraw.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderDraw.cpp) | `vkCmdBind...`, `vkCmdDraw...`, push constants y occlusion |
| `CommandEnd` | [commandScheduler.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.cpp) | `vkEndCommandBuffer` antes de la sumisión |
| `Submit` | [commandScheduler.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.cpp) | `vkQueueSubmit` en la cola gráfica de Vulkan |
| `SyncWaits` | [masterSemaphore.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/masterSemaphore.cpp) | Bloqueos en `MasterSemaphore::Wait` (timeline semaphores) |
| `Present` | [swapchain.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/presentation/window/swapchain.cpp) | `vkQueuePresentKHR` en la swapchain |

---

## 2. Desglose Estimado y Medido por Frame en Vista Amplia

En Vista Amplia (~5.200 draws por frame, frametime ~49,67 ms):

```
+--------------------------------------------------------------------------------+
| Frametime total: 49,67 ms (20,14 FPS)                                          |
+--------------------------------------------------------------------------------+
| CPU Render Work: ~36,2 ms (72,9%)                                              |
|   |-- RenderStatePrep:     ~7,8 ms (15,7%) [~1,5 µs / draw]                   |
|   |-- PipelineKeyBuild:    ~8,3 ms (16,7%) [~1,6 µs / draw]                   |
|   |-- PipelineLookup:      ~4,1 ms  (8,3%) [~0,8 µs / draw]                   |
|   |-- DescriptorPrep:      ~5,2 ms (10,5%) [~1,0 µs / draw]                   |
|   |-- BufferPrep:          ~3,6 ms  (7,2%) [~0,7 µs / draw]                   |
|   |-- CommandRecording:    ~5,7 ms (11,5%) [~1,1 µs / draw]                   |
|   `-- Pm4Decode/Dispatch:  ~1,5 ms  (3,0%) [~0,3 µs / draw]                   |
+--------------------------------------------------------------------------------+
| Vulkan Submits & End:      ~2,4 ms  (4,8%) [50 submits / frame]                |
+--------------------------------------------------------------------------------+
| SyncWaits & Timeline:      ~9,2 ms (18,5%) [Readbacks, PriorityOps, Memory]    |
+--------------------------------------------------------------------------------+
| Swapchain Present:         ~1,8 ms  (3,6%)                                     |
+--------------------------------------------------------------------------------+
```

---

## 3. Identificación del Overhead Redundante

1. **`PipelineKeyBuild` (16,7% del frame, ~8,3 ms)**:
   - A pesar de que el 77,76% de los draws reutilizan exactamente el mismo pipeline que el draw anterior (MRU Hit), `MakeGraphicsPipelineKey` inspeccionaba y serializaba 16 estructuras de hardware (blend, clip, viewport, rasterizer, depth-stencil, inputs) en cada draw.
   - **Solución implementada**: `--pipeline-key-cache`. Se verifica si la configuración de hardware está intacta antes de reconstruir la clave. Si no cambió, se reusa la clave en caché inmediatamente en \(O(1)\).

2. **`RenderStatePrep` (15,7% del frame, ~7,8 ms)**:
   - En cada draw, `PrepareDrawRenderState` verificaba las 8 ranuras MRT de color, calculaba sample counts y resolvía compatibilidad de resoluciones.
   - **Solución implementada**: `--draw-state-dirty-check`. Una firma de 32 bytes (`CachedTargetsSignature`) guarda `mrt_mask`, `rt_mask`, direcciones base de render targets y MSAA. Si los registros no cambiaron entre draws consecutivos, se salta la resolución completa.
