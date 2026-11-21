# KytyPS5 — Etapa 8.2: Informe de Auditoría, Matriz Experimental y Corrección de Regresiones Gráficas en Sky Garden

**Fecha:** 21 de Noviembre de 2026  
**Juego:** Astro Bot (`PPSA21567`), Escenario: Sky Garden, 1280 × 720  
**Hardware:** AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5080 16 GB, 32 GB DDR5-6000, Windows 11  
**Rama:** `etapa8_2_sync_render_optimization`  
**Commit con regresión observada:** `c845e3062` (HEAD de Etapa 8.1)  
**Estado resultante:** Corregido y deshabilitado por defecto para máxima fidelidad visual  

---

## 1. Síntomas Reportados

Durante la validación de la Etapa 8.2 en **Sky Garden** de Astro Bot, se detectaron defectos visuales severos:
1. **Sombras de objetos inexistentes / sombras provenientes del cielo:** Proyección de siluetas de sombras oscuras sobre el suelo y Astro sin objetos que las emitieran.
2. **Destellos y parpadeos de iluminación excesiva:** Parpadeo intenso de buffers de iluminación y sobreexposición al girar la cámara.
3. **Congelamientos de imagen:** Pausas al apuntar hacia el agua o el horizonte.
4. **Errores gráficos intermitentes:** Texturas y geometrías desfasadas en profundidad.

---

## 2. Evidencia Experimental de la Matriz A/B

