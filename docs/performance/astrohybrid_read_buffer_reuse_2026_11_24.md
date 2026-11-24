# Primera adaptaciÃƒÆ’Ã‚Â³n de AstroHybrid: buffers de lectura

## Alcance y procedencia

Referencia fijada: AstroHybrid v1.2.19, `Source/hybrid/patches/0015-hybrid-commit-epoch-read-memo.patch`:
https://github.com/imnotsharingmyname/pcportastrobot/blob/v1.2.19/Source/hybrid/patches/0015-hybrid-commit-epoch-read-memo.patch

Se adapta la idea de una prueba de memoria limpia; no se aplica literalmente el parche ni su infraestructura de buffers escritos de 0014. Esa infraestructura depende de interfaces del fork que esta base no contiene.

| Componente | Estado local / decisiÃƒÆ’Ã‚Â³n |
|---|---|
| PreparaciÃƒÆ’Ã‚Â³n y cachÃƒÆ’Ã‚Â© persistente de shaders | Ya presentes; no sustituir ni importar cachÃƒÆ’Ã‚Â©s binarias de otro motor. |
| Memo de uploads por generaciÃƒÆ’Ã‚Â³n | Ya existe en MemoryTracker::ForEachUploadRange. Evita duplicarlo. |
| ReutilizaciÃƒÆ’Ã‚Â³n de un buffer de lectura ya sincronizado | Nuevo candidato: evita bÃƒÆ’Ã‚Âºsquedas y preparaciÃƒÆ’Ã‚Â³n alrededor del memo de uploads. |
| Atajo para buffers escritos | Excluido: requiere auditorÃƒÆ’Ã‚Â­a independiente de ownership y coherencia. |
| TemporizaciÃƒÆ’Ã‚Â³n, real-time game-step y frame generation | Excluidos de esta adaptaciÃƒÆ’Ã‚Â³n. No son mejoras equivalentes del renderizador. |
| IluminaciÃƒÆ’Ã‚Â³n reducida / NonRT | No se importa ni se utiliza para atribuir una mejora de Kyty. |
| Cola de presentaciÃƒÆ’Ã‚Â³n y agrupaciÃƒÆ’Ã‚Â³n de pases de Jetsku #107 | PrÃƒÆ’Ã‚Â³ximo candidato independiente; no mezclado en este ensayo. |

## ImplementaciÃƒÆ’Ã‚Â³n

`--read-buffer-reuse` / `--no-read-buffer-reuse`, apagado por defecto. Tabla de 4096 entradas perteneciente a BufferCache, utilizada exclusivamente por el hilo GPU. Cada entrada contiene direcciÃƒÆ’Ã‚Â³n, tamaÃƒÆ’Ã‚Â±o, ID del buffer, ÃƒÆ’Ã‚Â©poca de registro y generaciÃƒÆ’Ã‚Â³n de memoria. No conserva punteros a allocations transitorias.

Solo se admiten lecturas no texel dentro de una regiÃƒÆ’Ã‚Â³n de tracking de 4 MiB. DespuÃƒÆ’Ã‚Â©s de la sincronizaciÃƒÆ’Ã‚Â³n normal se establece una prueba CPU-clean bajo el lock de la regiÃƒÆ’Ã‚Â³n. Es necesario tomar ese lock: la generaciÃƒÆ’Ã‚Â³n se incrementa antes de modificar bits/protecciones; una consulta lock-free no puede establecer la prueba inicial de forma segura. La comprobaciÃƒÆ’Ã‚Â³n de una prueba ya establecida lee la generaciÃƒÆ’Ã‚Â³n atÃƒÆ’Ã‚Â³micamente.

Cambios de CPU, cambios de ownership GPU y unmaps invalidan la generaciÃƒÆ’Ã‚Â³n. Registro, retirada y merges de buffers invalidan la ÃƒÆ’Ã‚Â©poca. Un acierto conserva TouchBuffer/LRU y devuelve el mismo buffer persistente y offset. Un fallo ejecuta la ruta completa. Las lecturas texel/imÃƒÆ’Ã‚Â¡genes, escrituras, rangos entre regiones y allocations del stream mantienen la ruta normal. No cambia relojes, calidad, shaders ni barreras necesarias.

