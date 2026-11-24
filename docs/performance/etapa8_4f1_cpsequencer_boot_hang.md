# 8.4F.1 — investigación del arranque con CpSequencer

Fecha: 2026-11-23. Base: `14185ebd9feed76015ebf8760ffad391e3c3ef50`, rama `main`.

## Hechos y correcciones

1. El productor original reintentaba WAIT_REG_MEM dentro de la submission. El resolver, al quedarse sin operaciones, permanecía dentro de ResolveSubmission. Esto impide que el scheduler del guest atienda otras colas capaces de escribir la condición esperada. El resolver ahora conserva una espera insatisfecha, envía el trabajo anterior y devuelve Blocked. El productor recibe respuesta únicamente cuando se cumple la condición. Se conservan comparaciones y tamaños de 32/64 bits.
2. DrawPrep avanzaba head mientras las siguientes ranuras estuvieran Free. Publish incrementa tail antes de rellenar y publicar su ranura: una reserva en curso aún puede estar Free. Ese recorrido podía saltarla. Las pruebas de saturación se bloquearon dos veces durante esta reconstrucción; después de sustituirlo por el avance de exactamente un ticket consumido, finalizaron correctamente.
3. Las notificaciones de Published/Ready se realizaban sin adquirir el mutex asociado al condition_variable. Las transiciones ahora toman ese mutex para evitar una notificación perdida entre la comprobación del predicado y el aparcado.
4. Los Drain de operaciones intermedias consumían futuras preparaciones ya publicadas en OpStream. Se retiraron del resolver; las barreras Vulkan y operaciones PM4 se siguen ejecutando.

Estos hechos no equivalen por sí solos a demostrar cuál causó el congelamiento histórico del primer logo. Esa atribución requiere una reproducción real con diagnóstico.

## Hallazgos adicionales reproducidos durante las pruebas C

- Primera ejecución C: abortó en shader.cpp:201 por superar RES_MAX. PrepareSlot conservaba recursos y flags de su uso anterior. Ahora reinicia PreparedDraw completo y no inventa un contador inicial de buffers. La prueba TestDrawPrepPreparedStateReset cubre cuatro vueltas del ring alternando shaders activos/inactivos.
- Segunda ejecución C: pantalla negra al cargar el mapa. Se registraron **88,5 s sin progreso muestreado** en executed=1125721, ticket=23882, cuatro slots Claimed y cinco snapshots CP pendientes. No era saturación del ring (capacidad 64).
- Cadena de espera identificada: PrepareProgram lee tablas de vértices de memoria guest; una página GPU-dirty pide BufferCache::ReadMemory, que utiliza GuestGpu::SendCommandSync. El GPU esperaba Ready en CommitPublished y no atendía esa solicitud. Ahora CommitPublished acepta un callback de servicio y lo ejecuta sin mantener el mutex de DrawPrep. El resolver procesa su cola de solicitudes GPU mientras espera.
- TestWorkerReadbackMakesProgressWhileResolverWaits reproduce esa dependencia con un worker real y respuesta síncrona. La prueba real posterior C llegó al worldmap visible, y los cuatro workers registraron respuestas gpu-sync-complete y progreso posterior a la antigua posición bloqueada.
- La primera instrumentación tenía una espera artificial de 1 ms en WaitSlow que ralentizaba B. Fue retirada. El heartbeat lo toma un jthread independiente: no se usa esa ejecución contaminada para evaluar rendimiento.

## Diagnóstico disponible

- `KYTY_CP_SEQ_DIAGNOSTICS=1`: registros JSON por hilo, limitados a una muestra por segundo. Incluyen progreso, número de operación, snapshots adquiridos/liberados y estado de shutdown.
- DrawPrep informa head/tail y números de ranuras Free, Published, Claimed y Ready, además de compromisos, fallbacks y esperas de tickets.
- WAIT_REG_MEM informa dirección, valor observado, referencia, máscara, comparación y tamaño; emitted/executed permiten detectar falta de progreso.
- `KYTY_CP_SEQ_TRACE_OPS=1`: traza exhaustiva emit/peek/consume por operación. Tiene sobrecoste e I/O apreciables: usar para diagnóstico y no para comparar FPS.
- `stage8_4f_run.ps1` conserva stdout, stderr, parámetros, SHA, hash del ejecutable, diff del código y muestras de CPU/memoria/título de ventana en process.jsonl.
- Se registran también I/O lógico del proceso, RAM disponible y compromiso/límite de compromiso de Windows. El I/O lógico incluye caché del sistema y logs: no equivale al tráfico físico del SSD.
- Hay registros de solicitud/respuesta GPU síncrona y resúmenes de finalización del productor, drenaje GPU y workers.
- `stage8_4f_analyze.ps1` genera diagnostics_summary.json y diagnostics_cp.csv, identifica el mayor intervalo muestreado sin progreso y conserva los últimos estados de espera.
- La telemetría existente exporta audit.json, audit.csv y series por cuadro. Los contadores de VBlank no se consideran FPS.

Interpretación: una secuencia y un valor de espera inmóviles sugieren una dependencia sin resolver; Claimed inmóvil apunta a un worker; snapshots al límite con released inmóvil indica backpressure. Son pistas que deben correlacionarse, no pruebas automáticas de deadlock.

## Ejecuciones reales

- Primer control A con caché: se detuvo a los 90 s mientras seguía preparando shaders (1118/1537). No se clasifica como congelamiento de CpSequencer, que estaba desactivado.
- Control A sin caché, 120 s: alcanzó `title_controller_ship` y `transition_next`; exportó telemetría y terminó con código 0. No se midió una escena estable de Sky Garden.
- B con instrumentación corregida: `20261123_010042_376_B`, 120 s, exit 0. Llegó al worldmap y se comprobó visualmente en pantalla.
- C tras corregir slots y servicio de readbacks: `20261123_011115_759_C`, 120 s, salida 0. Llegó al worldmap y se comprobó visualmente en pantalla.
- C tras reforzar también el orden de compute: `20261123_011526_986_C`, 90 s, salida 0. Registró `worldmap` y `transition_next`; el mayor intervalo muestreado sin avance de executed fue 4,021 s. Se exportaron 2460 registros por cuadro. Estos datos comprueban progreso durante ese arranque, no una mejora de FPS ni ausencia de bloqueos en todos los niveles.

## Reproducción

```powershell
.\stage8_4f_run.ps1 -Profile A -BootSeconds 120 -ColdBoot
.\stage8_4f_run.ps1 -Profile B -BootSeconds 120 -ColdBoot
.\stage8_4f_run.ps1 -Profile C -BootSeconds 120 -ColdBoot
# Si aparece una espera inmóvil, repetir ese perfil con -TraceOps.
```

ColdBoot desactiva la caché sólo para esa ejecución y no borra los archivos existentes. Separa el arranque del tiempo de preparación; las tres comparaciones deben utilizar el mismo modo. El benchmark de gameplay deja la caché activada y espera F9/F10.
