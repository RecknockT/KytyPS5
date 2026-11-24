# Investigación de carga y aterrizaje — 23 de noviembre de 2026

## Resultado

Se reprodujo la entrada a Sky Garden / aerial_garden con caché activada y se observó a Astro en tierra después del descenso. La lentitud persiste con cero shaders y pipelines nuevos compilados durante la captura. La evidencia apunta a preparación y ejecución de muchos dibujos y sincronización de memoria CPU/GPU; no demuestra que exista una única causa ni permite atribuir cada espera a trabajo físico de GPU.

No se modificó el motor, la calidad, los relojes guest, VBlank ni el audio en esta investigación. CP Sequencer, DrawPrep y Draw State Dirty Check estaban desactivados. No se afirma una ganancia de rendimiento.

## Evidencia de la ejecución actual

Directorio: `_Build/windows/stage8_4f_runs/20261123_012318_761_A/`. Base `14185ebd9feed76015ebf8760ffad391e3c3ef50`, con los cambios locales 8.4F. Hash SHA256 del ejecutable: `93EA195B436BB84A8823400791366F5ED094A06D106F256CD5B6AD0B765FCA41`.

La ejecución duró seis minutos, PID 22208, salida 0. Primero se capturó el arranque (`boot_audit.*`). F9 detuvo esa captura y comenzó otra durante el túnel de entrada al nivel (`audit.*`). La segunda terminó manualmente después de observar al personaje en tierra. Incluye el final del túnel, entrada, descenso y unos segundos en tierra: no representa exclusivamente la animación de aterrizaje.

| Métrica de esa segunda captura | Medición |
|---|---:|
| Duración real | 111,737 s |
| Cuadros registrados | 1522 |
| Tiempo medio por cuadro | 73,413 ms |
| FPS efectivos | 13,621 |
| P50 / P95 / peor cuadro | 67,428 / 84,397 / 1484,730 ms |
| Shaders compilados / pipelines creados | 0 / 0 |
| Dibujos por cuadro, media de toda la captura | 4971 |
| Preparación de descriptores | 12,785 ms/cuadro |
| Preparación del estado de renderizado | 33,270 ms/cuadro |
| Grabación de comandos | 4,047 ms/cuadro |
| Búsqueda de pipeline | 0,329 ms/cuadro |
| Lecturas de memoria GPU → CPU | 8,158 esperas/cuadro; 21,978 ms/cuadro acumulados |
| Esperas del hilo de operaciones prioritarias | 55,808 ms/cuadro acumulados |
| Esperas del búfer de subida | 3,189 ms/cuadro acumulados |
| Subidas CPU → GPU | 812,897 MB/s |
| Descargas GPU → CPU | 22,318 MB/s |

Los tiempos de etapas son inclusivos/anidados, y las esperas suman varios hilos: **no sumar estas filas para reconstruir el tiempo de un cuadro**. En particular, la espera del hilo prioritario puede solaparse con la espera de readback y con otros trabajos. El campo existente `gpu_thread_wait_time_ms` acumula esperas globales y no es una medida exclusiva de espera de Thread_Gpu.

Las etiquetas F10 son manuales: `DescentStart` contiene 528 cuadros a 85,666 ms de media y `ActiveDescent` 755 cuadros a 71,351 ms. La segunda etiqueta incluye el final de aproximación y el personaje ya en tierra: no se capturaron fronteras fiables de GroundApproach/Touchdown/EarlyGameplay y no deben inventarse sus duraciones. Las filas iniciales Gameplay corresponden al túnel, no al gameplay en tierra.

Los mensajes del juego registraron `LoadLevelResources: underwater_aerial_garden : 18.828 sec.` y otro recurso con 12,971 s. Son tiempos reportados por el juego, pueden incluir tareas concurrentes y no equivalen a tiempo exclusivo del SSD ni se pueden sumar para calcular la carga total. Durante el descenso hubo avisos Async de dos traducciones esperando recursos, con cero traducciones/compilaciones activas; terminaron en idle. En `pipelineAsync.cpp`, las traducciones pueden esperar un dibujo que proporcione recursos y se descartan al quedar antiguas: ese contador no demuestra por sí mismo un compilador bloqueado.

## Memoria, disco y GPU física

`system_pressure.jsonl` conserva seis consultas a contadores de Windows entre 01:28:18 y 01:28:28 (Argentina), durante el descenso:

- Memoria comprometida del sistema: 40,19–40,24 GiB de un límite de 41,69 GiB. Es un límite de compromiso de Windows, no el límite de memoria guest PS5 ni la VRAM.
- RAM disponible: 5816–5878 MiB.
- Page Reads/sec, Pages Input/sec, Pages Output/sec y Page Writes/sec: cero en esas seis muestras. No hay evidencia de paginación activa en ese intervalo; no se descartan picos anteriores, como el primer cuadro del nivel.
- Motor 3D de la GPU dedicada para el proceso: 34–37 %. Sugiere que la GPU no se mantuvo saturada durante esas muestras; no demuestra que ninguna parte del cuadro esté limitada por GPU.
- El muestreo breve de hilos queda en `thread_cpu_sample.json`. No identifica nombres de hilos y no debe usarse para atribuir automáticamente el hilo más ocupado a Thread_Gpu.

