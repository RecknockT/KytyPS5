# OPT-12 â€” Desglose PM4 por opcode

Estado: implementado y validado; pendiente de capturas reales en Sky Garden. Es instrumentaciÃ³n, no una optimizaciÃ³n de FPS.

## ActivaciÃ³n

`--profile-pm4` / `--no-profile-pm4`, apagado por defecto. Registra durante capturas de AuditProfiler. Los resultados aparecen en `pm4_opcode_profile` del JSON y en filas PM4 del CSV. No escribe una lÃ­nea por paquete.

Cada bucket corresponde al opcode de 8 bits de un handler invocado. Las llamadas incluyen reintentos de handlers suspendidos; no equivalen necesariamente a paquetes completados. Los NOP personalizados comparten el bucket IT_NOP. Los paquetes omitidos antes de invocar el handler no forman parte de este desglose.

Se exportan llamadas, llamadas/cuadro, duraciÃ³n inclusiva, uniÃ³n de intervalos hijos excluidos, duraciÃ³n exclusiva, uniÃ³n de esperas host y mÃ¡ximo inclusivo. El analizador agrega las capturas y ordena por tiempo exclusivo total, mostrando tambiÃ©n costo por llamada.

## Contabilidad y lÃ­mites

La atribuciÃ³n se mantiene en el hilo del handler. ScopedStageTimer marca los intervalos hijos; tambiÃ©n se cubren las esperas de MasterSemaphore, DrainPriorityOperations, WaitPriorityOperations y Submit. Los scopes anidados se descuentan por uniÃ³n, no sumando sus duraciones. Un handler PM4 anidado se excluye del padre.

El exclusivo es tiempo de pared del host restante despuÃ©s de esos scopes instrumentados. Puede contener trabajo/esperas sin instrumentar, desalojo del hilo y costo del profiler. No es CPU exclusiva del sistema operativo ni tiempo fÃ­sico de GPU. Las esperas host son un subconjunto de lo excluido y no deben restarse otra vez. Los inclusivos de handlers anidados se solapan. La definiciÃ³n nueva no equivale al antiguo exclusivo global calculado por diferencia de etapas.

Un mutex protege los buckets y sus reinicios; un identificador de captura evita incorporar scopes viejos en la siguiente captura. Los scopes incompletos o incoherentes se descartan. El contador de descartes refleja lo observado al congelar la captura. No se cambian comandos, coherencia de memoria, barreras, relojes guest, VBlank ni audio.

## ComparaciÃ³n manual en un proceso

```powershell
.\stage8_live_play.ps1 -ManualAB -ABMode profile_pm4 -FrameLimit 0
```

El usuario controla el joystick. La secuencia no contiene botones ni cierre. Espera el archivo `start_measurement`, creado Ãºnicamente despuÃ©s de confirmar personaje y cÃ¡mara quietos en Sky Garden. Alterna OFF/ON tres veces: capturas de 30 s, separaciÃ³n de 5 s y cambio de modo 3 s antes de cada captura. Los cambios se aplican al iniciar la siguiente captura, conservando una configuraciÃ³n estable durante ella. El diagnÃ³stico se desactiva al terminar la secuencia.

Sin effect-profile, live diagnostics ni timestamps GPU; las demÃ¡s opciones y resoluciÃ³n permanecen iguales entre las seis muestras. El lÃ­mite FPS se desactiva para observar el costo aÃ±adido. La secuencia espera capturas pendientes en lugar de cerrar el emulador si un cuadro tarda en llegar.

```powershell
python tools/benchmarks/analyze_pm4_profile.py _Build/windows/live_play/20261125_000540_496
```

El anÃ¡lisis exige capturas completas, configuraciÃ³n efectiva correcta, Sky Garden confirmado, carga de al menos 4000 draws/cuadro y ausencia de compilaciones. Si los draws varÃ­an mÃ¡s del 5 %, no calcula una diferencia de FPS atribuible a instrumentaciÃ³n. El ranking se guarda junto al costo observado de activarla. No extrapolar resultados de 1440p al baseline histÃ³rico sin igualar cÃ¡mara y configuraciÃ³n.

## ValidaciÃ³n realizada

CompilaciÃ³n Release del emulador y pruebas completada. Pasaron cuatro suites CTest: audit_profiler, benchmark_replay y ambos flags CLI. AuditProfiler pasÃ³ sus 48 pruebas, incluidas seis nuevas para uniÃ³n de intervalos, aislamiento entre hilos, handlers anidados, reinicios de captura, flags, integraciÃ³n de timers y exportaciÃ³n. El test de replay valida tambiÃ©n activar profile_pm4 antes de la siguiente captura.

Pasaron dos pruebas Python del ranking: ordenar por exclusivo y rechazar sumas/intervalos inconsistentes. Pasaron tambiÃ©n las tres pruebas existentes del analizador binding-reuse. Logs: `_Build/windows/opt12_tests.log`, `opt12_verified_build.log` y `opt12_final_check.log`.

