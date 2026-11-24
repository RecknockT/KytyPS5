# Recorte de memoria temporal del tiler — 2026-11-24

Se incorpora `--trim-scratch-memory` / `--no-trim-scratch-memory`, desactivado por defecto. La sesión manual iniciada con `stage8_live_play.ps1 -TrimScratchMemory` lo activa explícitamente.

Los dos buffers temporales se liberan después de cinco segundos sin uso. Si durante una ventana de cinco segundos la demanda cae a un cuarto o menos de un buffer de al menos 4 MiB, este se reduce, con un mínimo de 1 MiB. El buffer de entrada se marca como usado para proteger las operaciones alternadas. Se conserva la liberación diferida del scheduler: ningún recurso se destruye antes de terminar el trabajo GPU que lo utiliza. Las asignaciones dedicadas de Vulkan permiten devolver también la reserva del asignador.

La captura de auditoría registra el estado del flag, asignaciones, retiros y bytes efectivamente liberados. Los bytes pendientes de liberación no se cuentan como liberados.

## Validación

Compilación Release completada. Pasaron los siete tests seleccionados: scratch_memory_trim, compact_image_readback, audit_profiler, read_buffer_reuse, resource_prep_reuse, stream_buffer_ring y cadence_runtime.

La prueba Vulkan sintética comprobó contenido de ambas entradas después del recorte, liberación diferida, ausencia de reasignación en usos pequeños repetidos y crecimiento posterior. Con el flag apagado se conservaron 25.165.824 bytes; activado se devolvieron esos 24 MiB tanto de asignaciones como de bloques reservados de VMA. El contador acumulado de liberación fue 25 MiB porque incluye otro buffer de 1 MiB creado durante la reducción.

Resultados: `_Build/windows/scratch_trim_tests.log` y `_Build/windows/scratch_trim_memory_measurement.log`. Sesión de juego manual: `_Build/windows/live_play/20261124_130951_384`.

Esta medición es sintética. No demuestra una reducción equivalente de RAM/VRAM ni una mejora de FPS en Astro Bot. El cambio afecta exclusivamente a buffers temporales del tiler; no reduce la capacidad de memoria guest de 12 GiB ni otras reservas permanentes del emulador.
