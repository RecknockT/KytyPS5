# Integración de mejoras de transferencias antes de probar el juego

Base: `14185ebd9feed76015ebf8760ffad391e3c3ef50`, con los cambios locales de 8.4F conservados. No se crearon ramas ni commits. Esta integración no abre el juego ni atribuye una mejora de FPS a pruebas sintéticas.

## Selección y procedencia

Se incorporan tres mejoras que encajan con las esperas de readback y el tráfico de memoria observados en el aterrizaje. Son adaptaciones a la gestión de propiedad, publicación y barreras de este repositorio; no una importación completa del renderizador de otro fork.

1. **Lecturas de búferes vecinos en una misma espera.** Inspirada en [Jetsku 06fa23d1](https://github.com/Jetsku/KytyPS5/commit/06fa23d11d19eb0efc578b1bcbe556765a6fb301), que atribuye la idea original a chenxiao07 `0a0f59ac6`. Aquí se recoge la parte GPU-dirty de otros búferes registrados dentro de la ventana, se reserva staging para todo el lote y se publica después de una única espera. Cada origen mantiene sus barreras de transferencia. Se evita recoger otra vez un búfer que ya entró por la tabla de lecturas calientes.
2. **Ventana de readback de 4 MiB.** Inspirada en [Jetsku eadfca89](https://github.com/Jetsku/KytyPS5/commit/eadfca89fd29c2fe95bc9307aed906d400b3a6a2), adaptación del trabajo de Senaxx `2559c2737`. Sustituye opcionalmente los 512 KiB anteriores. Sin la opción de vecinos sólo amplía la lectura dentro del búfer original. No utiliza la implementación asíncrona de side-readbacks del otro fork.
3. **Ventana de escritura anticipada de 256 KiB.** Aplica la estrategia de agrupar faltas y cambios de protección descrita en [las notas de Jetsku](https://github.com/Jetsku/KytyPS5/blob/u59-windows-20261118-int22/docs/EXPERIMENTAL.md) al manejador existente de 64 KiB. Sigue limitado al búfer registrado, región del tracker y memoria mapeada. No se importan sus heurísticas adaptativas ni reglas de prioridad. Es una ampliación del mecanismo local, no un port textual de su código.

## Opciones y valores predeterminados

| Mejora | Activar | Desactivar | Predeterminado |
|---|---|---|---|
| Vecinos | `--readback-neighbours` | `--no-readback-neighbours` | Desactivada |
| Ventana de 4 MiB | `--wide-readback-window` | `--no-wide-readback-window` | Desactivada, 512 KiB |
| Escrituras anticipadas | `--fault-ahead` | `--no-fault-ahead` | Desactivada, 64 KiB |
| Agrupamiento caliente existente | `--batch-hot-readbacks` | `--no-batch-hot-readbacks` | Desactivado |

Las opciones no dependen de un Title ID y se pueden probar con cualquier juego. Quedan desactivadas hasta medirlas en gameplay: las ventanas mayores pueden copiar o volver a subir más datos y empeorar algunas cargas. No se cambiaron CP Sequencer, DrawPrep ni Draw State Dirty Check: siguen desactivados por defecto.

El agrupamiento de vecinos inspecciona como máximo 64 entradas del mapa por operación y comparte el presupuesto de 4 MiB de descargas adicionales con las lecturas calientes. Sólo recoge intervalos alineados a páginas del tracker, mapeados y sin una publicación pendiente de texturas. No crea búferes para los huecos, no elimina barreras, no limpia páginas ajenas a los intervalos publicados y no cambia relojes guest, VBlank, audio ni calidad de imagen.

## Diagnóstico y preparación de las pruebas posteriores

El JSON y CSV del profiler registran las tres opciones. Se añadieron `neighbour_readback_count` y `neighbour_readback_bytes` al JSON, y `Transfers,NeighbourReadbackCount` / `NeighbourReadbackBytes` al CSV. Son diferencias desde el inicio de cada captura, no acumulados de sesiones anteriores. El contador de bytes refleja staging reservado para las descargas de vecinos, incluido su alineamiento.

`stage8_4f_run.ps1` permite seleccionar por separado el modo de transferencias, manteniendo A/B/C para el secuenciador:

```powershell
# Estos comandos son para la prueba posterior; no fueron ejecutados durante esta integración.
.\stage8_4f_run.ps1 -Profile A -Transfers Baseline -CaptureSeconds 120
.\stage8_4f_run.ps1 -Profile A -Transfers Neighbours -CaptureSeconds 120
.\stage8_4f_run.ps1 -Profile A -Transfers Wide -CaptureSeconds 120
.\stage8_4f_run.ps1 -Profile A -Transfers FaultAhead -CaptureSeconds 120
.\stage8_4f_run.ps1 -Profile A -Transfers Combined -CaptureSeconds 120
```

`Hot` permite evaluar sólo el agrupamiento previo; `Combined` activa las tres incorporaciones y ese agrupamiento. Cada ejecución pasa explícitamente las opciones negativas antes de las positivas para que el control sea reproducible, y guarda `transfers` en run.json. Comparar primero cada opción por separado con la misma cámara, calidad, juego, caché caliente y recorrido.

Agregar `-Preview` muestra los argumentos sin abrir el juego, crear una carpeta de ejecución ni cambiar variables de entorno. Los seis modos se comprobaron así y no generan argumentos vacíos.

## Validación

Se añadió `transfer_prefetch` a CTest, ejecutable con `shader_recompiler_compute_tests --transfer-prefetch-only`. Usa el backend Vulkan real, rellenos GPU y lectura del backing guest; no es una simulación de un contenedor.

- Combinaciones de vecinos activados/desactivados y agrupamiento caliente activado/desactivado.
- Publicación de búferes cercanos y separados por huecos, integridad de bytes limpios y conservación de las páginas fuera de la ventana.
- Una única submission para el lote y ninguna para volver a leer datos ya publicados.
- Detección y publicación de una escritura GPU posterior a la precarga.
- Diferencia efectiva entre 512 KiB y 4 MiB dentro de un búfer grande.
- Paso por el manejador de faltas de escritura y comprobación de límites de búfer/región del tracker.
- Prueba del profiler para configuración y deltas de contadores entre dos capturas.

Resultados finales:

- Compilación Release del emulador, fixture Vulkan y pruebas de profiler/memoria/CP/DrawPrep: aprobada, log `transfer_prefetch_final_build.log`.
- 12/12 pruebas dirigidas aprobadas: transfer_prefetch, audit_profiler, cp_sequencer, draw_prep, memory_tracker, page_manager, stream_buffer_ring, tres de submit cadence, buffer_cache_hot_publish y hot_readback_table. Log `transfer_prefetch_final_ctest.log` (7,31 s).
- `--shader-cache-only`: salida 0, incluidos ShaderStore, regresión MRU y VertexPushTopCache. Log `transfer_prefetch_final_shader_cache.log`.
- Flags CLI positivos/negativos reconocidos; sintaxis PowerShell y `git diff --check` aprobados. Se verificó también el JSON exportado por el profiler y sus contadores entre capturas.
- No se abrió Astro Bot ni otro juego. Los procesos de compilación y pruebas finalizaron.

La ampliación inicial de regresiones detectó dos fallos: `buffer_cache_dirty_gc` en `protected-read bytes`, y `buffer_cache_ranges` en `layer growth with fewer levels`. Se recompiló un control reemplazando los nueve archivos de implementación/cabeceras/fixture afectados por sus versiones de HEAD, conservando el resto del trabajo 8.4F. **Ambos fallos se reprodujeron en ese control con los mismos mensajes**. Logs: `_Build/windows/transfer_prefetch_control/{build,ctest}.log`. Por tanto, no se presenta una suite global sin fallos ni se atribuyen esas dos averías al lote nuevo. La comparación tampoco determina si provienen de 8.4F o de una revisión anterior: eso queda por investigar.

Los nueve archivos candidatos se restauraron byte por byte, se verificaron contra sus copias y se recompiló la entrega final antes de repetir las pruebas aprobadas. El primer intento de la prueba nueva suponía erróneamente que una lectura pequeña por staging limpiaba el tracker CPU; el fixture se corrigió para establecer ese estado mediante un relleno y readback GPU real, antes de comprobar integridad de memoria.

Los logs finales quedan en `_Build/windows/transfer_prefetch_*.log`. Posteriormente se realizó la [prueba real de gameplay](transfer_prefetch_gameplay_validation.md): el paquete combinado no mostró una mejora apreciable de FPS en Sky Garden y aumentó el volumen descargado. Las mejoras permanecen desactivadas de forma predeterminada.

## Mejoras examinadas que no se importan en este lote

La corrección de prioridades de Windows que usa el límite 460 ya está presente. Los kernels de commit, reutilización de imágenes y generación específica de shaders NVIDIA de U59 dependen de su propio renderizador, sus verificadores y huellas de compilación. Incorporarlos como parches sueltos sin esas dependencias no permite preservar coherencia ni afirmar que funcionen en RTX 5080. PGO requiere generar un perfil con este código, no copiar el perfil de otro ejecutable. La atención de eventos de la ventana durante la preparación de shaders sigue siendo una corrección aparte.

No se incluyen cambios de resolución/iluminación, generación de cuadros ni modificaciones de velocidad del juego para alcanzar un contador de 60 FPS.
