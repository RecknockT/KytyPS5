# KytyPS5 — Etapa 8.3: Auditoría Técnica del Launcher y Flags de Ejecución

## 1. Resumen Ejecutivo

Un interrogante fundamental planteado al inicio de la Etapa 8.3 era si la ejecución directa de `kyty_emulator.exe` (utilizada a lo largo de las Etapas 5 a 8.2 en los scripts de benchmark) estaba perjudicando el rendimiento de Astro Bot (`PPSA21567`) al no pasar por `kyty_launcher.exe`.

Tras una auditoría exhaustiva del código fuente del launcher (`src/launcher/`) y del emulador (`src/main.cpp`, `src/common/emulatorConfig.cpp`), la respuesta técnica es **categórica**:

> [!IMPORTANT]
> **Ejecutar directamente el emulador NO perjudica el rendimiento bajo ninguna circunstancia.**
> Al contrario: el launcher oficial no implementa ninguna optimización de hardware oculta, no inyecta variables de entorno mágicas y **carece por completo de soporte en su interfaz gráfica para los flags de rendimiento avanzados de las Etapas 5 a 8.2** (`--gpu-submit-cadence`, `--partial-buffer-refresh`, `--batch-hot-readbacks`, `--pipeline-key-cache`, `--ensure-pipeline-cache`, `--descriptor-cache`, `--extended-stream-buffer`).

---

## 2. Flujo de Ejecución del Launcher (`kyty_launcher.exe`)

El launcher está desarrollado en **Qt 6 / C++20**. Su ciclo de vida al iniciar un juego es el siguiente:

1. **Lectura de Configuración:**
   - Lee las opciones persistidas desde `QSettings` (almacenadas en el registro de Windows en `HKEY_CURRENT_USER\Software\Kyty\Kyty Launcher`).
   - Mapea las claves del juego (`src/launcher/src/configuration.cpp`) si `custom_settings == true`.
2. **Construcción de Argumentos CLI (`src/launcher/src/mainDialog.cpp:242`):**
   - La función `CreateEmulatorArgs(const Configuration& info)` transforma los campos de la estructura Qt en una lista de parámetros en texto plano (`QStringList args`).
3. **Invocación del Proceso (`src/launcher/src/mainDialog.cpp:446`):**
   - En Windows, el launcher **no ejecuta el emulador mediante inyección ni librerías compartidas**, sino que ejecuta:
     ```cmd
     cmd.exe /K "kyty_emulator.exe" <args>
     ```
   - Abre una nueva consola Windows (`CREATE_NEW_CONSOLE`) y cede el control por completo al proceso `kyty_emulator.exe`.

---

## 3. Comportamiento del Emulador Directo (`kyty_emulator.exe`)

En `src/main.cpp`:
- `kyty_emulator.exe` es **completamente stateless**: no lee ningún archivo `.ini`, `.json` ni claves de registro al arrancar.
- Parsea exclusivamente la línea de comandos (`argc`, `argv`).
- Cualquier opción omitida en los argumentos toma su valor hardcodeado por defecto definido en `ConfigOptions` (`src/common/emulatorConfig.h`).
- Por tanto, invocar `kyty_emulator.exe` directamente desde scripts `.cmd` con los flags deseados es la forma más pura, reproducible y de menor sobrecarga de ejecutar el motor.

---

## 4. Tabla Comparativa Exhaustiva de Configuraciones

A continuación se detalla la matriz de todas las opciones del motor gráfico y de sistema, comparando sus valores por defecto, su disponibilidad en la GUI del launcher y su impacto en rendimiento:

