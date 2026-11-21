# KytyPS5 — Etapa 4.2: Estabilización del Rendimiento, Análisis de Stuttering y Optimización del GPU Scheduler

> [!WARNING] **CORRECCIÓN CRÍTICA DE METODOLOGÍA (Resultados Exploratorios — Etapa 4.1)**
> Tras una rigurosa auditoría metodológica, se ha determinado que las ocho capturas de la Etapa 4.1 **NO se realizaron bajo condiciones equivalentes garantizadas**.
> En algunas ejecuciones, la captura comenzó desde el inicio de la aplicación (capturando inicialización, pantallas de carga, subida masiva de assets y compilación de shaders). En otras, la medición comenzó cuando el juego ya se encontraba en gameplay interactivo.
> **Consecuencias metodológicas:**
> 1. Las diferencias de FPS entre configuraciones de la Etapa 4.1 no deben utilizarse como evidencia concluyente de mejora o regresión.
> 2. No se atribuye automáticamente la variabilidad de Cadence + Partial a un problema de sincronización de la arquitectura, ya que puede estar causada por la mezcla de fases de carga vs gameplay.
> 3. Los resultados de la Etapa 4.1 se conservan exclusivamente como referencia histórica y exploratoria.
> 4. Se ha implementado un nuevo sistema de segmentación por fases y el protocolo de benchmarking manual controlado por F9 con calentamiento previo.

---

## 1. Auditoría de las Ocho Capturas Originales: Evidencia de Diferencias de Fase

El análisis minucioso de los datos brutos de las ocho capturas (`stage4_1_*.json` y `stage4_1_*.csv`) revela discrepancias objetivas que demuestran que las ejecuciones midieron diferentes proporciones de carga de recursos y gameplay:

### Evidencias Cuantitativas en los Datos
1. **Discrepancia en Transferencias de Memoria (Baseline Run 1 vs Run 2):**
   * Aunque ambas ejecuciones duraron exactamente **180,06 s** y registraron un número similar de frames (**2.783 vs 2.819**), el volumen de transferencias CPU$\to$GPU difiere radicalmente:
     * **Run 1:** 321.954 MB (115,69 MB/frame).
     * **Run 2:** 750.919 MB (266,38 MB/frame) $\longrightarrow$ **+133,2% de aumento en uploads**.
     * **Readbacks GPU$\to$CPU:** 1.278 MB en Run 1 vs 6.707 MB en Run 2 (**+424,6% de aumento**).
   * Una diferencia de más de 428 GB de subida y 5,4 GB de bajada en el mismo intervalo de tiempo solo puede explicarse por la presencia de una **fase intensiva de streaming/carga de nivel** en Run 2 que no estuvo presente en Run 1.
2. **Divergencia Extrema en la Combinación (Combined Run 1 vs Run 2):**
   * **Total de fotogramas:** 3.624 frames en Run 1 frente a 4.791 frames en Run 2 (**+32,2% de fotogramas**).
   * **P50 Frametime:** **50,05 ms** en Run 1 vs **17,36 ms** en Run 2.
   * **Write faults:** 23.742.750 en Run 1 (6.551,5 faults/f) vs 20.068.354 en Run 2 (4.188,8 faults/f).
   * **Página caliente `0x579803000`:** En Run 1 acumuló **17.451 re-protecciones y 8.583 faults**, mientras que en Run 2 no figuró entre los puntos calientes.
   * **Submissions retenidos por Cadence:** **992.684 retenciones** en Run 1 frente a **410.959** en Run 2 (+141,5%).
   * Esta enorme divergencia indica que Run 1 atravesó una transición o carga pesada con re-protecciones continuas, mientras que Run 2 estuvo dominado por una escena de gameplay con menor rotación de buffers.
3. **Consistencia Relativa en Cadence (Cadence Run 1 vs Run 2):**
   * Cadence registró métricas sumamente similares entre repeticiones (4.298 vs 4.389 frames, 159,9 GB vs 162,3 GB upload, P50 de 17,26 vs 17,14 ms). Sin embargo, hasta no medir bajo el nuevo protocolo estricto, no puede afirmarse si esta consistencia se debió a una casual coincidencia en el punto de captura o a una propiedad intrínseca del scheduler.

