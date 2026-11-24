# Diagnóstico de cargas por shader

`--effect-profile` activa un diagnóstico opcional; `--no-effect-profile` conserva la ruta habitual y es el valor predeterminado. No desactiva agua, geometría, iluminación ni simulación. `--no-gpu-timestamps` permite recoger solo tiempos del host; las consultas GPU requieren además que la familia de cola soporte timestamps.

La herramienta anterior declaraba opciones para timestamps y omisión de agua, pero no tenía llamadas que midieran las ejecuciones GPU. Este diagnóstico usa un archivo separado, `effects.jsonl`, junto al informe de AuditProfiler. Los campos antiguos del overlay no se rellenan con estos intervalos: no confundirlos con una medición de ocupación GPU.

## Recogida

- Agrupa dibujos por hashes de programa de vértices y píxeles y máscara de destinos de color. Agrupa compute por hash y tipo directo/indirecto. Un hash no identifica por sí solo agua, esponjas o iluminación.
- Mide tiempo del host de preparación y emisión en las operaciones identificadas. Incluye esperas ocurridas dentro de esos ámbitos; no es tiempo CPU exclusivo. Las rutas que retornan antes de identificar un programa no se incluyen.
- Recoge intervalos GPU de submissions y muestras de dibujos (probabilidad 1/64, selección pseudoaleatoria) y de compute (todos hasta el límite del lote). Las sustituciones de compute por rellenos/copias pueden tener tiempo host sin muestra de dispatch.
- Los timestamps de muestras usan el mismo extremo de pipeline antes y después del comando. Son intervalos observados bajo esta instrumentación; pueden incluir solapamiento y trabajo de otros comandos. No equivalen a ejecución aislada del shader y no deben sumarse ni extrapolarse como ocupación GPU.
- Los intervalos completos de submissions tampoco son ocupación GPU: pueden incluir burbujas y solaparse con otros lotes.

Las consultas se resetean fuera de rendering al comenzar un command buffer y se leen solo cuando su timeline ya ha terminado, sin `WAIT` ni submits adicionales para medir. Hay 32 pools reutilizables, 128 muestras por lote y 4096 grupos como máximo. Si se agotan, se omiten muestras y se informa en `dropped`. Se respeta `timestampValidBits`, incluida su vuelta de contador, y `timestampPeriod`. La semántica de timestamps está descrita en la [especificación Vulkan](https://docs.vulkan.org/spec/latest/chapters/queries.html).

El propietario es exclusivamente el hilo de render. El scheduler de presentación no abre otro archivo ni reutiliza estos pools. En el cierre normal se esperan las operaciones propias ya previstas por el scheduler, se recogen resultados pendientes y después se destruyen los pools.

Se exporta y vacía un registro acumulado cada 120 cuadros y al cerrar. Si el proceso falla, permanecen los registros completos anteriores; se puede perder el intervalo aún no exportado. El análisis usa diferencias entre registros, no acumula de nuevo todos sus totales.

## Prueba automática

```powershell
.\stage8_effect_profile_auto.ps1
```

Usa la misma navegación de Sky Garden y mantiene X tres segundos para iniciar el descenso. Captura el descenso y una ventana fija; termina a los 320 segundos del replay. La navegación depende de la partida, de la posición del mapa y de que las transiciones hayan terminado. Una ejecución que entre a otro nivel no sirve para comparar Sky Garden. El wrapper exige captura completa y una carga de dibujos compatible con la escena pesada, y confirma el nombre `aerial_garden` en el log; eso no sustituye comprobar la imagen.

`tools/benchmarks/analyze_effect_profile.py <directorio>` calcula diferencias sobre los últimos aproximadamente 600 cuadros. Es un diagnóstico instrumentado, no un benchmark para afirmar ganancias de FPS. No mezclar sus resultados con capturas de otro ejecutable para atribuir un cambio a una optimización.

## Pruebas dirigidas

El CTest `effect_profile` usa Vulkan real, 80 lotes con rellenos GPU, lectura de resultados y reutilización de pools. Comprueba contabilidad host, muestras GPU, ausencia de pérdidas en esa carga y resta modular de timestamps de 32 y 64 bits. Las pruebas adyacentes `audit_profiler`, `binding_reuse` y `stream_buffer_ring` también deben pasar.

Los tiempos de readback por shader son un subconjunto del tiempo host de esa operaci�n, no un coste adicional. Se asignan al �mbito de dibujo/compute activo en el hilo de render; las lecturas de otros hilos fuera de ese �mbito no se atribuyen a un shader.
