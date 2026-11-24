# Reutilización de preparación de recursos de programas

## Implementación

Se añadió `--resource-prep-reuse` / `--no-resource-prep-reuse`, inicialmente apagado. Cada Program de PipelineCache conserva como máximo el último resultado de decodificación/clasificación de sus recursos directos. No se sustituye la preparación por una comparación de punteros de memoria guest.

El recorrido SRT compilado se ejecuta en cada consulta con los datos actuales. Mantiene las lecturas de memoria, los fallos y readbacks necesarios, el contenido SRT aplanado y los rangos de especialización. Solo después se compara por contenido completo el scratch resultante contra la copia anterior. Si coincide, se reutilizan las descripciones y su especialización; los datos de usuario se copian de la entrada actual. Si difiere, se ejecuta el camino original y solo un resultado exitoso puede sustituir la entrada. Un fallo del walker invalida la entrada. Las actualizaciones de buffers/texturas y las barreras posteriores conservan su ejecución.

La entrada pertenece a un plan inmutable y se reinicia al cambiar de plan o walker. Compute, planes desconocidos, recursos indirectos/heaps y planes con fill conservan el camino completo. Solo se admiten planes con hasta 4096 uint64 de scratch (32 KiB), 256 fuentes y 256 recursos directos en total. Se conserva una sola entrada por programa. No se modifica la serialización, el compilador de shaders, los relojes, la calidad ni el audio.

Las solicitudes del replay se aplican al inicio de una operación GPU. Los contadores ResourcePrepHits/ResourcePrepMisses se exportan como deltas por captura; misses incluye el camino completo de los planes no elegibles. No convertir aciertos en una supuesta ganancia de FPS.

## Corrección

Compilación Release aprobada. Siete pruebas aprobadas: resource_prep_reuse, audit_profiler, cadence_runtime, tile_layout_cache, effect_profile, binding_reuse y stream_buffer_ring. Log: `_Build/windows/resource_prep_tests.log`.

El nuevo test compara todos los campos de ResourceSnapshot y ResourceSpecialization con el camino original después de cambiar dirección y stride del descriptor, contenido escalar de SRT, datos de usuario y plan. Verifica repetición, invalidación después de una lectura fallida y fallback de compute. El selector también ejecuta pruebas Vulkan de sincronización de imágenes y descriptores nulos con la opción encendida. AuditProfiler comprueba aplicación diferida del flag, deltas respecto de la captura y reinicio al cargar configuración.

## Protocolo de juego

Reproducción: `./stage8_resource_prep_auto.ps1`. Un solo proceso, cuatro capturas de 40 s en orden apagado/encendido/encendido/apagado, con unos segundos de asentamiento entre cambios. Cadencia encendida en ambas condiciones; resolución 2560×1440, relaxed shader float y readback de imágenes lineales como en la partida interactiva del usuario. Sin diagnóstico por shaders ni diario vivo durante la comparación. No comparar directamente contra el ensayo anterior de 1280×720.

Ejecución iniciada: `_Build/windows/stage8_4f_runs/20261123_232354_510_A`, PID 16876. Binario SHA256 `29E17BC1C7998A7E66A2E6128BB9E972973A74F761051B89B3DC8883BF455A4D`, conservado también como `_Build/windows/resource_prep_tested_20261123.exe`.

El arranque recuperó el driver cache de 23,97 MB y preparó 968 entradas del shader store del entorno `2c4a952d03b776fd`. Esto confirma recuperación de caché en este arranque compatible. No equivale a demostrar que todas las variantes futuras de un nivel ya estén presentes.

## Estado final de esta ejecución

La repetición entró correctamente en Sky Garden. Se comprobó visualmente el descenso y el log registra `aerial_garden`. A las 23:28, durante la primera captura de control, Computer Use informó que el usuario había presionado Escape y detuvo el control. Se canceló el ensayo y se solicitó cierre normal únicamente al PID propio 16876, verificando identidad, ruta y hora de inicio. El proceso terminó y el wrapper rechazó explícitamente la captura control1 incompleta. Se conserva `interruption.json`.

**No hay un A/B completo ni una ganancia de FPS acreditada para esta optimización. Permanece apagada por defecto y no se activó en el launcher.** Los siete tests de corrección sí terminaron correctamente antes de iniciar el juego. El ejecutable entregado conserva el hash indicado arriba. No se modificó código durante la ejecución ni quedaron procesos de prueba propios activos.

