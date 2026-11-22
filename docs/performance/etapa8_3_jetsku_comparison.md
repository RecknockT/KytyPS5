# KytyPS5 — Etapa 8.3: Comparativa Técnica con el Fork Jetsku/KytyPS5

## 1. Contexto del Fork `Jetsku/KytyPS5`

El repositorio [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5) es uno de los forks experimentales más populares de la comunidad de emulación de PlayStation 5. Su desarrollo reciente se centra en la serie de compilaciones **U59** (destacando las revisiones `int21` e `int22`), que incorporan optimizaciones de draw calls, perfiles PGO (Profile-Guided Optimization) y presets dedicados para títulos de primera línea como **Astro Bot** (`PPSA21567`).

---

## 2. Desglose de los Resultados de Rendimiento Publicados por Jetsku

En las notas de lanzamiento de la versión `int22` de Jetsku se reportan las siguientes cifras en Astro Bot:

| Escenario reportado por Jetsku | FPS reportados en int21 | FPS reportados en int22 | Ganancia atribuida |
|---|:---:|:---:|:---:|
| **Sky Garden** | ~50.5 FPS | ~55.2 FPS | +4.7 FPS (optimización de command processing) |
| **Sponge Level (Nivel de la Esponja)** | ~40.8 FPS | ~53.0 FPS | +12.2 FPS (`KYTY_READBACK_EAGER_WRITES=1`) |

### Auditoría Crítica de las Condiciones de Prueba:

1. **Ángulo de Cámara y Carga de Geometría en Sky Garden:**
   - En nuestras mediciones experimentales de la Etapa 6.1 y Etapa 7:
     - En **vista amplia** (mirando a todo el archipiélago y el horizonte): **20.57 FPS** (5.031 draws/frame).
     - En **vista hacia el suelo o laterales**: **52.68 FPS** (1.200 draws/frame).
   - El resultado de ~50–55 FPS en Sky Garden reportado por Jetsku corresponde a un ángulo de cámara restringido o centrado en el personaje/suelo, **NO a la vista amplia completa**.
2. **Impacto de Parches de Iluminación (`Disable GI Probes`):**
   - En la comunidad de Jetsku, una gran parte de los usuarios que reportan tasas cercanas a 60 FPS estables utilizan el parche `_Patches/PPSA21567.json` que desactiva Global Illumination y GI Probes. Esto elimina miles de dispatches de compute y cálculos de ray tracing BVH, reduciendo el draw overhead a menos de la mitad.
3. **El Cuello de Botella de la Esponja y los Fluidos:**
   - El nivel de la esponja realiza simulaciones físicas de fluidos donde la CPU guest lee constantemente búferes modificados por la GPU en cada frame. Jetsku solucionó este bloqueo introduciendo `KYTY_READBACK_EAGER_WRITES=1`.

---

## 3. Arquitectura de Presets de Jetsku: `u59-preset.json`

Jetsku introdujo un sistema de configuración basado en variables de entorno inyectadas mediante archivos JSON colocados junto al ejecutable:
- `u59-preset.json`: Configuración global del emulador.
- `u59-preset.<title_id>.json` (ej. `u59-preset.PPSA21567.json`): Ajustes específicos por título.

### Contenido real de `u59-preset.json`:
```json
{
  "KYTY_ALIAS_CPU_WRITE_DIAGNOSTICS": "0",
  "KYTY_BDA_INCREMENTAL_SYNC": "1",
  "KYTY_CAPTURE_SHADER_DETAILS": "0",
  "KYTY_CP_RECORDER": "1",
  "KYTY_CP_RECORDER_VERIFY": "0",
  "KYTY_CP_SEQ": "1",
  "KYTY_CP_SEQ_PREFETCH": "1",
  "KYTY_CP_SEQ_VERIFY": "0",
  "KYTY_DCC_GPU": "1",
  "KYTY_DCC_GPU_REFRESH": "1",
  "KYTY_DESCRIPTOR_OFFSET_AUDIT": "0",
  "KYTY_DIRTY_IMAGE_QUERY": "direct",
  "KYTY_DRAW_PREP_BINDINGS": "1",
  "KYTY_FAST_BACKING_TRANSFERS": "1",
  "KYTY_GPU_OCCLUSION": "0",
  "KYTY_GPU_OP_COUNTERS": "0",
  "KYTY_GPU_OP_PROFILE": "0",
  "KYTY_HANG_TRACE": "0",
  "KYTY_HANG_WATCHDOG": "auto",
  "KYTY_HOST_INPUT_MIN_PRESS_MS": "0",
  "KYTY_HOST_INPUT_ONLY": "0",
  "KYTY_IMAGE_CACHE_POLICY": "legacy",
  "KYTY_IMAGE_LOOKUP": "fast",
  "KYTY_LABEL_MODE": "split",
  "KYTY_LOD_STATS_MODE": "gpu",
  "KYTY_MESH_RESTART": "1",
  "KYTY_NATIVE_IMAGE_POOL": "1",
  "KYTY_PROFILE_AGGREGATES": "0"
}
```