---

## 2. Matriz Consolidada de Métricas (Referencia Histórica / Exploratoria)

| Archivo de Captura | Fase | Estado | Duración | Frames | FPS Efect. | Avg (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) |
| :--- | :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `stage4_1_baseline_run1.json` | Mixta/Boot | Finalizado | 180,1s | 2.783 | **15,46** | 64,71 | 66,53 | 68,37 | 83,17 | 87,61 |
| `stage4_1_baseline_run2.json` | Carga Int. | Finalizado | 180,1s | 2.819 | **15,66** | 63,88 | 66,39 | 68,79 | 83,16 | 85,52 |
| `stage4_1_cadence_run1.json` | Gameplay | Finalizado | 180,0s | 4.298 | **23,87** | 40,95 | **17,26** | 68,05 | 83,68 | 2.584,99 |
| `stage4_1_cadence_run2.json` | Gameplay | Finalizado | 180,0s | 4.389 | **24,38** | 40,14 | **17,14** | 67,72 | 83,40 | 2.452,47 |
| `stage4_1_partial_run1.json` | Mixta | Finalizado | 180,0s | 4.199 | **23,32** | 41,85 | **17,09** | 83,17 | 83,74 | 1.600,57 |
| `stage4_1_partial_run2.json` | Mixta | Finalizado | 180,0s | 4.566 | **25,37** | 38,66 | **16,97** | 83,15 | 83,73 | 2.183,43 |
| `stage4_1_combined_run1.json` | Carga/Trans | Finalizado | 180,0s | 3.624 | **20,13** | 49,67 | **50,05** | 66,93 | 81,20 | 1.598,30 |
| `stage4_1_combined_run2.json` | Gameplay | Finalizado | 180,0s | 4.791 | **26,61** | 36,82 | **17,36** | 66,58 | 67,18 | 1.766,66 |

### Métricas de Sincronización y Scheduler por Frame

| Archivo de Captura | Faults/f | VEH ms/f | Upload MB/f | Readback MB/f | Submits/f | Rb Wait ms/f | Rb Lat. (µs) | SB Waits | SB Max ms | Max Frame ms |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `baseline_run1` | 7.981,2 | 43,45 | 115,69 | 0,46 | 24,10 | 18,59 | 2.043 | 0 | 0,0 | 87,6 |
| `baseline_run2` | 7.954,4 | 41,99 | 266,38 | 2,38 | 23,42 | 17,20 | 2.042 | 0 | 0,0 | 85,5 |
| `cadence_run1` | 3.959,2 | 20,14 | 37,21 | 0,77 | 40,00 | 6,23 | 1.308 | 26 | 515,1 | 2.585,0 |
| `cadence_run2` | 3.832,5 | 20,03 | 36,99 | 0,77 | 38,85 | 6,29 | 1.357 | 28 | 515,4 | 2.452,5 |
| `partial_run1` | 3.925,0 | 24,04 | 62,63 | 0,79 | 20,10 | 11,28 | 2.122 | 23 | 565,2 | 1.600,6 |
| `partial_run2` | 3.313,7 | 21,94 | 44,67 | 0,61 | 19,24 | 10,80 | 2.426 | 31 | 556,1 | 2.183,4 |
| `combined_run1` | 6.551,5 | 28,27 | 46,83 | 1,07 | 50,59 | 8,76 | 1.233 | 28 | 507,0 | 1.598,3 |
| `combined_run2` | 4.188,8 | 19,22 | 36,75 | 0,79 | 37,56 | 6,01 | 1.260 | 31 | 562,3 | 1.766,7 |

---

## 3. Arquitectura de Segmentación de Fases en `AuditProfiler`

Para eliminar de raíz la contaminación entre inicialización, carga de texturas y gameplay interactivo, se diseñó e implementó un sistema de fases formal en el emulador:

