# KytyPS5 — Análisis de Costo por Draw, Procesamiento PM4 y Evaluación de Batching

## 1. Desglose del Costo por Draw

En la captura de Vista Amplia (Aztec Sky, Astro Bot):
- **Draws por frame:** 5.031,81
- **Dispatches por frame:** 234,62
- **Frametime promedio:** 48,68 ms (20,57 FPS)
- **Frametime en Vista al Suelo:** 18,99 ms con 1.200,57 draws/frame (52,68 FPS)

### Comparación diferencial:
- Diferencia de tiempo: $\Delta T = 48,68\text{ ms} - 18,99\text{ ms} = 29,69\text{ ms}$
- Diferencia de draws: $\Delta D = 5.031,81 - 1.200,57 = 3.831,24\text{ draws}$
- **Costo marginal por draw adicional:**
  $$\frac{29,69\text{ ms}}{3.831,24\text{ draws}} \approx 7,75\,\mu\text{s / draw}$$

---

## 2. Componentes del Costo por Draw

Cada llamado a `DrawIndex` o `DrawAuto` en Kyty ejecuta dos fases principales:

### Fase 1: Despacho PM4 y Validación Guest (~25% del tiempo por draw, ~2,0 µs)
1. **Decodificación PM4:** Lectura de cabeceras, extracción de opcodes y longitud de paquete en `CommandProcessor::ProcessPm4`.
2. **Despacho del Handler:** Salto indirecto a través de la tabla de funciones `g_cp_op_func`.
3. **Mapeo de Argumentos:** Conversión de registros AMD PM4 (ej. `m_index_type_and_size`, `m_index_base_addr`, `NumInstances`).
4. **Verificación de Flush:** Comprobación del contador `m_draws` contra `m_flush_draws`.

### Fase 2: Host Renderer y Registro Vulkan (~75% del tiempo por draw, ~5,75 µs)
1. **Sincronización:** Adquisición de `m_context.GetMutex()`.
2. **Resolución de Render Targets:** Inspección de attachments en `ResolveRenderColorTarget` y `ResolveRenderDepthTarget`.
3. **Búsqueda de Pipeline (`PipelineCache::GetGraphicsPipeline`):**
   - Construcción de `GraphicsPipelineKey` (~128 bytes).
   - Hashing `XXH3_64bits`.
   - Búsqueda en `std::unordered_map<GraphicsPipelineKey, Pipeline>`.
4. **Vinculación de Buffers y Estado:**
   - Vinculación incondicional de buffers de vértices vía `vkCmdBindVertexBuffers2`.
   - Vinculación incondicional de buffer de índices vía `vkCmdBindIndexBuffer`.
   - Vinculación incondicional de pipeline vía `vkCmdBindPipeline`.
5. **Actualización de Estado Dinámico (`UpdateGraphicsDynamicState`):**
   - Recálculo de 16 viewports y 16 scissors con `calc_final_scissor`.
   - Llamadas a `DynamicState::Set*` y `DynamicState::Commit`.
6. **Emisión de Push Constants y Draw:**
   - `vkCmdPushConstants` para `ObjectId` / Mesh task params.
   - `vkCmdDrawIndexed` o `vkCmdDraw`.

---

## 3. Evaluación de Draw Batching y MultiDrawIndirect

El requerimiento de la Etapa 7 establece:
> *"Determinar si Vulkan permite agrupar esas operaciones sin modificar resultados. No implementar MultiDrawIndirect automáticamente."*

### Análisis Técnico:
1. **Dependencia de Push Constants (`ObjectId`):**
   - En cada llamada a `ExecutePreparedDraw`, el emulador emite:
     ```cpp
     draw_data[0] = buffer.GetUserConfig().GetObjectId();
     vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_stages, 0,
                             draw_dwords * sizeof(uint32_t), draw_data.data());
     ```
   - El valor de `ObjectId` cambia entre objetos del juego para indexar constantes de objeto o datos de material.
   - En la API Vulkan estándar, `vkCmdDrawIndexedIndirect` ejecuta múltiples sub-draws compartiendo exactamente los mismos push constants y descriptor sets.
   - Para que `MultiDrawIndirect` funcione con diferentes `ObjectId`, el recompiler de shaders de Kyty tendría que reescribir todos los shaders para que tomen `gl_DrawID` como índice dentro de un buffer SSBO global de `ObjectId`s.
2. **Variabilidad de Buffers de Vértices y Descriptores:**
   - Los draws consecutivos frecuentemente referencian distintos sub-rangos de vertex buffers o diferentes descriptor tables (texturas de material).
3. **Complejidad y Riesgo:**
   - Implementar `MultiDrawIndirect` host-side requeriría:
     - Generar un buffer indirecto dinámico por frame en CPU.
     - Detectar secuencias consecutivas con idéntico pipeline, descriptores, buffers y estados dinámicos.
     - Modificar el pipeline de shaders para soportar `gl_DrawID`.
   - El porcentaje de draws que son estrictamente idénticos en pipeline, descriptores y buffers (sin contar `ObjectId`) ya son en su mayoría instanciados por el propio motor del juego (`DrawIndexMultiInst` / `instances_per_frame = 20.771`).
4. **Conclusión:**
   - **Se descarta la implementación inmediata de MultiDrawIndirect** en esta etapa. El riesgo de rotura gráfica y regresiones por desincronización de push constants es inaceptable sin una reestructuración completa del backend de shaders SPIR-V.

---

## 4. Oportunidades Identificadas de Reducción de Redundancias

En lugar de un batching riesgoso a nivel de draws, la reducción directa de trabajo redundante dentro de cada draw ofrece un beneficio seguro e inmediato:

1. **Elisión de Vinculación de Pipeline en CommandBuffer:**
   - Rastrear el último `vk::Pipeline` bound en el `CommandBuffer`.
   - Si `pipeline.pipeline == m_bound_graphics_pipeline`, omitir la llamada `vkCmdBindPipeline`.
   - Ahorro: evita hasta ~4.800 llamadas al driver por frame.
2. **Elisión de Vinculación de Buffers de Vértices e Índices:**
   - Rastrear buffers, offsets y tamaños actualmente enlazados.
   - Si no hay cambios, omitir `vkCmdBindVertexBuffers2` y `vkCmdBindIndexBuffer`.
   - Ahorro: evita miles de llamadas al driver por frame.
3. **Caché MRU de Pipeline en `PipelineCache`:**
   - Almacenar la última clave `GraphicsPipelineKey` y el puntero al `Pipeline` resuelto.
   - Si la clave coincide byte a byte (`operator==`), retornar el puntero inmediatamente sin calcular `XXH3_64bits` ni buscar en `std::unordered_map`.
   - Ahorro: elimina el costo de hashing y búsqueda en mapa hash para la gran mayoría de draws consecutivos.
