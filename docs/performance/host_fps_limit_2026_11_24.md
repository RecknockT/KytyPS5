# Límite de presentación del host

`--fps-limit 30` limita las llamadas de presentación a un máximo de 30 por segundo mediante el reloj monotónico del host. `--fps-limit 0` y `--no-fps-limit` lo desactivan. Se admiten valores de 0 a 360; el emulador arranca sin límite si no se especifica.

El script de prueba manual `stage8_live_play.ps1` usa 30 por defecto; `-FrameLimit 0` permite volver a una sesión sin límite. El argumento efectivo queda registrado en `run.json`.

La espera ocurre fuera del mutex de la cola Vulkan. No se modifican relojes guest, frecuencia virtual de VBlank, política de flip queues ni temporización de audio. Los cuadros lentos no generan ráfagas para recuperar tiempo perdido. Es un tope, no una garantía de alcanzar 30 FPS ni una corrección de la velocidad de simulación de un juego que vincule sus pasos a los cuadros.

Pasaron las pruebas de plazos, espera real a 30 FPS, validación de argumentos, desactivación y regresión del recorte de memoria. Registro: `_Build/windows/fps_limit_tests.log`. La prueba de espera real valida nueve llamadas con al menos ocho intervalos de 33.333.334 ns; no representa rendimiento medido de Astro Bot.
