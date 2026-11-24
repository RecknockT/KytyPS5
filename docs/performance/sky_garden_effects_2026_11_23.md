# Sky Garden: atribución por shader y lectura síncrona

## Resultado del diagnóstico

Se implementó y ejecutó una atribución por grupo de shaders, con intervalos de timestamps Vulkan y tiempos del host separados. No se identifica un efecto por su aspecto ni se presenta esta instrumentación como una mejora de FPS.

Ejecución válida: `_Build/windows/stage8_4f_runs/20261123_180839_183_A`, PID 26644, SHA256 `7A5C26DBA01BF0E93172FC2AE56EA70D61D1C704FFC5FD9F8E6F499A7B9DBDEC`. Terminó normalmente con salida 0 a las 18:14:02. Se confirmó visualmente la selección de Sky Garden y el log contiene `StartLevel aerial_garden`. La ruta incluye el descenso y una escena fija sin movimiento del personaje.

La captura `control1` completó 623 cuadros a 15,56 FPS, unos 5025 dibujos por cuadro, sin shaders compilados ni pipelines nuevos. Es una ejecución instrumentada: no comparar esos FPS con otro ejecutable para atribuir una ganancia o regresión.

El análisis por diferencias de `effects.jsonl` comprende los cuadros 9840–10440: 600 cuadros y 37,644 segundos. Es un intervalo final de escena estable distinto de los límites exactos de `control1`. No hubo compilaciones ni pruebas ejecutándose simultáneamente durante esta ejecución.

| Tiempo host atribuido por cuadro | Medido |
|---|---:|
| Preparación/emisión de dibujos identificados | 48,93 ms |
| Preparación/emisión de compute identificado | 8,31 ms |
| Total de esos ámbitos | 57,23 ms |
| Descargas síncronas dentro de esos ámbitos | 15,67 ms |

Las descargas son un subconjunto de los tiempos host, no se suman de nuevo. Estos ámbitos incluyen esperas: no son tiempo CPU exclusivo.

## Concentración de las esperas

| Grupo | Llamadas por cuadro | Tiempo host por cuadro | Readback incluido |
|---|---:|---:|---:|
| Compute indirecto `c5ee521b8849cf83` | 2,0 | 4,980 ms | 4,905 ms |
| VS `2e661816f622e25d`, PS `a30803e0a28df091` | 19,5 | 3,752 ms | 3,628 ms |
| VS `cfbdc8bcef7b4c86`, sin PS | 19,0 | 3,731 ms | 3,604 ms |
| VS `95ab96b3b827eda7`, PS `ee9eaf0ac4d353d8` | 50,7 | 3,240 ms | 2,557 ms |

Estos cuatro grupos concentran aproximadamente el 93,8 % del tiempo de las descargas síncronas atribuidas. El grupo compute tiene 1200 muestras GPU en el intervalo, media de 2,612 microsegundos por muestra; su coste host procede casi enteramente del readback. No confundir ese intervalo GPU muestreado con una medición aislada sin solapamiento.

También hay costes distribuidos de preparación: por ejemplo, el grupo VS `e60d2cd17013702c` / PS `e4920333008f9668` consume 4,076 ms host por cuadro con readback atribuido nulo. Por tanto, resolver las esperas no elimina todo el coste del renderizador.

## Lo que no demuestra esta prueba

- No asigna nombres de juego a los hashes: no prueba que un grupo sea una esponja, agua, iluminación o sombras. Un grupo sin PS tampoco identifica por sí solo un pase de sombras.
- No se recorrió una interacción específica con esponjas; no extrapolar el resultado a esa escena.
- El contador `dropped` aumentó en 8833. La recogida GPU tiene límites por lote y de pools; las muestras GPU no cubren necesariamente todos los grupos por igual. El ranking host se recoge independientemente de ese muestreo. No convertir los intervalos GPU en un porcentaje de ocupación ni sumar sus medias como un cuadro físico.
- El sistema estuvo cerca de su límite de memoria comprometida. No se midió paginación física; no atribuir por ello estos costes al SSD o a falta de RAM.

## Automatización y validación

La guía [effect_profile.md](effect_profile.md) describe las opciones, el muestreo, los límites y el análisis reproducible. La secuencia actual confirma la partida a los 65 segundos, navega a partir de los 110 y mantiene X tres segundos para entrar al nivel. Se corrigió una carrera con la animación del menú que podía dejar la prueba en selección de partida. Los wrappers verifican también el nombre del nivel.

Las ejecuciones `20261123_175611_005_A` y `20261123_180631_659_A` se cerraron normalmente y se descartaron por ruta incorrecta. `20261123_175918_315_A` fue un piloto que llegó a Sky Garden, pero tuvo compilación del programa de pruebas durante parte de la fase final; no se usa como benchmark ni como resultado principal de tiempos. Sus campos de readback aún no estaban instrumentados y no equivalen a cero.

Pasaron `effect_profile`, `audit_profiler`, `binding_reuse` y `stream_buffer_ring` después de añadir la atribución de readbacks. También pasaron cinco pruebas del analizador: diferencias entre acumulados y media de muestras, conservación de registros completos ante cola truncada, datos ausentes de readback, ausencia de muestras GPU y rechazo de contadores decrecientes. Los datos ausentes se presentan como desconocidos.

## Prueba siguiente basada en este resultado

Comparar la cadencia de envíos existente a 1 ms, apagada/encendida/encendida/apagada, con un único ejecutable y proceso y sin `effect-profile`. Se añade cambio de política en límites de operación para evitar dos arranques y comparar con la misma cámara. No se elimina ninguna lectura obligatoria ni se alteran barreras, relojes guest, VBlank o audio. La decisión depende de FPS y frametimes reales; una reducción del contador de esperas por sí sola no basta.

Protocolo y comando reproducible: [cadence_live_ab.md](cadence_live_ab.md).

La comparación ya terminó: **16,09 frente a 18,49 FPS (+14,88 % observado)** en la escena estable. Ambas muestras encendidas superaron a ambas apagadas. Se activó la cadencia solo para Astro Bot en el launcher; el informe enlazado conserva medidas, límites y verificación del ajuste por juego.
