# Reutilización de preparación de bindings

## Cambio

Primera optimización posterior a la comparación de transferencias: conservar entre dibujos el rango normalizado de cada descriptor de buffer y el sampler Vulkan resuelto. Activación: `--binding-reuse`; control: `--no-binding-reuse`. Apagada por defecto: la comparación en Astro no mostró mejora.

La caché anterior de `PrepareBindings` compara punteros, pero `ResetBindings` limpia esas claves al terminar el dibujo. Las nuevas claves son copias propias del contenido de los descriptores; no retienen punteros a snapshots ni runtimes de dibujos anteriores. También contemplan los metadatos que afectan al resultado: scalar/max_byte_extent y las políticas de sampler.

Los rangos de buffers que dependen de tablas de otras etapas o del mapa de memoria (sentinel y rangos de 1 GiB o mayores) siguen calculándose en cada operación. Una generación del índice de buffers cambia al registrar o retirar un buffer: cuando cambia, se vuelve a resolver el ID aunque el descriptor siga siendo idéntico. Esto incluye buffers reemplazados por merges y reutilización de slots.

No se conservan resultados de dirty state, vistas de imágenes ni decisiones de sincronización. `RebindBuffers`, `ObtainBuffer`, los writebacks y las barreras mantienen su ejecución. Los samplers viven en el SamplerCache del mismo RenderContext hasta su destrucción.

Los uploads de SRT aplanada y datos de shader pueden reutilizarse exclusivamente dentro del mismo tick de grabación y la misma generación del stream buffer. Se comparan los bytes actuales con una copia en RAM normal, limitada a 64 KiB por entrada; las tablas mayores usan la ruta anterior. Una reserva que envuelve el anillo invalida la generación antes de devolver memoria escribible; un submit también invalida por tick. Si el contenido cambia, se asigna memoria nueva: no se sobrescribe lo que podría consumir un dibujo anterior.

Se descartó comparar leyendo directamente el stream buffer: su memoria de subida puede ser write-combined. La primera medición aislada ABBA con un SRT de 16 KiB dio ~740 ns por preparación sin reutilización frente a ~271.000 ns leyendo esa memoria. El camino final compara en RAM normal; esas cifras pertenecen al prototipo rechazado, no a la implementación corregida.

## Medición

Los JSON/CSV de AuditProfiler guardan el flag y cinco contadores del intervalo de captura: `binding_range_reuses`, `binding_lookup_reuses`, `binding_sampler_reuses`, `binding_upload_reuses` y `binding_upload_reuse_bytes`. Los contadores de rangos y samplers se acumulan localmente antes de actualizar los contadores para evitar un incremento atómico por cada descriptor.

El script existente acepta `-BindingReuse` y deja explícito el flag negativo cuando se omite:

```powershell
.\stage8_4f_run.ps1 -Profile A -Transfers Baseline -Preview
.\stage8_4f_run.ps1 -Profile A -Transfers Baseline -BindingReuse -Preview
```

Para medir el juego, quitar `-Preview`, usar la misma cámara y separar la ventana de aterrizaje de la escena estable. Comparar tiempos por cuadro, p95/p99, draws y compilaciones; no convertir aciertos de caché en una supuesta ganancia de FPS. Esta optimización elimina una parte del trabajo repetido, no los ~12 ms completos de preparación de descriptores.

## Validación

Selector dirigido: `shader_recompiler_compute_tests --binding-reuse-only`, registrado como `binding_reuse` en CTest. Comprueba el camino apagado y encendido, snapshots distintos con contenido idéntico, reemplazo de buffers, actualización real de datos CPU hacia GPU, cambios de descriptor, cambios de extensión y modo scalar, rangos sentinel y descriptores nulos. También prueba uploads iguales/distintos e invalidación por submit y vuelta del anillo. Incluye una medición ABBA del trabajo de preparación, sin dibujos ni afirmaciones de FPS.

Los resultados de compilación y ejecución se registran bajo `_Build/windows/binding_reuse_*`. No se declara una ganancia de FPS antes de una captura A/B de juego.

### Resultado de esta implementación

Compilación Release de emulador y pruebas: correcta. Pasan `--binding-reuse-only`, `--push-constant-bank-only`, `--zero-stride-binding-only`, `audit_profiler` y los tres CTest adyacentes `stream_buffer_ring`, `transfer_prefetch`, `buffer_cache_hot_publish`. Los tests audit comprueban además los deltas de captura y la exportación JSON del flag y los contadores.

Última medición sintética ABBA, una sesión, 100.000 preparaciones por fase, un buffer, un sampler, SRT repetida de 16 KiB y datos de shader. Incluye preparación y rebind/upload; no emite dibujos:

| Reutilización | ns por preparación |
|---|---:|
| Apagada, A1 | 736,11 |
| Encendida, B1 | 161,60 |
| Encendida, B2 | 155,57 |
| Apagada, A2 | 734,70 |

Promedios de esas fases: 735,405 ns frente a 158,585 ns. Esto demuestra reducción de trabajo en ese caso repetitivo; no mide el rendimiento total del emulador, la frecuencia real de aciertos en Astro ni los FPS. No extrapolar el porcentaje al tiempo completo de preparación de descriptores.

La prueba existente de push constants detectó scratch que no se limpiaba en el fast path de la caché anterior. Se corrigió reinicializando GDS/flattened-SRT/shader-data/shared-memory antes de elegir la ruta rápida. Este arreglo de estado temporal se aplica también con la nueva optimización apagada, por lo que ambos lados de un A/B lo incluyen.

La prueba adicional `--image-rebind-only` conserva un fallo de `RenderExecutorStencilBindingDiscovery`: un binding de profundidad consume un pending HTile stencil clear. Se reprodujo tanto con el candidato como restaurando temporalmente `descriptors.cpp` a su contenido de HEAD, manteniendo el resto del árbol local. La prueba de push constants también falló con ese control y pasa con la corrección de scratch. Los logs de control están en `_Build/windows/binding_reuse_control/`; el candidato fue restaurado con verificación de hash y recompilado después. Esto no demuestra que el fallo HTile existiera antes de todos los cambios locales de 8.4F, solo que no fue introducido por la nueva implementación de descriptores.

Se completó una comparación ABBA en Sky Garden en un solo proceso: 16,43 FPS con la opción apagada frente a 15,98 FPS encendida (−2,7 % observado). No hubo compilaciones de shaders ni creación de pipelines durante esas capturas. La variación entre controles impide afirmar una regresión general, pero no hay evidencia para activarla. Sigue apagada por defecto. Ver [resultados y límites de la medición](binding_reuse_gameplay_validation.md) y [ejecución automática](astro_automatic_binding_benchmark.md).
