# Prueba automática de Sky Garden en una sola sesión

```powershell
.\stage8_binding_reuse_auto.ps1
```

Para revisar botones y tiempos sin abrir el juego:

```powershell
.\stage8_binding_reuse_auto.ps1 -Preview
```

La secuencia está en `tools/benchmarks/astro_sky_garden_replay.json`. Cuenta tiempo real del host desde la entrada al bucle de ventana, después de la preparación inicial de shaders. Usa el archivo de partida 1, entra a Gorilla Nebula y Sky Garden, mantiene Cross/X durante tres segundos para iniciar el descenso y deja al personaje en el punto inicial. Las direcciones se mantienen medio segundo. No requiere joystick ni teclado físico: reproduce los controles dentro del emulador, sin automatización del escritorio.

Es una ruta específica de Astro Bot PPSA21567 01.019.000 con la partida y posición de mapa utilizadas en estas pruebas. Si cambia ese contexto, revisar la secuencia; no asumir que una espera fija garantiza haber llegado al nivel.

La captura de descenso es informativa. Después hay cuatro ventanas de 40 segundos sin más entradas de control: A1 apagada, B1 encendida, B2 encendida y A2 apagada. Entre los cambios hay cinco segundos de estabilización. La solicitud cambia de modo al iniciar una operación gráfica, sin modificar el estado guest ni eliminar sincronización. La última muestra vuelve a la ruta de control.

Solo se cambia `binding_reuse`. Se conserva el mismo proceso, ejecutable, cámara, cachés, resolución, VBlank y audio. La repetición no modifica relojes emulados. El emulador se cierra normalmente al terminar, siete minutos y treinta segundos después de iniciar la repetición, más la preparación anterior a ella.

Los JSON, CSV y series por cuadro se guardan por separado como `control1`, `candidate1`, `candidate2` y `control2` bajo el directorio de ejecución en `_Build/windows/stage8_4f_runs/`. El wrapper rechaza capturas incompletas, modos incorrectos y ventanas con menos de 3500 dibujos por cuadro; además hay que comprobar visualmente la escena y comparar cargas de dibujo y ausencia de nuevas compilaciones.

`KYTY_BENCH_REPLAY` activa el reproductor solo en el proceso de prueba. Sin esa variable no reproduce controles ni cambia opciones. Al salir libera todas las teclas simuladas. Se retiró el prototipo temporal que alargaba las pulsaciones en `controller.cpp`: la repetición envía explícitamente down/up, por lo que no cambia el comportamiento habitual de los controles.

El guardado normal del juego puede actualizarse al entrar al nivel; el script conserva esos cambios. El límite externo del proceso es de 700 segundos, con cierre normal primero y terminación forzada solo si ese proceso propio no responde.

Validada el 23 de noviembre de 2026 en la ejecución `20261123_124011_731_A`: completó el descenso, las cuatro capturas y el cierre normal (salida 0). Resultado: 16,43 FPS apagada y 15,98 FPS encendida; no se habilita la optimización por defecto. Ver [informe de medición](binding_reuse_gameplay_validation.md).

La secuencia se ajust� despu�s de detectar que la confirmaci�n de partida pod�a llegar antes de terminar la animaci�n del men� en arranques lentos. El benchmark ABBA del informe us� la secuencia anterior guardada en su propio `replay.json`; no se reescribi� ese artefacto. Ahora se confirma la partida a los 65 segundos y se empieza la navegaci�n del mapa a los 110 segundos. El wrapper comprueba tambi�n `StartLevel aerial_garden` en el log.