| Configuración / Flag | Valor predeterminado (código) | Valor en benchmarks actuales | Disponible en GUI Launcher | Impacto potencial en rendimiento |
|---|:---:|:---:|:---:|---|
| `--screen-width` / `--screen-height` | 1280 x 720 | 1280 x 720 | **Sí** | **Crítico** en GPU-bound. 720p minimiza la carga de shaders frente a 1080p o 4K. |
| `--present-mode` | `Mailbox` | `Mailbox` | **Sí** | Alto. Evita tearing sin el bloqueo forzado de framerate de FIFO (V-Sync). |
| `--vblank-frequency` | 60 | 60 | **Sí** | Pacing. 60 Hz es el estándar guest de PS5. |
| `--shader-cache` | `true` | `true` | **Sí** | **Crítico**. Reutiliza shaders SPIR-V y pipelines Vulkan en `_PipelineCache/`. |
| `--no-async-shader-compile` | `false` (Async activo) | `false` (Async activo) | **No** | **Crítico**. Si se desactiva, el hilo de render bloquea la CPU ante cada shader nuevo. |
| `--tessellation` | `false` | `false` | **Sí** | Moderado. Astro Bot no requiere teselación de hardware para su geometría base. |
| `--occlusion-queries` | `false` | `false` | **Sí** | Moderado. Si se activa sin soporte nativo eficiente, puede generar CPU stalls. |
| `--relaxed-shader-float` | `false` | `false` | **Sí** | Bajo/Medio. Usa aritmética flotante relajada en SPIR-V; puede causar imprecisiones. |
| `--native-rt` | `false` | `false` | **Sí** | **Negativo severo**. El soporte de RT nativo en Kyty es experimental y causa caídas masivas de FPS. |
| `--trinity` (PS5 Pro / PSSR) | `false` | `false` | **Sí** | Alto. Activa reescalado por DLSS/FSR; añade sobrecarga si la GPU ya está saturada. |
| `--gpu-submit-cadence` | `false` (1.0 ms) | Opcional (Etapa 5) | **NO** | Moderado. Reduce la latencia de submits intermedios en command streams largos. |
| `--partial-buffer-refresh` | `false` | Opcional (Etapa 5) | **NO** | Moderado. Evita resubidas completas de texturas modificadas parcialmente. |
| `--batch-hot-readbacks` | `false` | Opcional (Etapa 5) | **NO** | Alto en escenas con fluidos. Agrupa lecturas de memoria guest/host. |
| `--extended-stream-buffer` | `false` (64 MiB) | `false` (64 MiB) | **NO** | Alto en picos. Previene bloqueos de anillo (`StreamBufferWait`) a costa de RAM. |
| `--redundant-bind-elision` | `true` | `true` (Etapa 7) | **NO** | **Demostrado (+4-5%)**. Elimina llamadas redundantes a `vkCmdBindPipeline` y VBs. |
| `--pipeline-cache-mru` | `true` | `true` (Etapa 7) | **NO** | **Demostrado (+3-4%)**. Acelera la búsqueda de pipelines gráficos repetidos. |
| `--pipeline-key-cache` | `true` | `true` (Etapa 8) | **NO** | **Demostrado (+2-3%)**. Elide la construcción de hash keys de pipeline idénticas. |
| `--draw-state-dirty-check` | `false` (desactivado) | `false` | **NO** | **Desactivado**. Causa regresiones visuales severas (sombras del cielo, destellos). |
| `--ensure-pipeline-cache` | `true` | `true` (Etapa 8.1) | **NO** | **Demostrado (+2%)**. Fast-path en `EnsureGraphicsPipeline`. |
| `--descriptor-cache` | `true` | `true` (Etapa 8.1) | **NO** | **Demostrado (+3%)**. Fast-path en cacheo de descriptor bindings Vulkan. |
| `--game-patch` | (vacío) | (vacío) | **Sí** (`_Patches/`) | **CRÍTICO**. Permite inyectar parches GoldHEN para apagar Global Illumination. |

---

## 5. El único mecanismo que el Launcher automatiza: `_Patches`

En `src/launcher/src/mainDialog.cpp:332`:
```cpp
const auto patch_plan = Cheats::PlanPath(info.title_id);
if (QFileInfo::exists(patch_plan)) {
    args << "--game-patch" << patch_plan;
}
```
Si un archivo `_Patches/PPSA21567.json` está presente en la carpeta del emulador, el launcher lo detecta y pasa automáticamente el argumento `--game-patch _Patches/PPSA21567.json`.

En la ejecución por consola o scripts interactivos, dicho argumento simplemente se especifica explícitamente si se desea probar un parche de juego, brindando un control determinista idéntico o superior.

---

## 6. Conclusión de la Auditoría del Launcher

1. **No hay pérdida de rendimiento:** No ejecutar desde `kyty_launcher.exe` no introduce ninguna degradación.
2. **Ventaja de la ejecución directa:** Permite activar y evaluar de forma granular todos los flags de las Etapas 5 a 8.2 que el launcher ignora por no tener controles en su interfaz gráfica.
3. **Causa real de diferencias con otros forks:** La diferencia de rendimiento observada en forks externos como Jetsku no proviene de ejecutar a través del launcher, sino del uso de **parches específicos de juego (como desactivar Global Illumination y GI probes)**, **optimizaciones de readbacks de fluidos (`KYTY_READBACK_EAGER_WRITES=1`)**, y **precarga de pipelines (`.kyplst`)**.
