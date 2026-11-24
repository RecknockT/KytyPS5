# Backlog Dinámico de Optimizaciones — KytyPS5

| ID | Área | Descripción | Costo actual objetivo | Riesgo gráfico | Estado |
|---|---|---|---|---|---|
| OPT-01 | RenderStatePrep | Caché granular y exacta de `ResolveRenderColorTarget` / `ResolveRenderDepthTarget` con invalidación por registros HW y generación de `TextureCache` | 21,65 ms/frame | Bajo-Medio | **En curso (Iteración 1)** |
| OPT-02 | DescriptorPrep | Reducción de asignaciones por draw en `PrepareBindings` y optimización de resolución de texturas/samplers (`--binding-reuse` / caché granular) | 13,80 ms/frame | Bajo | Pendiente |
| OPT-03 | CpSequencer / DrawPrep | Ajustar `SnapshotCount()` (64 -> 256) para eliminar `snapshot-backpressure` y trasladar más preparación a workers | 10,45 ms/frame (Pm4Dispatch) | Bajo-Medio | Pendiente |
| OPT-04 | GPU Timestamps | Cablear `RecordGpuExecutionTime` con `vkCmdWriteTimestamp` para medir tiempo GPU físico real | N/A (observabilidad) | Nulo | Pendiente |
| OPT-05 | PriorityOperation / BufferReadback | Reducir esperas síncronas de `BufferCache::ReadMemory` (23,2 priority waits/frame, 8,3 readbacks/frame) | 28,14 ms acumulados | Medio | Pendiente |
