# KytyPS5 — Plan de Optimizaciones Priorizadas (Etapa 7)

## 1. Tabla de Evaluación de Optimizaciones Candidatas

| Optimización candidata | Evidencia técnica | Riesgo | Costo | Beneficio esperado | Estado en Etapa 7 |
|---|---|---|---|---|---|
| **1. Elisión de Vulkan Binds Redundantes** (`bindPipeline`, `bindVertexBuffers2`, `bindIndexBuffer`) | En Vista Amplia se emiten 5.031 draws/frame. En cada draw se invocan incondicionalmente estas llamadas al driver Vulkan, a pesar de que >90% de draws consecutivos comparten pipeline y buffers. | **Mínimo** (La spec de Vulkan garantiza que el estado vinculado persiste en el `VkCommandBuffer` hasta ser sobrescrito). | Muy bajo (comparaciones escalares en host CPU antes de invocar la API del driver). | Elimina ~10.000 a 14.000 llamadas al driver Vulkan por fotograma en escenas complejas, reduciendo la sobrecarga de CPU en el hilo de render. | **IMPLEMENTADA** (Toggle: `--no-redundant-bind-elision`) |
| **2. PipelineCache Fast-Path MRU** (Bypass de `XXH3_64bits` y `std::unordered_map`) | 5.031 consultas por frame a `m_graphics_pipelines`. Cada consulta ejecuta hashing XXH3 sobre una clave de ~128 bytes y recorre el mapa hash. Ráfagas de draws con idéntico pipeline son comunes. | **Nulo** (Si la clave es idéntica byte a byte por `operator==`, devuelve directamente el puntero al pipeline cacheado). | Mínimo (comparación directa de memoria vs hashing + bucket lookup). | Ahorro sustancial de ciclos de CPU en el hilo GPU para draws consecutivos con el mismo pipeline. | **IMPLEMENTADA** (Toggle: `--no-pipeline-cache-mru`) |
| **3. Mitigación de Hitches de StreamBuffer** (`--extended-stream-buffer`) | 10 paradas de frametime (>100 ms, máx 560 ms) en Vista al Suelo (52,68 FPS) causadas por wraparound y contención en `StreamBufferWait` sobre el buffer de 64 MiB. | **Bajo** | ~192 MiB de memoria host adicional (256 MiB vs 64 MiB). | Elimina por completo las 10 pausas severas de frametime en escenas ligeras/altas tasas de FPS. | **VALIDADA / RECOMENDADA** (Flag: `--extended-stream-buffer`) |
| **4. Dirty Tracking de Dynamic State (Viewports & Scissors)** | Se calculan 16 viewports y 16 scissors con `calc_final_scissor` en cada uno de los 5.031 draws, aunque los registros no cambien. | **Medio** (Riesgo de omitir actualizaciones si el tracking de dirty bits de registros guest falla). | Medio (requiere dirty flags en `Registers`). | Ahorro moderado de operaciones de punto flotante por draw. | *Planificada para Etapa 7.1* |
| **5. Draw Batching / MultiDrawIndirect** | Reducir el conteo de draws agrupando llamadas compatibles. | **Alto** (Astro Bot emite push constants de `ObjectId` por draw; los shaders SPIR-V de Kyty no soportan `gl_DrawID` dinámico sin reescribir el compilador). | Muy alto (buffer indirecto dinámico y refactor de shaders). | Potencial reducción de draws si los shaders se adaptaran. | **DESCARTADA** (Riesgo alto de rotura gráfica y regresiones) |

---

## 2. Decisiones de Diseño e Invariantes

### Invariante 1: Aislamiento por `CommandBuffer`
- El rastreo del pipeline, buffers de vértices y buffer de índices se realiza a nivel de cada instancia de `CommandBuffer`.
- En `CommandBuffer::Begin()`, cuando se inicia un nuevo buffer de comandos de Vulkan, todos los estados cacheados se reinician incondicionalmente a `nullptr`/vacío.
- Esto garantiza que ningún estado pendiente de un command buffer previo afecte al nuevo command buffer.

### Invariante 2: Preservación Semántica de Vulkan
- La especificación de Vulkan (secciones *Pipeline Binding*, *Vertex Input Description* y *Index Buffer Binding*) define explícitamente que los bindings permanecen vigentes a lo largo de todo el ciclo de vida del `VkCommandBuffer` y a través de render passes dinámicos (`vkCmdBeginRendering` / `vkCmdEndRendering`), a menos que sean re-enlazados.
- Por tanto, eludir un bind con los mismos parámetros exactos es 100% equivalente a emitirlo.

### Invariante 3: Control Independiente de Flags
- Ambas optimizaciones están activas por defecto para máximo rendimiento, pero cuentan con flags CLI (`--no-redundant-bind-elision` y `--no-pipeline-cache-mru`) para permitir diagnósticos A/B limpios.
