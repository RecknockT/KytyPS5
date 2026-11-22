# KytyPS5 — Etapa 8.3: Auditoría y Diseño de Presets para Astro Bot

## 1. Contexto y Objetivos

El uso de archivos de preset específicos para cada juego permite aplicar la combinación óptima de opciones del motor gráfico y de sincronización sin obligar al usuario a memorizar ni concatenar decenas de flags de línea de comandos.

En esta auditoría se establece:
1. El análisis del sistema actual de presets en KytyPS5.
2. La definición de dos presets reproducibles para Astro Bot (`PPSA21567`):
   - **Preset Astro Bot Optimizado** (máximo rendimiento seguro, calidad visual intacta).
   - **Preset Conservador / Baseline** (control estricto, sin cachés experimentales).

---

## 2. Auditoría del Sistema de Presets Actual

### Estado Actual en nuestro Fork:
- En `src/launcher/`: el launcher implementa presets guardando pares clave-valor en `QSettings` por cada juego si el usuario marca "Use custom settings for this game".
- En `src/main.cpp`: el emulador directo no lee archivos de presets automáticamente; depende de los flags pasados en la línea de comandos.
- En `Jetsku/KytyPS5`: los presets se leen de archivos JSON denominados `u59-preset.<title_id>.json` que residen en la raíz del emulador y configuran variables de entorno.

### Reglas de Prioridad y Jerarquía Recomendadas:
Para garantizar reproducibilidad y evitar colisiones:
1. **Línea de Comandos (CLI Flags):** Máxima prioridad. Si se pasa `--no-descriptor-cache`, este flag anula cualquier preset.
2. **Preset Específico de Juego (`presets/PPSA21567.json` o `--preset astrobot_optimized`):** Segunda prioridad. Aplica configuraciones optimizadas para ese título.
3. **Preset Global (`presets/default.json`):** Tercera prioridad.
4. **Valores Predeterminados Hardcodeados:** Cuarta prioridad (fallback seguro en código).

---

## 3. Preset Astro Bot Optimizado (Recomendado)

Este preset reúne **únicamente las optimizaciones validadas experimentalmente** que aportan beneficios de rendimiento sin comprometer la corrección gráfica (manteniendo explícitamente desactivado `draw-state-dirty-check` para evitar las regresiones de sombras y destellos en Sky Garden).

### Definición de Parámetros:

```json
{
  "title_id": "PPSA21567",
  "name": "Astro Bot - Rendimiento Seguro y Calidad Original",
  "screen_resolution": "1280x720",
  "present_mode": "Mailbox",
  "vblank_frequency": 60,
  "shader_cache": true,
  "async_shader_compile": true,
  "redundant_bind_elision": true,
  "pipeline_cache_mru": true,
  "pipeline_key_cache": true,
  "ensure_pipeline_cache": true,
  "descriptor_cache": true,
  "program_cache_mru": true,
  "coalesce_read_faults": true,
  "batch_hot_readbacks": true,
  "partial_buffer_refresh": true,
  "gpu_submit_cadence": true,
  "gpu_submit_cadence_interval_us": 1000,
  "extended_stream_buffer": false,
  "draw_state_dirty_check": false,
  "tessellation": false,
  "occlusion_queries": false,
  "relaxed_shader_float": false,
  "native_rt": false,
  "vulkan_validation": false,
  "shader_validation": false
}
```

### Flags CLI Equivalentes:
```cmd
--screen-width 1280 --screen-height 720 ^
--present-mode Mailbox ^
--shader-cache ^
--redundant-bind-elision ^
--pipeline-cache-mru ^
--pipeline-key-cache ^
--ensure-pipeline-cache ^
--descriptor-cache ^
--batch-hot-readbacks ^
--partial-buffer-refresh ^
--gpu-submit-cadence ^
--gpu-submit-cadence-interval-us 1000 ^
--no-draw-state-dirty-check
```

---

## 4. Preset Conservador (Control / Baseline)

Diseñado para depuración y como grupo de control estricto ante cualquier sospecha de regresión. Desactiva todas las optimizaciones experimentales de las Etapas 5 a 8.2:

### Definición de Parámetros:

```json
{
  "title_id": "PPSA21567",
  "name": "Astro Bot - Control Conservador",
  "screen_resolution": "1280x720",
  "present_mode": "Mailbox",
  "vblank_frequency": 60,
  "shader_cache": true,
  "async_shader_compile": true,
  "redundant_bind_elision": false,
  "pipeline_cache_mru": false,
  "pipeline_key_cache": false,
  "ensure_pipeline_cache": false,
  "descriptor_cache": false,
  "batch_hot_readbacks": false,
  "partial_buffer_refresh": false,
  "gpu_submit_cadence": false,
  "extended_stream_buffer": false,
  "draw_state_dirty_check": false,
  "tessellation": false,
  "occlusion_queries": false,
  "relaxed_shader_float": false,
  "native_rt": false,
  "vulkan_validation": false,
  "shader_validation": false
}
```

### Flags CLI Equivalentes:
```cmd
--screen-width 1280 --screen-height 720 ^
--present-mode Mailbox ^
--shader-cache ^
--no-redundant-bind-elision ^
--no-pipeline-cache-mru ^
--no-pipeline-key-cache ^
--no-ensure-pipeline-cache ^
--no-descriptor-cache ^
--no-draw-state-dirty-check
```

---

## 5. Compromisos Estrictos de Integridad

1. **PROHIBICIÓN ABSOLUTA DE FALSEAR FPS:**
   - Queda terminantemente prohibido alterar `vblank_frequency` a valores ficticios, modificar el reloj del guest (`--guest-tsc`) o acelerar el tiempo interno del juego para mostrar más frames por segundo en el overlay.
2. **INTEGRIDAD VISUAL:**
   - Ningún preset debe reducir silenciosamente la resolución interna de renderizado, suprimir attachments de iluminación o deshabilitar efectos de postprocesado sin declararlo explícitamente como una degradación visual.
