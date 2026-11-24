# Caché de cálculos de textura: resultado real

Fecha: 23 de noviembre de 2026. **No se acredita mejora de FPS. La opción permanece desactivada por defecto.**

## Implementación

`--tile-layout-cache` / `--no-tile-layout-cache` controlan una caché local a cada hilo para el cálculo puro `TileGetTiledTextureLayout`, desde `ImageInfo::UpdateSize`. Almacena únicamente cálculos exitosos en 128 entradas (64 conjuntos, dos vías), con comparación de clave completa: formato, modo de tiling, dimensión, ancho, alto, profundidad, niveles y capas. Copia únicamente los niveles solicitados y conserva intacta la cola del array del llamador.

No reutiliza memoria guest, identidad de imágenes ni descriptores Vulkan. Los cálculos dependientes de direcciones, la detección de cambios, las lecturas, actualizaciones y barreras siguen ejecutándose. No cambia shaders, calidad, relojes ni formato persistente. Las solicitudes del replay se aplican en el hilo del renderizador al iniciar una operación. No se añadió esta opción experimental al launcher.

## Diagnóstico previo

Ejecución `_Build/windows/stage8_4f_runs/20261123_183659_760_A`, PID 19740, salida 0. SHA256 del binario: `7F7DE8AE5ACADB67ACACC17BEF862F3D34C8628BF318DBC2A40EF54949648A68`. Cadencia activada, diagnóstico activado y timestamps GPU apagados. Ventana de 600 cuadros, 32,8836745 s, entre cuadros 10920 y 11520.

El grupo VS `e60d2cd17013702c`, PS `e4920333008f9668`, destinos `1f` emitió 170,61 draws/cuadro. Tiempo host identificado: 3,910 ms/cuadro; preparación: 1,503 ms; readback atribuido: 0. Preparar programas consumió 1,275 ms/cuadro y decodificar recursos 1,405 ms/cuadro. Los hashes no identifican por sí solos esponjas, iluminación ni otro objeto concreto.

Se añadieron secciones opcionales `sections_ns` en EffectProfiler: programas, destinos, decodificación, búsqueda de buffers, vistas de imagen, reenlace de buffers, pipeline, commit, emisión y buffers de vértices. El analizador exporta sus diferencias como `sections_ms_per_frame`. Son tiempos host, pueden incluir esperas y están parcialmente anidados: emisión contiene commit. **No sumarlos como CPU exclusiva.** Reproducción: `./stage8_draw_sections_auto.ps1`.

## A/B automático

Reproducción: `./stage8_tile_layout_auto.ps1`. Recorrido con X mantenida tres segundos para entrar al planeta. ABBA en un solo proceso, cadencia activada en ambas condiciones, diagnóstico detallado apagado. Únicamente cambia esta caché entre capturas.

- Directorio: `_Build/windows/stage8_4f_runs/20261123_184832_966_A`.
- PID 3360, 18:48:33 a 18:56:06, salida normal 0.
- SHA256 probado y conservado en `_Build/windows/kyty_emulator.exe`: `C5B139091F582F6F640F7EB1A7CBF227EF96B9C2700BB5474D088C41794F0BE9`.
- Evidencia: `run.json`, replay, diff, stdout/stderr, cuatro JSON/CSV, series por cuadro y `comparison.json`. El log registra `aerial_garden`; cada captura completa supera 3500 draws/cuadro.
- No se recogió una nueva captura visual estacionaria durante este A/B. No se afirma equivalencia visual completa.

| Muestra | Caché | FPS efectivos | Draws/cuadro |
|---|---|---:|---:|
| Control 1 | Apagada | 18,9565 | 4870,12 |
| Candidato 1 | Encendida | 18,9891 | 4948,04 |
| Candidato 2 | Encendida | 18,4389 | 5178,84 |
| Control 2 | Apagada | 18,7562 | 5014,85 |

Agregación por cuadros totales / duración total:

| Métrica | Apagada | Encendida |
|---|---:|---:|
| Cuadros / segundos | 1510 / 80,079 | 1498 / 80,047 |
| FPS efectivos | 18,8564 | 18,7140 |
| P95 de cuadro, ms | 66,8057 | 66,8026 |
| P99 de cuadro, ms | 67,1012 | 67,0980 |
| Descriptores, ms/cuadro | 13,5868 | 13,5374 |
| CPU del hilo gráfico, ms/cuadro | 47,6097 | 47,9181 |
| Shaders / pipelines nuevos | 0 / 0 | 0 / 0 |

Observado: **−0,755 % en FPS**, con 2,420 % más draws/cuadro en el candidato y variación entre muestras. No permite atribuir una regresión concluyente; tampoco demuestra mejora. Los 56.853.347 aciertos y 292.119 fallos prueban uso de la caché, no ahorro neto. No se activa para el usuario.

La cadencia anterior permanece activada únicamente para Astro Bot. Su A/B independiente midió 16,094 → 18,489 FPS (+14,88 %); no mezclar esos datos con este ensayo ni sumar porcentajes. El descenso de esta ejecución tuvo captura informativa sin control equivalente: **no se acredita aceleración del aterrizaje**.

## Validación y cierre

Compilación Release y nueve pruebas aprobadas: cadence_runtime, tile_layout_cache, effect_profile, binding_reuse, audit_profiler, tres pruebas de submit cadence y stream_buffer_ring. La nueva prueba compara contra el cálculo original formatos, tamaños, dimensiones, niveles, entradas inválidas, expulsión y cola de niveles; también ejecuta pruebas reales de lectura/escritura de imágenes Vulkan. Cinco pruebas del analizador aprobadas. Registros: `_Build/windows/tile_layout_tests.log` y `draw_sections_analysis_tests.log`.

El juego y la sesión de ejecución terminaron con código 0. Se verificó la ausencia de ventanas del emulador. No quedaron procesos auxiliares propios en ejecución.