### Fases de Ejecución (`ExecutionPhase`)
```cpp
enum class ExecutionPhase : uint32_t {
    Initialization = 0, ///< Boot, carga de módulos ELF, creación del dispositivo Vulkan.
    Loading,            ///< Streaming masivo de texturas, compilación JIT de pipelines, pantallas de carga.
    Warmup,             ///< Escena objetivo alcanzada; estabilización de cachés del OS y GPU.
    Gameplay,           ///< Sesión activa de benchmark en gameplay interactivo sostenido.
    Transition,         ///< Transiciones entre niveles, pausas o cinemáticas.
    Count
};
```

### Transiciones de Fase Automáticas y Manuales
1. **Arranque del Emulador (`Initialization`):** Al invocarse `Emulator::Init`, la fase arranca en `Initialization`.
2. **Primer Presentación en Swapchain (`Loading`):** En `Swapchain::Present()`, en el instante en que el primer marco es presentado en pantalla, la fase transiciona automáticamente a `Loading`.
3. **Modo Calentamiento (`Warmup`):** Al utilizar `--audit-manual-trigger`, el profiler arranca en estado `CaptureState::WaitingForHotkey` y fase `Warmup`. El reloj de 180 segundos **NO corre** y los marcos de juego no se registran en la captura.
4. **Inicio Quirúrgico con F9 (`Gameplay`):** Cuando el usuario alcanza la escena objetivo y presiona F9:
   * La fase pasa a `Gameplay`.
   * Se ejecuta un `Reset()` atómico de todos los contadores de fallos, esperas, submits y frametimes.
   * Se muestrean nuevamente los valores base de contadores externos de subida y bajada.
   * Se inicializa el cronómetro en $t=0$.
   * La sesión dura exactamente 180 segundos de gameplay puro.
5. **Métricas Separadas (`phase_segmentation`):**
   * Tanto en JSON como en CSV, las métricas de `Loading` y `Gameplay` se computan y almacenan de forma independiente (`PhaseLoading` vs `PhaseGameplay`), permitiendo auditar la carga sin distorsionar el frametime de juego.

---

## 4. Nuevo Protocolo de Benchmarking Riguroso para Astro Bot

Para garantizar comparaciones científicamente válidas entre configuraciones, se establece el siguiente protocolo estándar obligatorio:

### Procedimiento por Prueba
1. **Ejecutar el script correspondiente** (ej. `stage4_2_baseline_run1.cmd`).
2. **Fase de Inicialización y Carga:** Permitir que el emulador inicie y cargue el juego. El overlay mostrará en amarillo:
   `[F9] Audit: READY (Warmup) - Press F9 in target scene`
3. **Posicionamiento Estricto:** Navegar hasta la escena interactiva designada y ubicar la cámara en el punto y ángulo de referencia acordado.
4. **Fase de Calentamiento (Warmup):** Mantener el personaje y la cámara completamente quietos durante **20 a 30 segundos**. Esto estabiliza las cachés del driver Vulkan, los hilos de audio y el streaming de assets.
5. **Disparo de la Captura (F9):** Presionar **F9**. El overlay cambiará a rojo:
   `[F9] Audit: RECORDING (0s/180s) [Gameplay]`
6. **Medición Ininterrumpida:** Mantener la escena estática durante los 180 segundos de captura.
7. **Exportación Determinista:** Al cumplirse los 180 s, el emulador finaliza automáticamente la captura, exporta el archivo `.json` y `.csv`, y el overlay cambia a verde:
   `[F9] Audit: COMPLETED (Exported)`
8. **Cierre Limpio:** Cerrar el emulador antes de iniciar la siguiente prueba.

### Alternancia de Orden entre Rondas (Anti-Sesgo Térmico y de Caché)
Para evitar que el calentamiento de la GPU o la memoria en caché del sistema operativo beneficien sistemáticamente a las últimas configuraciones ejecutadas, se define un orden invertido:

* **Ronda 1:**
  1. `stage4_2_baseline_run1.cmd` (Baseline)
  2. `stage4_2_cadence_run1.cmd` (Cadence 1 ms)
  3. `stage4_2_partial_run1.cmd` (Partial Refresh)
  4. `stage4_2_combined_run1.cmd` (Combined + Extended Stream Buffer)
