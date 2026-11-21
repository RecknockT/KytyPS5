# KytyPS5 — Estado Actual del Rendimiento y Diagnóstico Técnico (Etapa 8)

Este documento resume el estado empírico del emulador KytyPS5, diferenciando estrictamente entre **resultados medidos y confirmados** y **proyecciones teóricas pendientes de validación**.

---

## 1. Resultados Confirmados Experimentalmente (Etapa 7)

Las siguientes métricas fueron medidas de manera reproducible en sesiones de 30 segundos en **Astro Bot** (`PPSA21567`), nivel **Aztec Sky**, a 1280 × 720 sobre hardware AMD Ryzen 7 9700X + NVIDIA GeForce RTX 5080 16 GB:

| Escenario de Prueba | FPS Efectivos | Frametime Promedio | P95 Frametime | Draws / Frame |
|---|---:|---:|---:|---:|
| **Vista Amplia** (Optimizaciones Etapa 7 activas) | **20,14** | 49,67 ms | 51,40 ms | ~5.205 |
| **Vista Amplia** (Control: sin optimizaciones Etapa 7) | **19,92** | 50,20 ms | 51,80 ms | ~5.210 |
| **Vista al Suelo** (StreamBuffer normal 64 MiB) | **59,37** | 17,03 ms | 17,30 ms | ~1.200 |
| **Vista al Suelo** (StreamBuffer extendido 256 MiB) | **45,18** | 22,14 ms | 19,80 ms | ~1.198 |

---

## 2. Hallazgos Técnicos Verificados

1. **La Carga Gráfica Explica la Disparidad de FPS**:
   - En **Vista Amplia**, el motor emite aproximadamente **5.200 draws por frame**.
   - En **Vista al Suelo**, la oclusión y el frustum culling del juego reducen la carga a aproximadamente **1.200 draws por frame** (-77%).
   - Esta diferencia de 4.000 draws por frame explica la brecha de rendimiento entre ~20 FPS y ~60 FPS.

2. **Impacto de las Optimizaciones de la Etapa 7**:
   - La elisión de enlaces redundantes (`--redundant-bind-elision`) y el caché MRU de pipelines (`--pipeline-cache-mru`) aportaron una mejora pequeña en Vista Amplia (+0,22 FPS, reduciendo el frametime en ~0,53 ms).
   - Aunque la tasa de acierto del MRU superó el 77%, la serialización y construcción de claves de pipeline (`MakeGraphicsPipelineKey`) seguía consumiendo tiempo de CPU en cada draw.

3. **El Fracaso del StreamBuffer Extendido (256 MiB)**:
   - La ampliación del buffer de streaming a 256 MiB **no resolvió los congelamientos**, sino que los agravó notablemente.
   - En Vista al Suelo provocó **13 hitches severos \(\ge 100\) ms** (frente a solo 2 con el buffer estándar de 64 MiB), con un peor fotograma de **1.483,2 ms**.
   - Causa técnica: un buffer de 256 MiB acumula cientos de megabytes de comandos pendientes antes de hacer un wraparound. Cuando finalmente se requiere reciclar memoria, la CPU se bloquea en `MasterSemaphore::Wait` durante cientos de milisegundos esperando que la GPU drene todo el trabajo acumulado.

---

## 3. Innovaciones Incorporadas en la Etapa 8

Para abordar la causa raíz identificada en la Etapa 7, la Etapa 8 introduce:

1. **Instrumentación Fina de 13 Etapas del Pipeline**: Medición con `ScopedStageTimer` de nanosegundos para `PipelineKeyBuild`, `RenderStatePrep`, `CommandRecording`, `DescriptorPrep`, etc.
2. **Separación CPU / Esperas / GPU**: Medición de tiempo de CPU real mediante la API nativa de Windows `GetThreadTimes()`.
3. **Búfer Circular de Trazas (Ring Buffer)**: Registro lock-free de 16.384 eventos para correlacionar exactamente qué causó cada hitch \(\ge 100\) ms.
4. **Optimización `--pipeline-key-cache`**: Elisión de la reconstrucción de `GraphicsPipelineKey` cuando el estado de hardware no cambia.
5. **Optimización `--draw-state-dirty-check`**: Omisión de la resolución redundante de render targets y sample counts entre draws consecutivos.

---

## 4. Aclaración Importante: Mediciones Reales vs. Proyecciones Teóricas

> [!WARNING]
> **El Rendimiento de la Etapa 8 en Astro Bot Continúa Pendiente de Validación Experimental.**

- **Proyección Teórica de ~26,2 FPS**:
  Los documentos técnicos de la Etapa 8 proyectan que el ahorro de ~11,6 ms de CPU por frame en Vista Amplia podría elevar la tasa de 20,14 FPS a aproximadamente ~26,2 FPS. **Esta cifra es una estimación matemática basada en microbenchmarks sintéticos de CPU, NO un resultado medido en juego**.
- **Medición de Tiempos de GPU**:
  Los valores de ejecución de GPU reportados hasta la Etapa 7 corresponden al tiempo de CPU del hilo de renderización deduciendo las esperas de sincronización. Las marcas de tiempo físicas directas en hardware de GPU mediante pools de queries de Vulkan se encuentran en fase de desarrollo y no deben asumirse como mediciones finales de hardware hasta disponer de capturas con queries completas.
- **Validación Requerida**:
  El colaborador que ejecute `stage8_camera_ab.cmd` obtendrá las primeras mediciones experimentales reales de la Etapa 8 para contrastar la efectividad de `--pipeline-key-cache` y `--draw-state-dirty-check`.
