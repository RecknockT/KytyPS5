# KytyPS5 — Etapa 8: Validación Técnica y de Integración

## 1. Validación de Compilación

La integración completa de la Etapa 8 se compiló limpiamente en Windows 11 con Clang/LLVM y MSVC Runtime mediante Ninja:
- **Herramienta**: Ninja / Clang-cl (LLVM 20+) / MSVC 14.44.
- **Configuración**: Release con símbolos y optimizaciones completas (`_Build/windows`).
- **Artefactos Verificados**:
  - `kyty_emulator.exe` (emulador principal)
  - `kyty_launcher.exe` (launcher Qt6)
  - Bibliotecas estáticas del núcleo: `common.lib`, `graphics.lib`, etc.
  - Binarios de pruebas unitarias: `AuditProfilerTests.exe`, `StreamBufferRingTests.exe`, etc.

---

## 2. Ejecución y Resultados de Suites de Pruebas Unitarias

Se ejecutó la suite de tests automatizados mediante CTest:

```text
Test project C:/Users/Recko/Documents/github/kyty/_Build/windows
    Start  9: emulator_gpu_submit_cadence_cli
1/6 Test  #9: emulator_gpu_submit_cadence_cli ..............   Passed    0.04 sec
    Start 12: audit_profiler
2/6 Test #12: audit_profiler ...............................   Passed    0.04 sec
    Start 61: shader_recompiler_submit_cadence
3/6 Test #61: shader_recompiler_submit_cadence .............   Passed    0.03 sec
    Start 62: shader_recompiler_submit_cadence_gpu
4/6 Test #62: shader_recompiler_submit_cadence_gpu .........   Passed    0.83 sec
    Start 63: shader_recompiler_submit_cadence_occlusion
5/6 Test #63: shader_recompiler_submit_cadence_occlusion ...   Passed    0.39 sec
    Start 82: stream_buffer_ring
6/6 Test #82: stream_buffer_ring ...........................   Passed    0.43 sec

100% tests passed out of 6
Total Test time (real) = 1.79 sec
```

### Pruebas Específicas Agregadas en la Etapa 8
En [tests/AuditProfilerTests.cpp](file:///C:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp):
1. **`PipelineStagesTiming`**: Valida que `RecordPipelineStage()` acumula conteos, tiempos totales, máximos y promedios con exactitud para las 13 etapas del pipeline.
2. **`StreamBufferAudit`**: Valida el registro de asignaciones, detección de wraparounds, cálculo de high-water mark y eventos de espera.
3. **`HitchTraceCorrelation`**: Valida que la correlación temporal entre frames lentos (\(\ge 100\) ms) y el búfer circular de trazas identifica correctamente el bloqueador dominante (`StreamBufferWait`, `TimelineWait`, etc.), la duración del bloqueo, el tick, la dirección y el nivel de confianza ("High", "Medium", "Low").
4. **`JsonCsvExportStructure`**: Valida que los esquemas JSON y CSV incluyan las nuevas secciones: `pipeline_stages`, `cpu_gpu_separation`, `stream_buffer_audit` y `detailed_hitches`.

---

## 3. Matriz de Validación A/B y Control

Se verificó el funcionamiento de todos los modificadores CLI mediante `kyty_emulator.exe --help` y pruebas cruzadas:

| Flag CLI | Propósito | Estado por Defecto | Flag de Control Negativo |
|---|---|:---:|---|
| `--pipeline-key-cache` | Omite reconstrucción redundante de pipeline keys | Activado | `--no-pipeline-key-cache` |
| `--draw-state-dirty-check` | Omite recálculo de render targets si no cambiaron | Activado | `--no-draw-state-dirty-check` |
| `--redundant-bind-elision` | Omite binds de Vulkan idénticos (Etapa 7) | Activado | `--no-redundant-bind-elision` |
| `--pipeline-cache-mru` | Fast-path MRU en caché de pipelines (Etapa 7) | Activado | `--no-pipeline-cache-mru` |
| `--gpu-submit-cadence` | Submits periódicos de GPU (Etapa 5) | Opcional | (sin flag) |
| `--extended-stream-buffer` | Buffer de streaming de 256 MiB | Desactivado | (sin flag) |

---

## 4. Preservación de la Corrección de Emulación

- **Cero Modificaciones en Temporización Simulada**: No se alteraron relojes guest, frecuencias virtuales de VBlank, lógica de flip queues ni reproducción de audio.
- **Invarianza Gráfica**: La comprobación de dirty state en `PrepareDrawRenderState` y la clave de pipeline en `GetGraphicsPipeline` invalidan inmediatamente sus cachés locales ante cualquier cambio en registros de hardware o punteros de memoria, garantizando que ninguna alteración de estado pase inadvertida al driver de Vulkan.