El profiler exporta `gpu_timestamps_supported=false`, cero consultas y cero tiempo de ejecución GPU. En el código actual, `RecordGpuExecutionTime` tiene declaración y definición pero no llamadas desde el renderizador. Los ceros **no significan que la GPU no trabaje**. Hace falta instrumentación Vulkan de timestamps para separar ejecución física de la GPU, espera de cola y CPU con mayor precisión.

## Precarga y ventana «No responde»

La precarga terminó: 608 programas, 678 módulos y 859 pipelines en **186220 ms**. Se prepararon 1537 elementos usando un hilo. `pipelineSerialization.cpp` fuerza ese modo para NVIDIA por un fallo de driver documentado en el propio código; no conviene activar compilación concurrente indiscriminadamente.

El contador siguió avanzando mientras Windows mostraba «No responde». La construcción de PipelineCache llama WarmUp durante la inicialización de la ventana, antes de WindowRun y de su bucle de eventos. La recuperación después de terminar la precarga se comprobó visualmente. Esto explica ese episodio de falta de respuesta; no es el mismo bloqueo circular de DrawPrep corregido en 8.4F. Mantener la ventana atendiendo eventos durante la precarga es una corrección de interfaz pendiente, independiente de acelerar el descenso.

## Capturas anteriores y limitaciones

Se analizaron también `stage8_1_landing_{optimized,memory,shaders}.json` y sus series CSV, del 21 de noviembre, revisión db2aba1-dirty. Las tres registran cero shaders/pipelines nuevos y unos 62–64 ms por cuadro durante ActiveDescent. La captura optimized contiene 967 cuadros a 64,254 ms de media y 15,555 FPS efectivos.

Estas capturas usaban Draw State Dirty Check y otras opciones distintas de la ejecución actual. No forman un A/B válido con esta build ni demuestran una regresión o mejora entre versiones. `stage8_1_landing_baseline.json` contiene cero cuadros y su CSV contiene filas antiguas que no coinciden: no sirve como control. La herramienta rechaza esa mezcla. Las afirmaciones históricas que descartan totalmente la GPU o identifican el SSD como causa no quedan demostradas por estos datos.

## Siguiente trabajo, por prioridad

1. Separar las esperas de readback y publicación de resultados por hilo y dirección/tick. Medir qué datos se vuelven a descargar y cuántas esperas pueden agruparse conservando barreras, propiedad y coherencia. El agrupamiento de lecturas calientes ya existe como opción; necesita un A/B del mismo recorrido, sin dar por supuesto que ayuda.
2. Desglosar PrepareBindings/PrepareGraphicsBindings en búsqueda de recursos, validación de caché, actualización de texturas y escritura de descriptores. La búsqueda de pipelines ya es pequeña en esta captura; no es el primer objetivo.
3. Añadir timestamps Vulkan y asociarlos a submissions/cuadros sin esperar sincrónicamente sólo para leer la medición. Con ello se podrá evaluar el coste físico de geometría/sombras y las oclusiones, que actualmente se tratan como visibles por defecto. Activar oclusiones también puede introducir esperas: requiere prueba visual y A/B.
4. Medir lectura y descompresión de recursos por separado durante el túnel. Los mensajes LoadLevelResources no permiten decidir cuánto corresponde a SSD, CPU o dependencias de renderizado.
5. Corregir la atención de eventos durante la preparación obligatoria de shaders; mantener el modo NVIDIA seguro hasta demostrar una alternativa estable.

Cada optimización deberá conservar sus flags positivos/negativos y validarse con el mismo nivel, cámara, calidad, estado de caché y fases. No alterar la velocidad de simulación para acortar la animación.

## Herramienta de análisis

Tras esta investigación se integró un primer lote de opciones de memoria (lecturas de vecinos, ventana de 4 MiB y ventana de escritura de 256 KiB), con validación dirigida y sin ejecutar el juego. Véase [la integración de transferencias](transfer_prefetch_integration.md). No cambia los resultados medidos aquí ni demuestra todavía una mejora del aterrizaje.

`stage8_landing_analyze.ps1 -Capture <audit.json>` exporta JSON y CSV por etiqueta bajo `_Build/windows/landing_analysis`, o en el directorio indicado por `-OutputDirectory`. Fue ejecutada con las tres capturas históricas y la actual. Rechaza capturas vacías o diferencias de número de filas, utiliza decimales invariantes y avisa sobre ausencia de timestamps y opciones con regresiones conocidas.

No quedaron procesos de esta prueba abiertos; no se hicieron commits ni push. Los archivos de guardado modificados por las pruebas se conservan separados de los cambios de código.
