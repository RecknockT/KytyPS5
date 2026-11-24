# 8.4F — aceptación de la reconstrucción

## Alcance

Reconstrucción sobre `14185ebd9feed76015ebf8760ffad391e3c3ef50`, sin recuperar el commit perdido. No hay commit nuevo ni push. CpSequencer, DrawPrep y Draw State Dirty Check conservan sus valores predeterminados desactivados.

## Diferencias deliberadas respecto al informe perdido

- Se conserva la capacidad de 64 snapshots; las pruebas usan incluso 4 para forzar saturación. Aumentarla a 256 no resolvería ciclos de espera ni tickets saltados.
- Cada una de las cinco operaciones consumidoras de registros tiene un snapshot CP, independiente de DrawPrep. El resolver libera su ticket después del último uso.
- Los draws usan ese snapshot también al hacer fallback; los indirectos ejecutan sobre un CommandProcessor local y devuelven los registros SGPR modificados al productor mediante handshake.
- El estado de registros del scheduler GPU no apunta al estado mutable del productor.
- Se incorporan correcciones adicionales de head de DrawPrep, notificaciones CV y cesión del resolver para esperas entre colas.
- La regresión MRU se ejecuta en el fixture Vulkan real de ShaderRecompilerComputeTests, mediante --shader-cache-only, para probar la PipelineCache de producción en lugar de un mapa simulado dentro del test de transporte CP.

## Estado de aceptación

- Compilación Release aprobada para emulador, tests CP/DrawPrep y fixture Vulkan.
- Suite solicitada: 4/4 aprobadas (audit_profiler, cp_sequencer, draw_prep y stream_buffer_ring). El regex no incluye pruebas independientes pipeline_cache/cadence porque no están registradas con esos nombres; la cadencia se comprueba dentro del fixture GPU y MRU mediante --shader-cache-only.
- cp_sequencer_tests y draw_prep_tests ejecutados directamente, exit 0. Incluyen cancelación de saturación y la espera circular worker/readback/resolver.
- --shader-cache-only: exit 0, incluyendo la regresión MRU real, ShaderStore y VertexPushTopCache.
- A: alcanzó título y transición; salida 0 en prueba de 120 s.
- B: alcanzó mapa de planetas visible; salida 0 en prueba de 120 s.
- C: tras dos fallos reproducidos y corregidos (metadatos reciclados y readback síncrono), alcanzó mapa de planetas visible y terminó con salida 0 en prueba de 120 s. La repetición tras reforzar el orden de compute, `20261123_011526_986_C`, terminó con salida 0 a los 90 s y registró `worldmap` y `transition_next`. El mayor intervalo muestreado sin cambio de executed fue 4,021 s; no equivale a una medida de fluidez.
- Después de esa repetición se añadió una comprobación de parada en Publish para rechazar reservas cuando el Engine ya está detenido. La compilación y las pruebas CP, DrawPrep, ctest (4/4) y el fixture Vulkan se repitieron correctamente después de este último cambio; no se repitió el juego por esa comprobación de cierre.
- No se afirma ninguna mejora de FPS. Los arranques con caché desactivada incluyen compilación y no constituyen una comparación de gameplay.

## Causas confirmadas y límites

Las pruebas de saturación confirmaron el avance indebido de head sobre una reserva no publicada. El overflow de metadatos se reprodujo realmente en C. El bloqueo posterior registró 88,5 s sin progreso y cuatro workers Claimed; el código de readback síncrono cierra el ciclo de espera, la regresión dirigida termina al atender esa cola y la prueba real posterior superó el punto de bloqueo.

No es posible atribuir con certeza el congelamiento del commit perdido a una sola de estas causas. No se probó una sesión prolongada, todos los juegos ni equivalencia visual completa. Los modos experimentales siguen apagados por defecto.

## Ejecución manual

`stage8_4f_gameplay_benchmark.cmd` permite A/B/C. En Sky Garden, usar F9 para capturar 30 s y F10 para marcar fases. Mantener cámara, configuración y estado de caché comparables. Revisar frametimes y cuadros reales en la telemetría. Cerrar una prueba antes de iniciar la siguiente.

Los registros quedan bajo `_Build/windows/stage8_4f_runs/`. Las pruebas reales pueden actualizar los archivos de guardado del juego; esos cambios se conservan separados del código, sin incorporarlos a un commit.

Para la transición de aterrizaje:

```powershell
.\stage8_4f_run.ps1 -Profile A -CaptureSeconds 120
```

Iniciar F9 antes de entrar al planeta. F10 avanza por WorldEntryPrep, DescentStart, ActiveDescent, GroundApproach, Touchdown, EarlyGameplay y Gameplay. Marcar cada fase cuando ocurra y repetir el mismo recorrido con B y C. No alterar resolución, temporizadores ni calidad entre perfiles. El análisis distingue I/O lógico, compilación, esperas, CPU y compromiso de memoria. Ya se realizó una captura de entrada y aterrizaje con A y caché activada; véase [la investigación del 23 de noviembre](landing_investigation_2026_11_23.md). La optimización y un A/B del mismo recorrido siguen pendientes; no se inició la Etapa 8.5.

## Entrega para revisión

- SHA inicial y final: `14185ebd9feed76015ebf8760ffad391e3c3ef50`, con cambios locales sin commit.
- Código: commandProcessor.h, cpOps.{h,cpp}, cpSequencer.{h,cpp}, graphicsRun.{h,cpp}, drawPrep.{h,cpp}, pipelineCache.cpp.
- Pruebas: CpSequencerTests.cpp, DrawPrepTests.cpp, ShaderRecompilerComputeTests.cpp.
- Herramientas: stage8_4f_gameplay_benchmark.cmd, stage8_4f_run.ps1 y stage8_4f_analyze.ps1.
- Informes: este documento, etapa8_4f_concurrency_validation.md y etapa8_4f1_cpsequencer_boot_hang.md.
- Sin ramas nuevas, commit ni push. Revisar únicamente código, tests, herramientas e informes; los cambios de _SaveData provienen de las ejecuciones reales y no forman parte de la corrección.