Para completar la validación de rendimiento queda repetir `./stage8_resource_prep_auto.ps1` y comprobar la escena estacionaria en las cuatro ventanas. Los archivos run.json, source.patch, replay.json, stdout/stderr y las capturas individuales son la evidencia primaria; no usar una captura de descenso ni control1 interrumpida como comparación del candidato.

## Variante con joystick controlado por el usuario

`./stage8_live_play.ps1 -ManualAB` conserva los ajustes del launcher y abre el juego con el joystick del usuario, sin navegación automática. La repetición contiene únicamente cambios de modo y capturas. El motor rechaza botones y quit cuando se utiliza `KYTY_BENCH_ARM_FILE`; no exige cerrar el juego al terminar. Espera a que el usuario llegue a la escena y confirme que dejó quietos personaje y cámara. Solo entonces se crea el archivo de armado de esa ejecución.

Las cuatro ventanas duran 40 segundos y el ciclo termina aproximadamente 187 segundos después del armado. No se crea un temporizador guest ni se altera la velocidad del juego. Se conserva el control manual después de medir. El diario vivo y el diagnóstico por shader permanecen apagados durante este A/B. Análisis: `python tools/benchmarks/compare_mode_benchmark.py <directorio> resource_prep_reuse`.

## Resultado de la sesión manual

El usuario llegó al nivel con su joystick, confirmó que estaba quieto y se armó la medición mediante el archivo de esa sesión. No se enviaron botones. Las cuatro capturas terminaron completas y los CSV coinciden con los cuadros exportados. Directorio: `_Build/windows/live_play/20261123_233325_734`; PID 14268; SHA256 del ejecutable `11FCD0C4B9794B12241F13CD6FF7E5D3F17E295016ACFE66D981AD22256D1BEB`. Este binario añade el armado manual al anterior; no modifica el algoritmo de la optimización. Resolución 2560×1440, cadencia activada en todos los intervalos, restantes opciones iguales.

| Captura | Reutilización | Cuadros | Segundos | FPS | P95 ms | Shaders / pipelines nuevos |
|---|---|---:|---:|---:|---:|---:|
| Control 1 | Apagada | 585 | 40,053 | 14,6056 | 83,5006 | 6 / 12 |
| Candidato 1 | Encendida | 582 | 40,005 | 14,5482 | 83,5068 | 0 / 0 |
| Candidato 2 | Encendida | 571 | 40,054 | 14,2558 | 83,7079 | 0 / 0 |
| Control 2 | Apagada | 562 | 40,055 | 14,0307 | 83,6464 | 0 / 0 |

Agregación por cuadros/duración: apagada 1147/80,108 = **14,3182 FPS**; encendida 1153/80,059 = **14,4019 FPS**. Diferencia observada +0,5846 %, menor que la variación de 4,0977 % entre controles. CPU del hilo gráfico: 54,6262 frente a 54,8704 ms/cuadro; preparación de estado: 19,2331 frente a 19,4082 ms/cuadro. No se demuestra reducción de esos costes.

La compilación del primer control hace que el analizador estricto rechace la comparación. Se mantuvo esa comprobación: **no se presenta el +0,58 % como ganancia validada** ni se eliminan cuadros del control para mejorar el resultado. Las capturas completas y sus condiciones se conservan; `comparison_exploratory.json` contiene la agregación con `clean_performance_comparison=false`. Los candidatos no compilaron shaders nuevos, pero eso no vuelve limpio el primer control.

Se registraron 792.473 reutilizaciones y 9.638.250 recorridos completos/fallbacks; aproximadamente un 7,60 % de las consultas accedió a la ruta rápida. Esto explica su cobertura limitada, sin convertir los aciertos en ahorro medido.

**Decisión: conservar el flag apagado y no activar esta optimización en el launcher. No hay mejora demostrada de FPS.** Al terminar se restauró el modo apagado y se avisó al usuario que podía seguir jugando; el replay no contiene cierre automático. En la comprobación posterior el proceso ya no estaba activo y el log mostraba la exportación de cierre. No se atribuye aquí la causa de ese cierre. No hay monitor externo ni proceso auxiliar propio pendiente. La cadencia anterior sigue habilitada para Astro Bot.