Se exportan ReadBufferHits/Misses como deltas por captura JSON/CSV. Cambiar el modo mediante replay solo se aplica al inicio de una operaciÃƒÆ’Ã‚Â³n GPU. Los aciertos no equivalen a FPS ganados.

## ValidaciÃƒÆ’Ã‚Â³n y reproducciÃƒÆ’Ã‚Â³n

Pruebas nuevas: `memory_tracker` (prueba limpia, invalidaciÃƒÆ’Ã‚Â³n CPU/GPU, fallback entre regiones) y `read_buffer_reuse` (Vulkan, datos despuÃƒÆ’Ã‚Â©s de escrituras CPU/GPU, ID retirado por merge, remapeo, exclusiÃƒÆ’Ã‚Â³n de texel/escritura/stream), comparando opciÃƒÆ’Ã‚Â³n apagada y encendida.

CompilaciÃƒÆ’Ã‚Â³n y resultados se registrarÃƒÆ’Ã‚Â¡n al terminar en este documento. AÃƒÆ’Ã‚Âºn no hay resultado de rendimiento del juego para este candidato.

MediciÃƒÆ’Ã‚Â³n manual conservando joystick y configuraciÃƒÆ’Ã‚Â³n del launcher:

```powershell
./stage8_live_play.ps1 -ManualAB -ABMode read_buffer_reuse
```

Espera confirmaciÃƒÆ’Ã‚Â³n del usuario en la escena estÃƒÆ’Ã‚Â¡tica despuÃƒÆ’Ã‚Â©s del aterrizaje antes de crear `start_measurement` en el directorio de esa ejecuciÃƒÆ’Ã‚Â³n. Capturas de 40 segundos en orden ABBA; no se inyectan controles ni cierre. Analizar con:

```powershell
python tools/benchmarks/compare_mode_benchmark.py <directorio> read_buffer_reuse
```

El analizador conserva la exigencia de cero shaders/pipelines nuevos y configuraciÃƒÆ’Ã‚Â³n idÃƒÆ’Ã‚Â©ntica excepto este modo. No comparar los FPS de AstroHybrid con ajustes de iluminaciÃƒÆ’Ã‚Â³n, resoluciÃƒÆ’Ã‚Â³n o temporizaciÃƒÆ’Ã‚Â³n distintos.

## SesiÃ³n manual preparada

Abierta la compilaciÃ³n nueva, PID 16936, inicio 2026-11-24 09:07:32 -03:00. Directorio `_Build/windows/live_play/20261124_090732_480`. Proceso verificado con ruta, hora de inicio y Responding=true. Conserva resoluciÃ³n 2560x1440, cadencia activada, joystick del usuario y los demÃ¡s ajustes del launcher. El replay contiene solo cambios de modo y capturas; espera el archivo de armado que aÃºn no se creÃ³. No hay mediciones de FPS del candidato todavÃ­a.

Este arranque recuperÃ³ 27,63 MB de cachÃ© del driver y completÃ³ la preparaciÃ³n de 1046 entradas: 461 programas, 508 mÃ³dulos y 538 pipelines. No implica que no puedan aparecer variantes nuevas durante la partida.

El juego queda abierto para el usuario; las compilaciones, pruebas y microbenchmark terminaron y no quedan helpers temporales de esta tarea. La optimizaciÃ³n sigue apagada y no se modificÃ³ la configuraciÃ³n persistente del launcher.

## Pruebas autónomas: navegación y carga

El usuario autorizó realizar las pruebas sin su intervención. Se añadió `stage8_read_buffer_auto.ps1` y `tools/benchmarks/astro_read_buffer_replay.json`. Ejecutar `./stage8_read_buffer_auto.ps1`.

