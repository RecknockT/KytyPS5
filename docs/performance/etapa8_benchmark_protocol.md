# KytyPS5 — Etapa 8: Protocolo de Benchmarks y Validación Experimental

## 1. Objetivo del Protocolo

Establecer un procedimiento experimental riguroso, reproducible y sin variabilidad para evaluar las optimizaciones de la Etapa 8 y aislar los bloqueos de rendimiento en **Astro Bot** (`PPSA21567`).

---

## 2. Preparación y Requisitos del Entorno

1. **Hardware de Referencia**:
   - AMD Ryzen 7 9700X
   - NVIDIA GeForce RTX 5080 16 GB
   - 32 GB DDR5 6000 MT/s
   - Windows 11
2. **Ubicación de la Escena de Prueba**:
   - Nivel: **Aztec Sky**.
   - Posición del personaje: En la plataforma inicial abierta tras cruzar el primer arco.
3. **Fase de Calentamiento (Warmup Obligatorio)**:
   - Una vez cargado el nivel, dejar al personaje completamente quieto durante **20 a 30 segundos** antes de presionar F9.
   - Esto asegura que todos los pipelines iniciales se encuentren compilados en disco, la memoria de texturas esté subida a VRAM y el profiler no registre artefactos de carga (`Loading Phase`).

---

## 3. Matriz de Pruebas en `stage8_camera_ab.cmd`

El script [stage8_camera_ab.cmd](file:///C:/Users/Recko/Documents/github/kyty/stage8_camera_ab.cmd) ubicado en la raíz del repositorio cuenta con un menú interactivo con 6 opciones de prueba automatizadas:

```text
=====================================================================
   KytyPS5 - Etapa 8: DIAGNOSTICO TEMPORAL, HITCHES Y OPTIMIZACION
=====================================================================

  [1] Vista Amplia: Todas las optimizaciones de Etapa 8 activas
      (--pipeline-key-cache --draw-state-dirty-check)
      Salida: stage8_camera_wide.json / .csv

  [2] Vista Amplia: Control / Baseline sin optimizaciones de Etapa 8
      (--no-pipeline-key-cache --no-draw-state-dirty-check)
      Salida: stage8_camera_wide_baseline.json / .csv

  [3] Vista al Suelo: StreamBuffer normal de 64 MiB
      Salida: stage8_camera_ground_normal.json / .csv

  [4] Vista al Suelo: StreamBuffer extendido de 256 MiB
      (--extended-stream-buffer)
      Salida: stage8_camera_ground_extbuf.json / .csv

  [5] Vista Amplia: Captura detallada de etapas del pipeline (F9)
      Salida: stage8_camera_wide_stages.json / .csv

  [6] Vista al Suelo: Captura detallada de hitches y correlacion de bloqueos (F9)
      Salida: stage8_camera_ground_hitches.json / .csv

  [7] Salir
```

---

## 4. Instrucciones Detalladas de Ejecución

### Prueba 1: Vista Amplia con Optimizaciones Etapa 8
1. Ejecutar `stage8_camera_ab.cmd` y seleccionar la opción **1**.
2. Al ingresar a Aztec Sky, apuntar la cámara horizontalmente hacia el horizonte del nivel (mostrando la totalidad de la geometría y escenarios lejanos).
3. Esperar 20 segundos de estabilización.
4. Presionar la tecla **F9**. La captura correrá durante exactamente 30 segundos.
5. Al finalizar los 30 segundos, el emulador cerrará la sesión de auditoría y guardará automáticamente `stage8_camera_wide.json` y `stage8_camera_wide.csv`.

### Prueba 2: Vista Amplia Control (Baseline)
1. Ejecutar `stage8_camera_ab.cmd` y seleccionar la opción **2**.
2. Mantener exactamente la misma posición de cámara horizontal que en la Prueba 1.
3. Esperar 20 segundos de estabilización.
4. Presionar **F9**. Se generará `stage8_camera_wide_baseline.json` y `.csv`.
5. **Comparativa**: La diferencia directa entre la Prueba 1 y la Prueba 2 mide cuantitativamente la ganancia neta aportada por `--pipeline-key-cache` y `--draw-state-dirty-check`.

### Prueba 3 vs. 4: StreamBuffer Normal (64 MiB) vs. Extendido (256 MiB)
1. Para la Prueba 3 (Opción 3), rotar la cámara completamente hacia abajo enfocando el suelo bajo los pies de Astro.
2. Esperar estabilización y presionar **F9**. Genera `stage8_camera_ground_normal.json`.
3. Para la Prueba 4 (Opción 4), repetir con el buffer extendido activado. Genera `stage8_camera_ground_extbuf.json`.
4. **Comparativa**: Permite verificar la desaparición de los hitches severos de 1,5 segundos en la Opción 3 (`wait_max_ms` de ~209 ms frente a ~947 ms).

### Prueba 5 y 6: Auditoría de 13 Etapas y Correlación de Hitches
- Las opciones 5 y 6 registran trazas de alta densidad para validar la sección `pipeline_stages` y la tabla `detailed_hitches` en los informes generados.
