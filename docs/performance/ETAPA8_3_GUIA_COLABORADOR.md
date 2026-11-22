# KytyPS5 — Guía del Colaborador: Etapa 8.3 (Presets, Launcher y Benchmarks)

Esta guía documenta los pasos necesarios para que cualquier desarrollador o colaborador pueda compilar el emulador, verificar la ausencia de regresiones visuales y ejecutar la batería de pruebas de la **Etapa 8.3** en Astro Bot (`PPSA21567`).

---

## 1. Estado de Git y Rama de Trabajo

Asegúrese de estar ubicado en la rama específica de la etapa:
```cmd
git branch --show-current
```
Debe indicar: `etapa8_3_launcher_presets_astrobot`.

---

## 2. Compilación y Validación de Tests

### Compilación en Release con Ninja:
```cmd
cmake -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _Build/windows
```

### Ejecución de Tests Unitarios:
```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|stream_buffer_ring)"
```
Ambos tests deben arrojar `Passed (100% tests passed)`.

---

## 3. Script Interactivo de Benchmarks (`stage8_3_astrobot_configuration_benchmark.cmd`)

Para realizar pruebas deterministas y reproducibles, ejecute en la raíz del repositorio:
```cmd
stage8_3_astrobot_configuration_benchmark.cmd
```

El script presenta un menú interactivo con las 12 opciones requeridas:

1. **Configuración Actual del Ejecutable (Preset Optimizado):**
   - Ejecuta con: `--descriptor-cache`, `--ensure-pipeline-cache`, `--pipeline-key-cache`, `--pipeline-cache-mru`, `--redundant-bind-elision`, `--gpu-submit-cadence`, `--batch-hot-readbacks`, `--partial-buffer-refresh`, con `--no-draw-state-dirty-check`.
2. **Configuración Equivalente desde Launcher:**
   - Emula exactamente los argumentos que genera `kyty_launcher.exe` con sus valores por defecto.
3. **Preset Astro Bot Optimizado:**
   - La combinación recomendada para máxima fluidez preservando 100% la calidad visual original.
4. **Preset Conservador (Control / Baseline):**
   - Desactiva todas las cachés experimentales (`--no-descriptor-cache`, `--no-pipeline-key-cache`, etc.) para verificar posibles regresiones.
5. **Shader Cache Fría:**
   - Ejecuta con `--no-shader-cache` para medir la sobrecarga pura de compilación durante el juego.
6. **Shader Cache Caliente:**
   - Ejecuta con `--shader-cache` habiendo precalentado los pipelines para medir el rendimiento limpio del motor.
7. **Configuración Gráfica Original (Modo A):**
   - Calidad de referencia de PS5 sin ningún tipo de parche (Global Illumination activa).
8. **Configuración con Parches Explícitos (Modo B):**
   - Carga el parche `_Patches/PPSA21567.json` (si está presente) para evaluar el impacto de desactivar GI probes.
9. **Comparación de Buffers:**
   - Compara el ring buffer estándar (64 MiB) frente a `--extended-stream-buffer` (256 MiB).
10. **Comparación de Flags de Renderizado:**
    - Menú rápido para activar/desactivar flags individuales en pruebas A/B.
11. **Exportar Resultados:**
    - Genera resúmenes comparativos en la carpeta de telemetría.
12. **Salir.**

---

## 4. Protocolo de Captura y Verificación en Astro Bot

1. Inicie el juego con la opción deseada del menú.
2. Cargue la partida guardada en **Sky Garden**.
3. Sitúe la nave o al personaje en el punto de inicio.
4. Para la **Vista Amplia**: apunte la cámara hacia el centro del archipiélago mostrando la mayor cantidad de islas y objetos flotantes.
5. Presione **F9** para iniciar la captura de telemetría de 30 segundos (o utilice la opción con temporizador automático de 30 s).
6. Al completarse la captura, los archivos `stage8_3_*.json` y `stage8_3_*.csv` se guardarán en la raíz del proyecto.
7. Compruebe la fidelidad visual:
   - Verifique que **NO aparezcan sombras proyectadas desde el cielo abierto**.
   - Verifique que **NO se produzcan destellos o parpadeos violentos de luz**.
   - Verifique que el agua y las cascadas se rendericen de forma fluida y coherente.