* **Ronda 2 (Orden Invertido):**
  5. `stage4_2_combined_run2.cmd` (Combined + Extended Stream Buffer)
  6. `stage4_2_partial_run2.cmd` (Partial Refresh)
  7. `stage4_2_cadence_run2.cmd` (Cadence 1 ms)
  8. `stage4_2_baseline_run2.cmd` (Baseline)

---

## 5. Causa Raíz Confirmada de Stuttering: Agotamiento de StreamBuffer

Independientemente de las fases de juego, la auditoría identificó una causa física y reproducible de tirones severos (hitches de 515–565 ms) en las configuraciones con mayor tasa de cuadros:

1. **Capacidad del Anillo:** `BufferCache` asigna por defecto 64 MiB para uploads dinámicos (`m_stream_buffer`) y 64 MiB para descargas (`m_download_buffer`).
2. **Frecuencia de Agotamiento:** A más de 20 FPS, este espacio se llena en aproximadamente 6 a 8 segundos.
3. **El Bloqueo Sincrónico:** Al envolver el puntero circular, [streamBuffer.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/cache/streamBuffer.cpp#L50-L75) invoca `WaitPendingOperations()`, obligando a la GPU a drenar todas las operaciones pendientes de la generación anterior.
4. **Impacto:** Provoca una parada del hilo CPU de entre **200 y 227 ms** de media y picos de **515 a 565 ms**. Durante este bloqueo, `BufferReadback` y `PriorityOperation` se congelan por la misma duración, produciendo marcos atípicos de hasta **2,5 segundos** en el swapchain.
5. **Mitigación:** Se incorporó la bandera `--extended-stream-buffer`, que cuadruplica el tamaño del buffer a **256 MiB** de subida y **128 MiB** de bajada, espaciando los ciclos de vaciado en un 75%.

---

## 6. Verificación y Tests Unitarios

Tras implementar la segmentación de fases, el disparador manual `--audit-manual-trigger`, y las métricas de hitches, se ejecutaron las pruebas unitarias del proyecto:

```
[==========] Running tests from audit_profiler_tests.exe
[  PASSED  ] 16 tests.
  - AuditProfilerTest.DisabledByDefaultProducesZeroOverheadNoData
  - AuditProfilerTest.RecordsFaultsAndReprotectionsAccurately
  - AuditProfilerTest.CategorizesGpuWaitsByOrigin
  - AuditProfilerTest.CategorizesSubmitReasons
  - AuditProfilerTest.ComputesExactFrametimePercentilesP50P95P99
  - AuditProfilerTest.ConcurrentRecordingIsThreadSafeAndLockFree
  - AuditProfilerTest.ExportsJsonAndCsvCorrectly
  - AuditProfilerTest.ColdReprotectsDoNotSaturateTableAndHotPagesAreRetained
  - AuditProfilerTest.SimultaneousCadenceAndPartialBufferRefreshConfigCoexistence
  - AuditProfilerTest.ControlledCaptureSessionLifecycleAndReset
  - AuditProfilerTest.HotkeyToggleCaptureStartsAndStops
  - AuditProfilerTest.ConfigurableCadenceIntervalReflectedInSnapshot
  - AuditProfilerTest.RecordsStutteringHitchesAndSlowestFrames
  - AuditProfilerTest.ExtendedStreamBufferConfigReflectedInSnapshot
  - AuditProfilerTest.ManualTriggerModeWaitsForHotkeyAndDoesNotRecordBeforeF9
  - AuditProfilerTest.ExecutionPhaseTrackingAndSeparation
```

Todas las suites de prueba adicionales (`memory_tracker_tests.exe`, `page_manager_tests.exe`, `resource_tracking_tests.exe`, `shader_cfg_tests.exe`) pasaron al 100% sin regresiones.

---

## 7. Próximo Paso: Ejecución del Nuevo Benchmark Riguroso

Con el emulador compilado, el profiler preparado para disparo manual por F9, y los 8 scripts generados (`stage4_2_*.cmd`), el siguiente paso consiste en ejecutar la nueva ronda de pruebas en Astro Bot siguiendo estrictamente el protocolo de calentamiento y escena estática. Los resultados resultantes permitirán evaluar de forma definitiva el beneficio real de GPU Submit Cadence y Partial Refresh.