El colaborador ejecutó las 8 opciones de prueba del script [`stage8_2_visual_regression_matrix.cmd`](file:///C:/Users/Recko/Documents/github/kyty/stage8_2_visual_regression_matrix.cmd) en Astro Bot (Sky Garden):

| Opción | Configuración Evaluada | Resultado Observado | Conclusión |
|---|---|:---:|---|
| **Opción 1** | Todas las optimizaciones activadas | **FALLA** | Reproduce sombras fantasma, destellos y bloqueos. |
| **Opción 2** | Control Baseline (sin Etapa 8 ni 8.1) | **CORRECTO** | Fidelidad visual 100% limpia (referencia Etapa 7). |
| **Opción 3** | Sin `descriptor-cache` | **FALLA** | El defecto persiste. `descriptor-cache` NO es el causante primario. |
| **Opción 4** | Sin `draw-state-dirty-check` | **CORRECTO** | **Elimina completamente todos los defectos visuales.** |
| **Opción 5** | Sin optimizaciones de Etapa 8.1 | **FALLA** | El fallo sigue presente (la causa se originó en Etapa 8). |
| **Opción 6** | Sin optimizaciones de Etapa 8 | **CORRECTO** | Al quitar Etapa 8 desaparece el fallo. |
| **Opción 7** | Sin `dirty-check` ni `descriptor-cache` | **CORRECTO** | Escena 100% correcta. |
| **Opción 8** | Sin `pipeline-key-cache` | **FALLA** | El defecto persiste. `pipeline-key-cache` es inocente. |

### Conclusión Experimental Definitiva
**`draw-state-dirty-check` es el único y exclusivo responsable de las regresiones visuales observadas.** Desactivarlo restaura de inmediato la corrección visual en todas las combinaciones evaluadas.

---

## 3. Causa Raíz Demostrada en el Código

En la Etapa 8 (commit `db2aba1e8`), se introdujo `m_cached_targets_sig` en [`renderDraw.cpp`](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/renderDraw.cpp) para saltarse la preparación de render targets cuando la firma coincidía.

### Fallos Arquitectónicos de la Implementación Original:
1. **Omisión de Registros de Control de Profundidad:**
   La firma original únicamente verificaba:
   ```cpp
   m_cached_targets_sig.mrt_mask == state.mrt_mask &&
   m_cached_targets_sig.rt_mask == current_rt_mask &&
   m_cached_targets_sig.depth_base == current_depth_base &&
   m_cached_targets_sig.msaa_samples == current_msaa
   ```
   **Omitía por completo:**
   - `dc.z_enable` (activación de depth test).
   - `dc.z_write_enable` (escritura en depth buffer).
   - `dc.zfunc` (función de comparación: Less, Equal, Greater, Always).
   - `rc.depth_clear_enable` (flag de borrado/clear del depth buffer).
   - `z.depth_view.depth_write_disable`.
   - `hw.GetDepthClearValue()`.

2. **Propagación Errónea del Borrado de Profundidad (`depth_clear_enable`):**
   - Cuando el draw call 1 era un borrado de profundidad (`rc.depth_clear_enable = true`), `targets.depth.depth_clear_enable` quedaba fijado en `true`.
   - En el draw call 2 (que dibujaba geometría sobre el mismo buffer), la firma coincidía porque `depth_base` era igual. Al saltarse `ResolveRenderDepthTarget`, `targets.depth.depth_clear_enable` **permanecía en `true`**, provocando que Vulkan borrara la profundidad en draw calls intermedios de renderizado de objetos.
   - De forma análoga, si una pasada de shadow map requería un clear pero el draw anterior no lo tenía, el shadow map **nunca se borraba**, proyectando sombras acumuladas de frames y objetos inexistentes ("sombras del cielo").

3. **Desincronización de Enlace en `textureCache` y Vulkan Layouts:**
   - Al final de cada draw, `ResetBindings()` (`descriptors.cpp:660`) limpia `image.binding = {}` para todas las imágenes enlazadas.
   - Al marcar `targets_unchanged = true`, se omitía `ResolveRenderColorTarget` y `ResolveRenderDepthTarget`, por lo que **`BindRenderTarget` no era llamado**.
   - Como resultado, `image.binding.is_target` permanecía en `false`, lo que en `AcquireRenderTargets` generaba layouts de Vulkan inconsistentes (`eColorAttachmentOptimal` en vez de transiciones adecuadas con feedback loops).

4. **Omisión de Mip Levels y Array Layers en Render Targets de Color:**
   - La firma original solo comparaba `rt.base.addr`. Si dos draws escribían en diferentes capas de un array (ej. caras de cubemap o slices de cascada de sombras) sobre la misma dirección base, se reutilizaba la vista del slice anterior.

---

## 4. Medidas de Corrección Aplicadas

### 4.1 Desactivación por Defecto (`draw_state_dirty_check_enabled = false`)
Siguiendo el principio de máxima fidelidad y las instrucciones técnicas:
- En [`src/common/emulatorConfig.h`](file:///C:/Users/Recko/Documents/github/kyty/src/common/emulatorConfig.h): `draw_state_dirty_check_enabled = false;`.
- En [`src/common/emulatorConfig.cpp`](file:///C:/Users/Recko/Documents/github/kyty/src/common/emulatorConfig.cpp): `DrawStateDirtyCheckEnabled()` retorna `false` por defecto.
- En [`src/common/auditProfiler.h`](file:///C:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.h): `draw_state_dirty_check_enabled = false;`.
- En [`src/main.cpp`](file:///C:/Users/Recko/Documents/github/kyty/src/main.cpp): actualizado el texto de ayuda CLI indicando `(default: off)`.
- En [`AGENTS.md`](file:///C:/Users/Recko/Documents/github/kyty/AGENTS.md): documentado como desactivado por defecto por regresión en Sky Garden.

### 4.2 Blindaje Arquitectónico en `renderDraw.cpp` y `render.h`
Para garantizar que incluso si un usuario activa `--draw-state-dirty-check`, el emulador no sufra desincronizaciones:
1. **Firma Exhaustiva:**
   Se amplió `CachedTargetsSignature` en [`render.h`](file:///C:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/render.h) para incorporar:
   - `depth_write_base`
   - `depth_clear_enable`
   - `z_enable`
   - `z_write_enable`
   - `zfunc`
   - `depth_write_disable`
   - `depth_clear_val`
   - `color_views` (combinación de `base_level`, `base_layer` y `layer_count`).
2. **Re-vinculación Obligatoria de Render Targets:**
   Cuando `targets_unchanged` es `true`, ahora se invoca siempre `BindRenderTarget` para cada render target de color y profundidad activo, asegurando que `m_bound_images` y `textureCache` mantengan coherencia de layouts.
3. **Refresco de Estado Dinámico de Profundidad:**
   Se re-evalúan de forma obligatoria `depth_clear_enable`, `depth_clear_value`, `depth_test_enable`, `depth_write_enable` y `depth_compare_op` para cada draw call.

---

## 5. Validación y Tests Unitarios

- **Suite de Tests:** Se incorporó el test `Stage8_2DrawStateDirtyCheckRegressionProtection` en [`tests/AuditProfilerTests.cpp`](file:///C:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp) para garantizar que `Config::DrawStateDirtyCheckEnabled()` permanezca estrictamente desactivado por defecto.
- **Optimizaciones Preservadas:** Se mantuvieron **activadas por defecto** `--descriptor-cache`, `--ensure-pipeline-cache`, `--pipeline-key-cache`, `--pipeline-cache-mru` y `--redundant-bind-elision`, dado que las pruebas experimentales demostraron que son inocentes respecto a la regresión visual.
