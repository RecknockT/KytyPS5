# Binding reuse: medición real de Sky Garden

El 23 de noviembre de 2026 la optimización no mostró mejora de FPS. Permanece apagada por defecto.

## Método y evidencia

La repetición automática entró a Sky Garden, mantuvo X tres segundos para iniciar el descenso y dejó al personaje quieto en el punto inicial. Se midieron cuatro ventanas de 40 segundos en orden ABBA: apagada, encendida, encendida, apagada. No hubo entradas de control durante las cuatro ventanas. La captura de descenso es informativa y no forma parte de esta comparación de escena estable.

Una sola ejecución, PID 28576, directorio `_Build/windows/stage8_4f_runs/20261123_124011_731_A`. SHA256 del ejecutable: `51F0230CBE7C17F1931B7106D660A6DC9C1B7776AE200AB0D0912F0B4712EBA1`. Se comprobó igualdad de configuración salvo `binding_reuse`. El cambio se aplica al iniciar una operación gráfica. No se alteraron relojes guest, VBlank ni audio. El proceso terminó normalmente a las 12:47:25, salida 0.

El directorio contiene `run.json`, `replay.json`, `source.patch`, `process.jsonl`, logs y las capturas JSON/CSV con series por cuadro. `comparison.json` contiene los agregados calculados desde esas capturas. Los FPS conjuntos son la suma de cuadros dividida por la suma de duraciones; p95/p99 conjuntos se calculan sobre las series por cuadro.

## Resultados

| Ventana | Opción | Cuadros | FPS | p95 (ms) |
|---|---|---:|---:|---:|
| control1 | Apagada | 640 | 15,99 | 67,36 |
| candidate1 | Encendida | 633 | 15,82 | 67,35 |
| candidate2 | Encendida | 646 | 16,15 | 67,41 |
| control2 | Apagada | 675 | 16,86 | 66,93 |

| Métrica conjunta | Apagada | Encendida |
|---|---:|---:|
| Cuadros / segundos | 1315 / 80,049 | 1279 / 80,030 |
| FPS | 16,43 | 15,98 |
| p95 / p99 (ms) | 67,12 / 67,75 | 67,39 / 68,07 |
| Dibujos por cuadro | 5067,62 | 4957,25 |
| Preparación de descriptores (ms/cuadro) | 11,40 | 11,52 |
| Preparación de estado de render (ms/cuadro) | 26,37 | 27,14 |
| Reservas del stream buffer (KiB/cuadro) | 38735,52 | 39651,12 |
| Shaders compilados / pipelines creados | 0 / 0 | 0 / 0 |

La diferencia observada de FPS es −2,71 %. Los controles varían de 15,99 a 16,86 FPS y la carga de dibujos conjunta difiere un 2,18 %: esta sesión no demuestra una regresión general ni equivalencia exacta entre escenas animadas. Sí descarta presentar esta prueba como una ganancia de rendimiento.

La ruta encendida registró 35.370.037 reutilizaciones de rangos, 35.370.025 búsquedas, 13.318.279 samplers y 260.732 uploads, evitando 12.689.888 bytes de uploads en 1279 cuadros. Los aciertos existen, pero no redujeron los tiempos totales medidos. Los bytes evitados son unos 9,9 KB por cuadro frente a decenas de MiB de reservas de streaming; estas reservas no equivalen necesariamente a bytes transferidos. Los tiempos del hilo gráfico del host tampoco son mediciones nativas de tiempo GPU.

## Validación y repetición

Compilación Release correcta. Pasaron los tres CTest dirigidos: `audit_profiler`, `binding_reuse` y `stream_buffer_ring`, incluyendo el cambio de modo en el límite de operación. Evidencia: `_Build/windows/binding_reuse_replay_final_build.log` y `_Build/windows/binding_reuse_replay_tests.log`. El fallo HTile de la prueba adicional está documentado con su control en [el informe de implementación](binding_reuse_2026_11_23.md).

Para repetir desde la raíz del proyecto:

```powershell
.\stage8_binding_reuse_auto.ps1
```

El script navega, inicia el descenso, captura las cuatro ventanas y cierra el emulador en una sola sesión. La ruta depende de la partida y posición del mapa; sus condiciones y validaciones están en [la guía automática](astro_automatic_binding_benchmark.md). La medición sintética de preparación aislada no se convierte en una mejora de FPS del juego.