La secuencia incluye título, confirmación de partida, movimiento al sistema, selección del planeta y X mantenida cinco segundos. `wait_file` congela el tiempo del replay hasta que el supervisor confirma en el log el mapa, la carga de la galaxia o el inicio de Sky Garden. El modo automático puede reintentar X durante cinco segundos cada veinte segundos en una espera de confirmación; se suelta al confirmar. El modo manual rechaza estos reintentos igual que rechaza controles y quit. Cada espera sin confirmación se cancela a los 180 segundos desde que el supervisor la observa. El supervisor mantiene el límite total de ejecución y el cierre de sus procesos.

La captura `galaxy_load` precede al gate de galaxia; `landing` precede a la espera estacionaria. No se envían controles durante el A/B. El análisis sigue exigiendo escena pesada, inicio confirmado, cuatro muestras completas y cero shaders/pipelines nuevos.

Se hicieron cuatro intentos; **ninguno produjo un A/B válido ni confirmó el aterrizaje**. No afirmar que la automatización ya completó exitosamente toda la ruta. Los primeros tiempos fijos enviaban botones durante la carga. Los gates sí confirmaron mapa y carga de galaxia en el último intento, pero el de inicio de Sky Garden no se abrió pese a reintentos. Las capturas de ventana mostraron negro. Se probó también con compilación síncrona: no restableció una imagen verificable. La compilación anterior guardada del 23/11 presentó el mismo síntoma de captura negra después de entrar al título; no demuestra que el problema sea causado por esta optimización, ni identifica aún su causa.

Último intento: `_Build/windows/stage8_4f_runs/20261124_102626_792_A`, PID 15648, SHA256 `A9131B92DE94209EB08E5BFAE52C0FD5880D1ED39BF85E13A2424BF291030A5E`. Se preserva `invalid_run.json`. El candidato nunca se encendió. No existen cuatro capturas estáticas completas de esta opción.

### Hallazgo útil de la carga

`galaxy_load.json` y `galaxy_load_frames.csv` terminaron completos: 40,002 segundos, 1316 cuadros, **cero shaders y cero pipelines compilados**. Se contabilizaron 143.697,25 MiB de descargas GPU→CPU, 109,19 MiB/cuadro y 3592,25 MiB/s. Son contadores agregados de transferencias, no lecturas físicas del SSD ni una atribución a una textura concreta. Hay además 10,37 segundos acumulados por hilos en esperas de stream y 2,82 segundos en BufferReadback; no sumar esperas superpuestas ni presentarlas como fracciones exclusivas de la carga. CPU del hilo gráfico: 21,61 segundos durante la ventana de 40 segundos. No se midió duración GPU con timestamps en esta captura.

El juego informó `hub_crashsite : 32.778 sec.`. Entre los archivos de confirmación del mapa y la galaxia transcurrieron 52.059 segundos de reloj host; ese intervalo incluye navegación y otras cargas. **El valor de 32,8 no demuestra que la carga host haya bajado desde 50 segundos.** No se atribuye a una optimización ni se confunde con el descenso de Sky Garden.

Esto justifica estudiar el volumen y las repeticiones de readbacks de buffers/imágenes durante la carga. `TrackImageDownload` y `ProcessDownloadImages` incluyen la ruta opt-in de imágenes lineales, pero el contador agregado no basta para responsabilizarla de todos los bytes ni para desactivarla: puede ser necesaria para datos que consume la CPU guest. Hace falta preservar coherencia y comprobar el consumidor antes de eliminar trabajo.

Evidencia resumida: `load_analysis.json` en el último directorio de ejecución. La automatización y los cambios se compilaron; las siete pruebas de corrección volvieron a pasar (`_Build/windows/read_buffer_gates_tests.log`). Se verificó la salida de todos los procesos de emulación iniciados para estas pruebas; se cerró también el control anterior. No se cambió la configuración persistente ni se borraron cachés o partidas.