---

## 4. Matriz Comparativa de Características Técnicas

A continuación se comparan las 17 áreas técnicas clave investigadas entre nuestro fork y Jetsku:

| Característica | Estado en nuestro fork | Estado en Jetsku | Comentario técnico |
|---|:---:|:---:|---|
| **1. u59-preset.json** | No existe | **Implementado** | En Jetsku se usa para inyectar variables de entorno en el launcher. |
| **2. Presets específicos Astro Bot** | Mediante flags CLI | **u59-preset.PPSA21567.json** | Jetsku activa `KYTY_READBACK_EAGER_WRITES=1`. |
| **3. Renderer non-tiled** | Soportado nativo | **Vía parche de juego** | El juego se parchea para evitar el lighting tiled de PS5. |
| **4. Global Illumination & GI Probes** | Emulación completa | **Desactivable vía parche** | Apagar GI elimina miles de dispatches de compute por frame. |
| **5. Ray Tracing y parches** | Stub / experimental | **KYTY_RT_SOFTWARE=1** | Software fallback para evitar device loss en instrucciones BVH. |
| **6. Shader Precompilation** | Caching en runtime | **Listas `.kyplst`** | Jetsku distribuye listas de pipelines para precalentar antes de jugar. |
| **7. Pipeline Cache** | **Avanzado (MRU, Key Cache)** | Estándar Vulkan + PGO | Nuestro fork tiene MRU fast-path y pipeline key hash cache. |
| **8. GPU Occlusion Queries** | Flag `--occlusion-queries` | `KYTY_GPU_OCCLUSION=0` | Desactivado por defecto en ambos para evitar sincronizaciones CPU. |
| **9. Command Processing (PM4)** | En hilo GPU | `KYTY_CP_SEQ=1`, `CP_PREFETCH` | Optimización de decodificación secuencial de paquetes PM4. |
| **10. Sincronización CPU-GPU** | **Timeline semaphores + Cadence** | `KYTY_READBACK_EAGER_WRITES` | Nuestro fork cuenta con instrumentación y Submit Cadence periódica. |
| **11. Descriptor Management** | **Descriptor Cache (Etapa 8.1)** | `KYTY_DRAW_PREP_BINDINGS` | Ambos implementan fast-paths para eludir re-bindings de descriptores. |
| **12. Stream Buffers** | **Ring buffer 64/256 MiB** | Estándar Kyty | Nuestro fork dispone de `--extended-stream-buffer` (256 MiB). |
| **13. Depth/Stencil Management** | **Validación exhaustiva 8.2** | Legacy tracking | Nuestro fork corrigió la regresión visual de `draw-state-dirty-check`. |
| **14. Resource Barriers** | Redundant Bind Elision | `KYTY_DIRTY_IMAGE_QUERY` | Ambos eliden barreras y bindings redundantes. |
| **15. Render Target Management** | Multi-target signature | Fast image lookup | Nuestro fork verifica con precisión todos los estados de depth clear. |
| **16. Memoria y VRAM** | VMA + Write tracking | `KYTY_FUNCTION_ARRAY_SHRINK` | Jetsku reduce el uso de VRAM achicando arrays locales de shaders. |
| **17. Variables de entorno** | Flags CLI explícitos | Variables `KYTY_*` | Nuestro fork prefiere flags CLI tipados y testeables con CTest. |

---

## 5. Conclusiones Principales

1. **Rendimiento Base Similar:** En escenas equivalentes (vista al suelo sin parches), ambos forks alcanzan **52–55 FPS**.
2. **Diferencia Fundamental en 60 FPS:** Las capturas comunitarias de 60 FPS en Astro Bot no se logran por una arquitectura milagrosa del emulador, sino por **parchear el juego para apagar Global Illumination y GI Probes**, lo cual alivia masivamente la GPU y la CPU guest.
3. **Optimizaciones Transferibles Identificadas:**
   - La técnica de `KYTY_READBACK_EAGER_WRITES` (descarga anticipada de páginas de fluidos).
   - Precompilación de listas de pipelines `.kyplst`.
   - Adopción de un preset formal para Astro Bot que encapsule los mejores flags sin requerir líneas de comando complejas.
