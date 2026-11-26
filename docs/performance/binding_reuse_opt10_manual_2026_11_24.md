# Binding reuse sobre OPT-10: validación manual 3+3

Resultado: sin mejora neta demostrada; mantener `binding_reuse_enabled=false` por defecto.

Sesión: `_Build/windows/live_play/20261124_224707_241`, PID 3948. SHA256 del ejecutable: `8C4A8BB1F943444FF9A91C58AD27F85263C90FA7820F5200510C006CAB37BC33`. Base del motor: `2f76cc50d`; los cambios de esta sesión corrigen y verifican la secuencia de medición, no el renderizador. El usuario llegó a Sky Garden con joystick, completó el descenso y confirmó personaje/cámara quietos antes de armar las capturas. Sin controles automáticos ni cierre programado.

Configuración: 2560x1440, readback lineal y relaxed shader float activados; submit cadence activada; límite FPS desactivado; CP sequencer, draw prep, effect profile, live diagnostics y timestamps GPU desactivados. OPT-10 y caché ResolveTexture 512 permanecen en sus valores por defecto. Solo binding-reuse cambia entre las seis capturas de 30 segundos.

| Par | OFF FPS | ON FPS | Diferencia ON/OFF |
|---|---:|---:|---:|
| 1 | 15,8445 | 15,8958 | +0,32 % |
| 2 | 15,9190 | 15,6775 | -1,52 % |
| 3 | 16,0533 | 16,2428 | +1,18 % |
| Medianas | 15,9190 | 15,8958 | -0,15 % |

P95 mediano: 67,1525 ms OFF y 67,1868 ms ON. DescriptorPrep mediano: 8,3943 -> 8,1317 ms/cuadro. La pequeña reducción de preparación no se traduce en mejora neta de FPS. La dispersión y el signo mixto de los pares no justifican afirmar una regresión general.

Las seis capturas finalizaron completas, sin shaders compilados ni pipelines creados. Los draws/cuadro oscilaron entre 4927,60 y 5150,74 (menos del 5 %); animaciones de la escena impiden identidad exacta de trabajo. No comparar estos FPS con los ~20 FPS históricos sin igualar resolución, cámara y configuración.

Artefactos: `binding_off_1.json` a `binding_on_3.json`, sus CSV de cuadros, `run.json`, `replay.json`, `manual_measurement.json`, `binding_comparison.json` y stdout en el directorio indicado. Analizador reproducible:

```powershell
python tools/benchmarks/analyze_alternating_binding.py _Build/windows/live_play/20261124_224707_241
```

Las esperas prioritarias medianas son 28,25 ms/cuadro OFF y 28,96 ON, acumuladas entre hilos; no son milisegundos recuperables ni prueban por sí solas el camino crítico. La ejecución física GPU no se midió (cero consultas). El siguiente experimento debe distinguir GPU, cola y trabajo CPU para estas operaciones, identificando la operación/recurso que bloquea el hilo de renderizado. No sumar ámbitos solapados ni usar la clasificación agregada QUEUE_SYNC_BOUND como prueba causal.

## Corrección del automatizador

La corrida anterior `20261124_224047_027` se descarta: el cambio de modo coincidió con el plazo de una captura cuyo cierre se procesa al renderizar un cuadro, provocando un cierre ordinario no deseado. BenchmarkReplay ahora conserva pendiente la orden hasta finalizar la captura y evita esperar en bucle activo. La prueba de regresión `benchmark_replay` pasó; también pasaron las tres pruebas del analizador, incluidas capturas contaminadas, configuración incorrecta y falta de confirmación del nivel.

El juego quedó abierto con la última muestra ON; el valor por defecto del motor sigue OFF. No se reinició ni se restauró el guardado del usuario al terminar. Los cambios de `_SaveData` de la sesión manual se conservan.
