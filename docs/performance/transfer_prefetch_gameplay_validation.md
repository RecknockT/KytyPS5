# Prueba real de transferencias en Astro Bot

Fecha: 23 de noviembre de 2026. PPSA21567, versión 01.019.000. Ryzen 7 9700X, RTX 5080. Base `14185ebd9feed76015ebf8760ffad391e3c3ef50`, con cambios locales. Hash SHA256 del ejecutable en ambas ejecuciones: `FC279ADEFBC6422A5086BFDC0ABDC79D0BEA96ADFE737A0E916DDD8FFE8339C6`.

## Resultado

**El paquete combinado no produjo una mejora apreciable de FPS ni resolvió la lentitud del aterrizaje en Sky Garden.** Redujo las faltas de página y el número de readbacks, pero descargó aproximadamente 15,7 veces más bytes desde la GPU por cuadro. Las tres opciones nuevas permanecen desactivadas por defecto.

Ambas ejecuciones completaron el descenso y llegaron a gameplay. No se observaron bloqueos ni corrupción visible en las escenas inspeccionadas; no se hizo una comparación de imagen píxel a píxel ni se validaron otros niveles.

## Método

- Control: perfil A, `Transfers Baseline`, opciones de transferencias desactivadas.
- Candidato: perfil A, `Transfers Combined`, vecinos, ventana de 4 MiB, fault-ahead de 256 KiB y agrupamiento caliente previo activados.
- CP Sequencer, DrawPrep y Draw State Dirty Check desactivados en ambos.
- Mismo ejecutable, directorio de juego y opciones restantes del profiler, verificadas contra los JSON. No se cambiaron relojes, VBlank, calidad ni opciones de resolución.
- Se recorrió la entrada al mismo nivel. Después se dejó al personaje quieto, con la cámara inicial orientada hacia el nivel y el panel visible, tomando dos ventanas por condición. Las animaciones y aves siguen moviéndose.
- No hubo compilaciones de shaders ni creación de pipelines durante las cuatro ventanas fijas. La preparación inicial de shaders quedó fuera de esas ventanas.
- Cada captura tiene JSON, CSV y serie por cuadro propios. Los tiempos reales varían por la intervención manual; los FPS agregados se calculan como cuadros totales divididos por segundos reales totales. P95 se obtiene de las series concatenadas, no promediando percentiles.

## Vista fija: resultados medidos

| Métrica | Control | Combinado |
|---|---:|---:|
| FPS, muestra 1 | 16,059 | 16,117 |
| FPS, muestra 2 | 15,862 | 15,952 |
| Cuadros / tiempo real agregado | 1394 / 87,384 s | 1329 / 82,877 s |
| FPS efectivos agregados | **15,953** | **16,036** |
| P95 de frametime | 67,718 ms | 67,377 ms |
| Dibujos por cuadro | 4957,98 | 4952,50 |
| Faltas de página por cuadro | 1268,82 | 616,42 |
| Readbacks con espera por cuadro | 8,132 | 6,136 |
| Latencia media de cada espera de readback | 1976,29 us | 2543,54 us |
| Espera acumulada de readback por cuadro | 16,071 ms | 15,608 ms |
| Descargas GPU → CPU por cuadro | 0,574 MiB | 9,009 MiB |
| Subidas CPU → GPU por cuadro | 60,717 MiB | 61,585 MiB |
| Preparación de descriptores por cuadro | 11,964 ms | 11,621 ms |
| Preparación de estado por cuadro | 25,872 ms | 25,619 ms |
| PM4 dispatch exclusivo reportado por cuadro | 13,679 ms | 14,727 ms |
| Descargas de vecinos en las dos ventanas | 0 | 4024 |
| Shaders / pipelines nuevos | 0 / 0 | 0 / 0 |

La diferencia nominal de FPS es aproximadamente +0,52 %, insuficiente para afirmar una ganancia con estas muestras y su variación. Las faltas de seguimiento bajaron aproximadamente 51,4 % y los readbacks con espera 24,5 %; el volumen descargado creció aproximadamente 15,7 veces. El ahorro de llamadas/esperas no se tradujo en una mayor fluidez. Esta observación no atribuye el efecto a una opción individual: Combined también activa el agrupamiento caliente ya existente.

Los tiempos de etapas son anidados y las esperas acumulan hilos concurrentes. No deben sumarse para reconstruir un cuadro. Los timestamps Vulkan no se registran en esta build, por lo que no se puede concluir que toda espera sea ejecución física de GPU ni descartar una limitación de GPU en otras escenas.

## Descenso y diagnóstico

Se conservaron las dos capturas de descenso, pero sus comienzos no son equivalentes: la del control incluye parte del túnel previo y la del candidato empieza con la nave ya visible sobre el nivel. **No comparar sus FPS medios como un A/B ni usarlos para afirmar una mejora/regresión de tiempo de carga.** El contenido de la vista fija, con un número medio de dibujos prácticamente igual, es la comparación principal.

El control exportó 96,653 s y 1742 cuadros, con 8864 esperas de readback y un peor cuadro de aproximadamente 1,8 s. La captura candidata exportó 98,114 s y 1368 cuadros, con 5153 descargas de vecinos. Ambas registraron cero shaders/pipelines nuevos. En las inspecciones visuales el descenso seguía siendo lento con las opciones activadas; no hubo una solución visible del problema.

El clasificador de los hitches mayores los etiquetó con confianza Low: esas etiquetas no prueban que la GPU física sea la causa. La memoria comprometida del sistema estuvo próxima a su límite durante partes de las ejecuciones. En esta comparación no se tomaron contadores de paginación física; no atribuir los picos al SSD o al archivo de paginación sin una medición adicional.

## Interpretación y decisión

Se conserva el modo anterior como predeterminado. Las nuevas rutas quedan disponibles para pruebas aisladas y otros juegos, pero **Combined no se recomienda como preset de rendimiento de Sky Garden con esta evidencia**. No se retiraron comprobaciones de coherencia ni se alteró la velocidad del juego para esconder la lentitud.

El siguiente trabajo debe separar las opciones para identificar cuál produce el tráfico adicional y medir costes concretos de preparación/registro de los aproximadamente 5000 dibujos por cuadro. Los aproximadamente 12 ms de descriptores persisten, y se necesita una lectura de tiempos GPU real para completar la atribución. Estas dos ejecuciones no establecen estabilidad prolongada, equivalencia visual completa ni resultados de todos los niveles.

## Artefactos y cierre

En `_Build/windows/stage8_4f_runs/`:

- `20261123_070343_679_A`: control, PID 28068, cierre normal con salida 0.
- `20261123_071230_733_A`: Combined, PID 26408, cierre normal con salida 0.
- Cada directorio contiene `landing_snapshot.*`, `static1_snapshot.*`, `static2_snapshot.*`, sus series `_frames.csv`, stdout/stderr, muestras de CPU/memoria/I/O y run.json con parámetros y hash.

Resumen procesado: `_Build/windows/transfer_gameplay_samples.json`, `transfer_gameplay_comparison.json` y `transfer_gameplay_comparison.csv`. Las dos ventanas fijas se guardaron con identificadores de captura distintos y número de filas coincidente con sus resúmenes.

El script de pruebas permite ahora `-MaxRuntimeSeconds` para limitar una ejecución manual sin incluir la precarga en la duración de captura. Ambos procesos terminaron y la inspección final de ventanas confirmó que no queda un juego abierto. No se hicieron commits ni push. Se conservaron los archivos de guardado que el juego actualizó.
