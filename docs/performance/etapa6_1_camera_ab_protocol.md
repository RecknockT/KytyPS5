# KytyPS5 — Etapa 6.1: Protocolo de Prueba A/B de Cámara en Astro Bot

Este documento establece el protocolo experimental estandarizado y reproducible para realizar la prueba A/B de orientación de cámara en Astro Bot (`PPSA21567`), diseñada para aislar el cuello de botella gráfico y verificar la relación entre los FPS y la velocidad de simulación.

---

## 1. Justificación y Objetivos de la Prueba A/B

### Observación del Usuario:
- **Vista Amplia:** Cuando la cámara enfoca el horizonte y gran parte del mapa, los FPS caen de manera acusada y la velocidad del juego parece descender a cámara lenta.
- **Vista Reducida:** Cuando la cámara apunta hacia el suelo o hacia un muro cercano (reduciendo los objetos visibles), los FPS aumentan sustancialmente.
- **Audio:** La música de fondo mantiene su velocidad inalterada en ambas situaciones.

### Hipótesis a Contrastar:
1. **Hipótesis de Pacing:** Si los FPS suben (por ejemplo, a 30 o 40 FPS), ¿aumenta proporcionalmente la tasa de flips (`flip_rate_hz`) y la velocidad aparente del juego?
2. **Hipótesis del Cuello de Botella Gráfico:** ¿La caída de rendimiento en vista amplia se debe a mayor cantidad de llamadas de dibujo (CPU), saturación de sombreadores/píxeles (GPU), aumento de transferencias/write faults o contención de sincronización?

---

## 2. Configuración Fija de la Prueba

Para garantizar comparabilidad absoluta con los resultados de las Etapas 4.3 y 5, las siguientes opciones del motor se mantienen invariables:

```text
gpu_submit_cadence = true
gpu_submit_cadence_interval_us = 1000 (1.0 ms)
partial_buffer_refresh = true
extended_stream_buffer = false
batch_hot_readbacks = false
shader_cache = true
audio_compile_hold = true
perf_overlay = true
audit_faults = true
audit_manual_trigger = true
audit_capture_duration = 30 segundos
```

---

## 3. Protocolo de Ejecución Paso a Paso

El script interactivo [stage6_1_camera_ab.cmd](file:///c:/Users/Recko/Documents/github/kyty/stage6_1_camera_ab.cmd) automatiza el lanzamiento con los parámetros precisos.

### 3.1. Captura A — Vista Amplia (`stage6_1_camera_wide.json` / `.csv`)

1. Ejecutar `stage6_1_camera_ab.cmd` y seleccionar la opción **[1]**.
2. El emulador se iniciará en modo **WARMUP** (el profiler no registrará métricas durante la carga).
3. Ingresar al nivel de prueba interactivo (**Aztec Sky**).
4. Posicionar a Astro Bot en el punto de prueba establecido en las etapas anteriores.
5. **No mover al personaje.**
6. Orientar la cámara hacia el horizonte, abarcando la mayor cantidad de mapa, estructuras y elementos dinámicos visibles.
7. **Esperar entre 20 y 30 segundos** con la cámara fija para permitir la estabilización de texturas y el enfriamiento de la compilación de shaders.
8. Presionar la tecla **F9**. El overlay mostrará `[F9] Audit: RECORDING (30s)`.
9. Durante los 30 segundos, mantener el mando y la cámara completamente inmóviles.
10. Al cumplirse los 30 segundos, el emulador exportará automáticamente `stage6_1_camera_wide.json` y `stage6_1_camera_wide.csv`, mostrando `COMPLETED` en pantalla.
11. Cerrar la ventana del emulador.

---

### 3.2. Captura B — Vista Reducida / Suelo (`stage6_1_camera_ground.json` / `.csv`)

1. Ejecutar nuevamente `stage6_1_camera_ab.cmd` y seleccionar la opción **[2]**.
2. El emulador se iniciará en modo **WARMUP**.
3. Ingresar a la **misma escena de Aztec Sky**, situando a Astro en la **exacta misma posición** que en la Captura A.
4. **No mover al personaje.**
5. Inclinar la cámara apuntando **completamente hacia el suelo** o hacia un lateral ciego, de modo que solo se visualice la textura del suelo inmediatamente adyacente a Astro Bot y se excluya toda la geometría del mapa de fondo.
6. **Esperar entre 20 y 30 segundos** para estabilizar el estado del motor.
7. Presionar la tecla **F9**.
8. Mantener la escena inmóvil durante los 30 segundos de captura activa.
9. Al finalizar, se exportarán automáticamente `stage6_1_camera_ground.json` y `stage6_1_camera_ground.csv`.
10. Cerrar el emulador.

---

## 4. Archivos de Salida Requeridos para el Diagnóstico

Al finalizar ambas pruebas, deberán existir en el directorio raíz:
- `stage6_1_camera_wide.json`
- `stage6_1_camera_wide.csv`
- `stage6_1_camera_ground.json`
- `stage6_1_camera_ground.csv`

Estos archivos permitirán ejecutar la matriz comparativa de cuellos de botella definida en la siguiente sección.
