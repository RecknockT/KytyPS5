# KytyPS5 — Etapa 8.3: Auditoría de Precompilación y Caché de Shaders

## 1. Arquitectura de Shaders y Pipelines en KytyPS5

En la emulación de PlayStation 5 mediante Vulkan, la ruta desde los shaders originales de la GPU guest (AMD RDNA) hasta los binarios ejecutables en la GPU host (NVIDIA RTX 5080) atraviesa seis etapas bien diferenciadas:

```
[Guest Shader AMD RDNA] 
       │
       ▼ (1. Extracción y análisis de registros en CommandProcessor)
[ShaderRecompiler IR]
       │
       ▼ (2. Traducción a SPIR-V)
[SPIR-V Bytecode]
       │
       ▼ (3. vkCreateShaderModule)
[VkShaderModule]
       │
       ▼ (4. vkCreateGraphicsPipelines / vkCreateComputePipelines)
[VkPipeline Vulkan]
       │
       ▼ (5. Compilación del Driver Host NVIDIA)
[Machine Code GPU (RTX 5080)]
```

---

## 2. Niveles de Caché Persistente en nuestro Emulador

El sistema de caché de pipelines (`src/graphics/host_gpu/renderer/pipeline/`) implementa dos niveles de almacenamiento persistente en disco bajo la ruta:
`_PipelineCache/<title_id>/<app_version>/`

### Nivel 1: Caché de Shaders Recompilados (`shaders_<env_hash>.shaders`)
- Guarda la representación serializada de los shaders traducidos a SPIR-V junto con sus estructuras de recursos (bindings, push data layout, SRT runtime).
- Se genera y actualiza en segundo plano mediante un hilo de persistencia asíncrono (`m_store`).
- Es independiente del driver gráfico, pero se invalida si cambia la firma del entorno (`PipelineCacheEnvironmentHash`).

### Nivel 2: Caché Binaria del Driver Vulkan (`driver_<env_hash>.bin`)
- Almacena el blob binario nativo generado por el driver de la GPU mediante `vkGetPipelineCacheData` (`VkPipelineCache`).
- Al iniciar el emulador, este blob se pasa a `vkCreatePipelineCache`, permitiendo al driver de NVIDIA reutilizar la compilación de microcódigo de la GPU en lugar de re-optimizar el SPIR-V.
- Se guarda periódicamente cada 60 segundos (`DriverCacheSaveInterval`) y al cerrar limpiamente el emulador.

---

## 3. Comportamiento en Frío vs Caliente (Warm-up vs Runtime)

### Caché Caliente (Re-ejecución):
- Al arrancar el emulador, el constructor de `PipelineCache` invoca `WarmUp(shader_store)` (`pipelineSerialization.cpp:200`).
- Lee el archivo `shaders_<env_hash>.shaders`, precarga todos los módulos y compila los pipelines de forma concurrente en hilos de trabajo.
- **Resultado en Gameplay:** Cero tartamudeo (hitching) al ingresar a zonas previamente exploradas. La GPU dispone inmediatamente de los pipelines compilados.

### Caché Fría (Primera Ejecución o tras Borrado):
- Si no existe caché previa, el emulador compila los shaders en runtime.
- **Async Shader Compile (`--async-shader-compile`, activado por defecto):**
  - Los shaders se compilan en un pool de hilos de fondo (`Common::ParallelThreads()`).
  - Para evitar congelar el hilo principal de render, **los draw calls que dependen de un shader aún en compilación se omiten temporalmente** en ese frame (`NoteDrawSkippedForShaders()`).
  - Esto evita congelamientos de 1 a 2 segundos en CPU, a cambio de que ciertos objetos parpadeen o aparezcan con un frame de retraso durante la primera carga.
- **Modo Síncrono (`--no-async-shader-compile`):**
  - El hilo de render se detiene y espera a que la compilación finalice antes de continuar. Garantiza que ningún objeto falte en pantalla, pero produce micro-pausas (stutters) apreciables.

---

## 4. Comparación con las Listas `.kyplst` de Jetsku

En el fork de Jetsku, se introdujo una característica basada en archivos de lista de pipelines con extensión `.kyplst` junto con un botón **"Precompile Shaders"** en la GUI del launcher:
1. **Mecanismo de Jetsku:** El launcher lee la lista preempaquetada `.kyplst` y genera todos los pipelines Vulkan antes de arrancar el proceso del juego, eliminando los tirones en la primera partida de nuevos usuarios.
2. **Mecanismo de nuestro Fork:** La precarga está integrada de forma totalmente nativa en el motor a través de `WarmUp` y `VkPipelineCache`. No requiere un paso manual en la interfaz gráfica; cualquier sesión previa alimenta automáticamente las ejecuciones futuras.

---

## 5. Recomendaciones para Benchmarking

1. **Condición Obligatoria de Prueba:** Toda comparación de rendimiento A/B entre etapas debe realizarse siempre con la **caché de shaders en estado caliente** (habiendo transitado por la escena objetivo al menos una vez antes de registrar los 30 segundos de telemetría).
2. **Aislamiento de Stutter:** Si se desea evaluar la sobrecarga intrínseca del compilador frente a la del renderizador, deben compararse capturas explícitas con `--shader-cache` vs `--no-shader-cache`.