No se ha medido todavÃ­a el costo de esta instrumentaciÃ³n en Astro Bot ni se ha obtenido el top-5 real. No se afirma una ganancia de rendimiento.

## SesiÃ³n preparada

Directorio: `_Build/windows/live_play/20261125_000540_496`. PID: `16452`. SHA256: `E4567A1CE713D41189D604E8D000D666203C45B3EA5F12AD4718C22A0F823D23`. ResoluciÃ³n: 2560x1440. Motor base: `2f76cc50d` con cambios locales de diagnÃ³stico. Esperando aviso del usuario antes de armar las capturas. Se conserva la partida y no se ejecuta navegaciÃ³n automÃ¡tica.

## Retoma del 25 de noviembre

Las sesiones `20261125_000540_496` y `20261125_000730_955` se cerraron después de inicializar gráficos, antes de armar la medición. No hay capturas válidas de ellas. No se encontró un evento Windows de fallo de aplicación que explique el cierre.

Se añadió `-WaitForExit` al lanzador: conserva la conexión con el proceso y registra el código de salida cuando termina. La sesión `20261125_005937_911` (PID 5184) superó el arranque, permanece abierta y llegó a worldmap. Esto verifica el arranque observado; no establece por sí solo la causa de los cierres anteriores. El lanzador asociado permanece activo porque es necesario para esta sesión manual.

Esperar confirmación del usuario en Sky Garden antes de crear el archivo `start_measurement`; no activar navegación automática. El script pasó la comprobación de sintaxis.

## Resultado real de las seis capturas a 1440p

Sesión `20261125_005937_911`, seis capturas de 30 s OFF/ON completas. No hubo shaders compilados. La primera captura ON creó un pipeline: se excluye del ranking limpio. Los draws/cuadro variaron entre 4804,19 y 5161,02 (más del 5 %); por ello no se establece el costo FPS de activar la instrumentación ni se presenta una mejora/regresión de rendimiento.

El ranking siguiente agrega únicamente `pm4_on_2` y `pm4_on_3`, sin compilación ni creación de pipelines y con cero scopes descartados:

| Opcode | Invocaciones/cuadro | Restante host ms/cuadro | Inclusivo ms/cuadro | Espera host incluida ms/cuadro |
|---|---:|---:|---:|---:|
| IT_DRAW_INDEX_OFFSET_2 (0x35) | 4835,03 | 3,5705 | 36,3483 | 3,1513 |
| IT_DISPATCH_DIRECT (0x15) | 259,58 | 1,7936 | 1,7942 | 0,0006 |
| IT_DISPATCH_INDIRECT (0x16) | 10,99 | 1,5160 | 3,1810 | 1,5971 |
| IT_SET_SH_REG (0x76) | 9837,72 | 0,4774 | 0,4774 | 0 |
| IT_RELEASE_MEM (0x49) | 1008,08 | 0,4011 | 0,4011 | 0 |

La columna restante es el campo exclusivo del profiler: tiempo de pared después de descontar las regiones instrumentadas. No es CPU pura. La espera es un subconjunto del inclusivo, no se suma otra vez. En todos los opcodes, el restante suma 8,6762 ms/cuadro bajo esta definición.

La revisión de código confirma un límite de cobertura importante: `renderCompute.cpp` no tiene los ScopedStageTimer de las fases de preparación que sí existen en `renderDraw.cpp`. Por eso los valores de dispatch todavía incluyen preparación de cómputo no separada, además de trabajo del handler. No atribuir esos 1,79/1,52 ms únicamente a decodificar paquetes.

La primera ruta concreta para revisar es `CpOpDrawIndexOffset` / ejecución del dibujo indexado fuera de los scopes ya cubiertos. El costo restante de SET_SH_REG es mucho menor pese a su frecuencia: estos datos no justifican priorizar una nueva tabla de despacho o una caché general de registros. Para evaluar compute se debe completar primero esa atribución por fases, sin concluir una optimización por el solo contador exclusivo.

Reproducción del ranking sin afirmar costo de instrumentación:

```powershell
python tools/benchmarks/analyze_pm4_profile.py _Build/windows/live_play/20261125_005937_911 --ranking-only
```

Resultado: `pm4_ranking_only.json` en esa sesión. El modo de comparación estricto continúa rechazando la calibración contaminada; el modo ranking excluye explícitamente las muestras con compilación y no calcula ganancias FPS. Las tres pruebas Python del analizador pasan, incluida la exclusión de la muestra contaminada.

## Sesión posterior a 1080p

A pedido del usuario se cerró la sesión de medición y se abrió `20261125_010849_012`, PID 10472, a 1920x1080 con límite de 30 FPS. No hay replay ni botones automáticos. `-LightDiagnostics` mantiene el overlay y deja la captura manual en espera; apaga effect-profile, live diagnostics y profile-pm4 para no mezclar el costo detallado con la prueba de juego. El lanzador sigue conectado con `-WaitForExit`. No se modificó la configuración global ni se restauró el guardado.
