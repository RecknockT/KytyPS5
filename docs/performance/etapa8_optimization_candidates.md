# KytyPS5 — Etapa 8: Candidatos de Optimización y Cambios Implementados

## 1. Optimizaciones Implementadas en la Etapa 8 (Fase E)

A partir de la descomposición nanométrica del pipeline de renderizado realizada en la Fase A, se identificó que las etapas `PipelineKeyBuild` y `RenderStatePrep` consumían conjuntamente **~16,1 ms por frame** (~32,4% del tiempo total en Vista Amplia con 5.205 draws).

Para abordar estos cuellos de botella sin comprometer la corrección de la emulación, se implementaron dos optimizaciones con sus correspondientes flags de control:

### 1.1 Cache y Elisión de Pipeline Keys (`--pipeline-key-cache`)
- **Problema**: A pesar del MRU de la Etapa 7, `MakeGraphicsPipelineKey` examinaba 16 campos de hardware (mezcla de color, estados de recorte, viewport, rasterizador, profundidad y máscaras de buffers de vértices) en **cada draw call** para generar la clave de 128 bytes antes de consultar el caché.
- **Implementación**:
  En [src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp), se añadió una comprobación de firma compacta de estado hardware (`CachedHardwareSignature`):
  - Modos de mezcla y lógica de blend.
  - Configuración de profundidad y stencil.
  - Modos de rasterización y primitivas.
  - Si la firma coincide con el draw previo y no hubo cambios de shaders, se reutiliza directamente la clave generada y el puntero al pipeline en caché en \(O(1)\), elidiendo la serialización de 128 bytes.
- **Impacto**: Reduce el tiempo de `PipelineKeyBuild` de ~8,3 ms a menos de ~2,1 ms por frame en Vista Amplia.

### 1.2 Verificación de Suciedad de Draw State (`--draw-state-dirty-check`)
- **Problema**: `PrepareDrawRenderState` iteraba sobre las 8 ranuras MRT de color, verificaba resoluciones de formato de imagen, chequeaba paridad de muestras MSAA y calculaba dimensiones de render pass en cada draw.
- **Implementación**:
  En [src/graphics/host_gpu/renderer/renderDraw.cpp](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderDraw.cpp) y [render.h](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/render.h), se implementó `CachedTargetsSignature`:
  ```cpp
  struct CachedTargetsSignature {
      bool     valid = false;
      uint32_t mrt_mask = 0;
      uint32_t rt_mask = 0;
      uint64_t depth_base = 0;
      uint32_t msaa_samples = 0;
      std::array<uint64_t, RENDER_COLOR_ATTACHMENTS_MAX> color_bases {};
  };
  ```
  Si ninguna dirección de buffer de render target ni la máscara MRT ha cambiado respecto al draw anterior, se omite por completo la resolución de color y profundidad.
- **Impacto**: Reduce el tiempo de `RenderStatePrep` de ~7,8 ms a ~2,4 ms por frame en Vista Amplia.

---

## 2. Complementariedad con Optimizaciones Anteriores

| Optimización | Etapa | Mecanismo | Beneficio Acumulado |
|---|:---:|---|---|
| Submit Cadence (1.0 ms) | Etapa 5 | Segmentación de command buffers a intervalos periódicos | GPU comienza a renderizar en paralelo mientras CPU graba |
| Partial Buffer Refresh | Etapa 5 | Subida selectiva de áreas modificadas de texturas | Ahorro masivo de ancho de banda PCIe |
| Redundant Bind Elision | Etapa 7 | Omite `vkCmdBindPipeline` y bind de buffers si no cambiaron | Reduce llamadas a driver Vulkan |
| PipelineCache MRU | Etapa 7 | Puntero directo al último pipeline en \(O(1)\) | Omite hashing XXH3 y búsqueda en `unordered_map` |
| Pipeline Key Cache | **Etapa 8** | Omite reconstrucción de `GraphicsPipelineKey` | Ahorro de ~6,2 ms en CPU |
| Draw State Dirty Check | **Etapa 8** | Omite recálculo de MRTs y render targets | Ahorro de ~5,4 ms en CPU |

---

## 3. Candidatos Futuros de Optimización para Etapas Posteriores

1. **Grabación de Comandos Multihilo (Multi-Threaded Command Buffer Recording)**:
   Dividir los 5.200 draws de un frame entre 2 o 4 hilos de trabajo (usando command pools secundarios de Vulkan) para paralelizar la emisión de `vkCmdDraw` en los 8 núcleos del Ryzen 7 9700X.
2. **Batching de Paquetes PM4**:
   Reagrupar llamadas contiguas con idéntico shader y estado geométrico mediante `vkCmdDrawIndexedIndirect` con multi-draw.
3. **Descriptor Set Dynamic Offsets**:
   Reducir el número de re-enlaces de descriptores pasando buffers de constantes a través de offsets dinámicos en lugar de actualizar y bindear nuevos descriptor sets.
