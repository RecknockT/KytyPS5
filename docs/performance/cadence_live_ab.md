# Comparación de cadencia en una sesión

```powershell
.\stage8_submit_cadence_auto.ps1
```

Reutiliza la navegación corregida de Sky Garden y mide cuatro ventanas de 40 segundos: control apagado, candidato encendido, candidato encendido y control apagado. La última ventana restaura el control. El proceso se cierra normalmente a los 450 segundos del replay, después de la preparación inicial de shaders. `-Preview` permite revisar la secuencia sin abrir el juego.

La cadencia existente adelanta envíos Vulkan aproximadamente cada 1 ms cuando hay trabajo grabado y la GPU no acumula demasiados envíos pendientes. No cambia relojes guest, VBlank, audio, calidad, resolución ni controles del juego. El objetivo es solapar preparación del host y trabajo GPU, no aumentar el número mostrado de cuadros por un cambio de temporización.

El cambio de modo se solicita desde el reproductor y se aplica únicamente en `CommandScheduler::StartOperation` del renderizador. No se reemplaza el command buffer ni se fuerza un submit al cambiar la política. Se conserva el trabajo ya grabado y se reinicia la cuenta de cadencia; el scheduler de presentación conserva su política propia. La configuración exportada refleja el modo efectivo, no la solicitud pendiente.

La opción CLI sigue disponible como `--gpu-submit-cadence` y ahora también tiene su negativa explícita `--no-gpu-submit-cadence`. El valor predeterminado no se cambia por esta prueba. El cambio dinámico se usa para comparar con la misma escena en un solo proceso.

El wrapper exige que el log confirme `aerial_garden`, que las capturas estén completas, que cada modo sea el esperado y que la carga corresponda a una escena pesada. `tools/benchmarks/compare_mode_benchmark.py <directorio> gpu_submit_cadence` comprueba además igualdad de las otras opciones, correspondencia entre cuadros JSON/CSV y ausencia de shaders/pipelines nuevos.

Los agregados se calculan desde cuadros y duraciones reales; los percentiles usan las series concatenadas. El informe también conserva cargas de dibujo, motivos de submit, lecturas y tiempos del hilo de render. Las esperas acumuladas entre hilos no se suman como duración de un cuadro. No declarar ganancia únicamente porque disminuya un contador.

El diagnóstico por shader está apagado en esta comparación. Una medición de escena estable no demuestra por sí sola una mejora del descenso: su captura inicial se conserva como referencia, pero ambos modos se comparan después de llegar al punto inicial.

Validación dirigida: `cadence_runtime` comprueba cuatro cambios de modo, coincidencia entre política y configuración y conservación de tick/command buffer. También pasan las pruebas existentes de cadencia en GPU, oclusiones, stream buffer y AuditProfiler. Evidencia: `_Build/windows/cadence_ab_tests.log`.

## Resultado medido el 23 de noviembre de 2026

Ejecución `_Build/windows/stage8_4f_runs/20261123_181815_705_A`, PID 5104. Un solo proceso y ejecutable, SHA256 `F4F2B7575DBE6DC9DC244AA737B4EEB55898CFC7D17066AA7651F5F4052CD4E9`. Cierre normal, salida 0, a las 18:25:49. El directorio conserva `comparison.json`, cuatro JSON/CSV con sus series por cuadro, captura de descenso, logs, secuencia y metadatos del proceso.

| Muestra, 40 segundos | Cadencia | FPS |
|---|---|---:|
| control1 | Apagada | 16,24 |
| candidate1 | Encendida | 18,61 |
| candidate2 | Encendida | 18,37 |
| control2 | Apagada | 15,95 |

| Agregado por condición | Apagada | Encendida |
|---|---:|---:|
| Cuadros / segundos | 1288 / 80,030 | 1480 / 80,047 |
| FPS efectivos | 16,09 | 18,49 |
| p95 / p99 (ms) | 67,34 / 68,01 | 66,78 / 66,98 |
| Dibujos por cuadro | 5059,05 | 4990,11 |
| Tiempo CPU del hilo de render por cuadro | 46,68 ms | 46,67 ms |
| Esperas de readback por cuadro | 6,53 | 6,53 |
| Tiempo acumulado de readback por cuadro | 15,29 ms | 6,74 ms |
| Submits por cuadro | 16,54 | 54,32 |
| Shaders / pipelines nuevos | 0 / 0 | 0 / 0 |

La ganancia conjunta observada es **14,88 %**. Ambas muestras encendidas superan a ambas apagadas, y al desactivar la política al final reaparece el rendimiento anterior. Las otras opciones coinciden. La carga de dibujos difiere un 1,36 %; la escena contiene animaciones, por lo que no es una reproducción idéntica de cada cuadro ni una garantía para todo el juego.

La cantidad de readbacks prácticamente no cambia. El beneficio procede de reducir su latencia entregando trabajo antes: el tiempo CPU del hilo de render permanece casi idéntico y el tiempo de espera baja. Los tiempos acumulados de esperas siguen siendo contabilidad de hilos; no sumarlos al tiempo CPU para reconstruir un cuadro. El coste sostenido de CPU restante explica por qué esta mejora no alcanza por sí sola los 30 FPS. Los p95/p99 mejoran poco: no se presenta como una solución completa a los tirones.

No se desactivaron efectos ni se cambiaron calidad, resolución, relojes guest, VBlank o audio entre condiciones. Esta comparación mide el punto inicial después de aterrizar; no demuestra una mejora A/B del descenso ni de una interacción con esponjas.

## Aplicación en el launcher

Se agregó **Send graphics work earlier** en Graphics & display. Se guarda y copia mediante el sistema existente de ajustes por juego y genera la opción CLI positiva o negativa de forma explícita.

Se compiló y verificó el launcher visualmente, se activó únicamente para Astro Bot y se guardó desde el diálogo del juego. Se comprobó la persistencia en `C:/ProgramData/Kyty/Kyty.ini`: `GameConfigurations/1/custom_settings=true` y `gpu_submit_cadence_enabled=true`, con ruta PPSA21567; el valor global permanece `false`. Se conservaron las otras selecciones del usuario, incluida su resolución de ventana. Los resultados anteriores corresponden a la configuración del benchmark, no constituyen una medición adicional de esa configuración de launcher.

Se actualizaron tanto `_Build/windows/launcher/kyty_launcher.exe` como el paquete `_Build/windows/install/`, con sus dependencias. El launcher sigue apuntando a `_Build/windows/kyty_emulator.exe`. El emulador de prueba y el launcher de verificación se cerraron normalmente; no se dejaron procesos temporales de la tarea abiertos.

Validación final: 8/8 CTest dirigidos aprobados, 5/5 pruebas del analizador, compilación Release del launcher y comprobación visual del guardado. La opción permanece apagada por defecto para otros juegos; el mecanismo y su ajuste individual están disponibles para cualquier título.

Al compilar el launcher se volvió a enlazar su dependencia del emulador tras cambios de formato en los archivos del cambio dinámico. El ejecutable entregado tiene SHA256 `F99CE0A9E06276056B4C608240336688620341D840751CFD617FE1C37F45EAC3`; los resultados A/B corresponden al hash `F4F2...` indicado arriba. Los cambios posteriores de lógica corresponden al launcher y al guardado de la opción; no se repitió el benchmark completo tras ese enlace. Se repitieron y aprobaron los 8 CTest dirigidos después de preparar la entrega (`cadence_delivery_tests.log`). Las copias del emulador y launcher en el paquete local coinciden con sus respectivas salidas de compilación.
