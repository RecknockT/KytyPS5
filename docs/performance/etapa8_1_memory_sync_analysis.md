# KytyPS5 — Etapa 8.1: Análisis de Memoria, Sincronización y StreamBuffer

## 1. Introducción

El rendimiento de un emulador de consola moderna depende críticamente de la coherencia de memoria entre la CPU huésped y el procesador de comandos de la GPU. En PlayStation 5, la memoria está unificada (UMA), permitiendo que la CPU y la GPU lean y escriban directamente en las mismas direcciones físicas sin copias explícitas de PCIe.

En un host Windows x64 con arquitectura de memoria discreta (como el sistema de referencia con AMD Ryzen 7 9700X y NVIDIA GeForce RTX 5080), esta coherencia se implementa mediante:
1. **Mecanismo de Write Tracking**: Protección de páginas de memoria virtual (`PAGE_GUARD` o `PAGE_READONLY`) para detectar escrituras del guest.
2. **Búferes de Anillo Mapeables (`StreamBuffer`)**: Memoria mapeada en host para transferir vértices e índices generados por la CPU hacia la GPU.
3. **Semáforos de Línea de Tiempo Vulkan (`MasterSemaphore`)**: Sincronización asíncrona CPU/GPU con avance monotónico de ticks.

Este documento analiza el comportamiento de la memoria y sincronización durante la **transición de aterrizaje** y en **vista amplia** de Astro Bot (`PPSA21567`).

---

## 2. Telemetría de Memoria en Etapa 8

Durante las capturas de la Etapa 8 se obtuvieron las siguientes métricas de memoria:

| Métrica | Vista Amplia | Vista al Suelo (Normal 64 MiB) | Vista al Suelo (Extendido 256 MiB) |
|---|---:|---:|---:|
| **Total Page Faults** | 22 | 14 | 14 |
| **Total Reprotects** | 0 | 0 | 0 |
| **Avg Handler Time** | 23,10 µs | 24,15 µs | 24,02 µs |
| **Upload Bytes CPU->GPU** | 1.104.912 B (~1,05 MiB) | 398.240 B (~389 KiB) | 398.240 B (~389 KiB) |
| **Download Bytes GPU->CPU**| 0 B | 0 B | 0 B |
| **Texture Partial Refreshes** | 0 | 0 | 0 |
| **StreamBuffer Allocations** | 4.887 / frame | 1.189 / frame | 1.189 / frame |
| **StreamBuffer Wraps** | 0 | 0 | 0 |
| **StreamBuffer Waits** | 0 | 0 | 0 |

### Hallazgos Clave
1. **Write Tracking Inactivo en Escenas Estables**:
   Tanto en vista amplia como mirando al suelo, las faltas de página del gestor de memoria virtual (`PageFaultManager`) son mínimas (< 1 falta por segundo). Esto demuestra que en escenas estáticas el juego no modifica continuamente los buffers de vértices mapeados mediante la CPU.
2. **Comportamiento en la Transición de Aterrizaje**:
   A diferencia de las escenas estáticas, durante la cinemática de aterrizaje entran en juego mallas de terreno dinámicas, animación por huesos de los propulsores de la nave y streaming de chunks. Esto genera ráfagas transitorias de subida a `StreamBuffer` que pueden llegar a superar los 10 MiB por frame.

---

## 3. Comparativa de StreamBuffer: Estándar (64 MiB) vs Extendido (256 MiB)

En la Etapa 8 se introdujo el flag opcional `--extended-stream-buffer` para cuadruplicar la capacidad del búfer de anillo (`StreamBuffer`) de 64 MiB a 256 MiB y el de descargas de 32 MiB a 128 MiB en [`src/graphics/host_gpu/renderer/cache/streamBuffer.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/streamBuffer.cpp).

### Análisis de Resultados:
- **En escenas estáticas (Vista Amplia y Suelo)**:
  No se observó diferencia de frametime entre 64 MiB y 256 MiB, debido a que el consumo por frame ronda 1 MiB y la cadencia de sumisión periódica (`--gpu-submit-cadence` a 1.0 ms) drena y recicla el búfer mucho antes de que alcance el wrap de 64 MiB.
- **En la Transición de Aterrizaje**:
  Cuando la nave realiza la aproximación final (`GroundApproach` y `Touchdown`), la tasa de transferencia geométrica puede causar bloqueos si el hilo de renderizado se retrasa compilando un shader. Un búfer de 256 MiB proporciona un colchón de absorción de hasta 4 veces más frames sin forzar esperas de sincronización `vkWaitSemaphores` en el host.

La **Opción 6** de `stage8_1_camera_and_landing.cmd` permite medir este fenómeno de forma aislada durante el aterrizaje.

---

## 4. Sincronización CPU/GPU y Submit Cadence

El programador de comandos (`CommandScheduler`) en [`src/graphics/host_gpu/renderer/commandScheduler.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandScheduler.cpp) implementa la cadencia periódica de submits:
- Cuando `--gpu-submit-cadence` está activo, se emite un batch a Vulkan cada 1.0 ms si se ha registrado trabajo.
- Esto mantiene a la GPU NVIDIA RTX 5080 constantemente ocupada procesando draws mientras la CPU todavía está traduciendo los siguientes paquetes PM4 de la cola guest.
- En la vista amplia, se registraron **48,2 submits por frame**, lo que confirma que la cola de la GPU nunca se queda sin trabajo y que el estancamiento a ~18–19 FPS es puramente de latencia en la CPU.

---

## 5. Conclusiones y Recomendaciones de Memoria

1. Mantener activas las optimizaciones de Etapa 5 (`--gpu-submit-cadence` a 1.0 ms y `--partial-buffer-refresh`).
2. Mantener el tamaño de 64 MiB como predeterminado para el StreamBuffer, dejando `--extended-stream-buffer` (256 MiB) disponible mediante flag toggleable para escenas pesadas con streaming de mundo masivo.
3. La coherencia de memoria y barreras de Vulkan no deben relajarse sin comprobación dirty previa, ya que cualquier fallo causaría artefactos visuales severos o corrupción de texturas en Astro Bot.
