# KytyPS5 — Etapa 4.3: Validación Final y Protocolo de Benchmark

## 1. Síntesis General de la Etapa 4.3

La Etapa 4.3 ha cumplido de manera exhaustiva todos los objetivos establecidos:

1. **Corrección de Instrumentación**:
   - Se erradicó la fuga de acumulación de fases pre-captura en `AuditProfiler` (`ResetCaptureSessionState`).
   - Se garantizó la coherencia estricta entre exportaciones JSON y CSV desde un snapshot único e inmutable.
   - Validado con 17/17 tests automáticos en `tests/AuditProfilerTests.cpp`.
2. **Integración Rigurosa de PRs Upstream**:
   - **PR #2357** (Fallback para mips más allá de `MAX_MIP`): Integrado (`ec10f1462`).
   - **PR #2356** (Reintento de lecturas de archivos guest con bounce buffer): Integrado (`719686e0f`).
   - **PR #2358** (BDA pages past table): Verificado pre-existente en `origin/main`.
   - **PR #2188** (Stacks dedicados de 64 KB para fibras pequeñas): Integrado (`26da2768c`, `498a3be0e`).
   - **PR #2012** (Storage image sync con write faults de CPU): Auditado a fondo y formalmente descartado por obsolescencia técnica (superado por `9678c59b7`) e incompatibilidad con Submit Cadence.
3. **Diagnóstico Técnico de Sincronización y "Cámara Lenta"**:
   - Se demostró el mecanismo de cuantización de VBlank (16,66 ms a 60 Hz).
   - Se explicó cómo `WaitFlipDone` y el paso de tiempo fijo (*fixed timestep*) de Astro Bot causan que un tiempo de render de 48–52 ms ralentice la simulación física a un 33% de su velocidad nominal.
4. **Preservación y Configuración Óptima de Rendimiento**:
   - Se mantiene la combinación de **GPU Submit Cadence (1.0 ms)** y **Partial Buffer Refresh**.
   - Se **descartó** el `Extended Stream Buffer` tras evidenciar inestabilidad y contención en la Etapa 4.2.
   - Compilación completa exitosa de `kyty_emulator.exe` y suites de prueba al 100% de éxito.

---

## 2. Preparación del Script de Benchmark

Se verificó el script de ejecución [stage4_3_cadence_partial.cmd](file:///c:/Users/Recko/Documents/github/kyty/stage4_3_cadence_partial.cmd), configurado específicamente para ejecutar la prueba de 180 segundos en condiciones idóneas:

```cmd
"%EMU_EXE%" --game "%GAME_DIR%" ^
    --shader-cache ^
    --audio-compile-hold ^
    --perf-overlay ^
    --gpu-submit-cadence ^
    --gpu-submit-cadence-ms 1.0 ^
    --partial-buffer-refresh ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 180 ^
    --audit-export "stage4_3_cadence_partial.json"
```

---

## 3. Protocolo Riguroso de Benchmark para el Usuario

Para garantizar que los resultados de esta validación final sean comparables, repetibles y libres de sesgos de carga inicial, siga estrictamente estos pasos:

1. **Ejecutar el script**:
   - Haga doble clic o ejecute desde la terminal:
     ```cmd
     stage4_3_cadence_partial.cmd
     ```
2. **Fase de Calentamiento (*WARMUP*)**:
   - El emulador se abrirá mostrando en el overlay:
     `[F9] Audit: WARMUP (Ready in Scene)`
   - El profiler **NO** registrará datos durante la carga del juego, menús ni compilación inicial de shaders.
3. **Navegar a la Escena de Prueba**:
   - Inicie la partida y sitúe a Astro Bot en la **misma escena interactiva de gameplay** utilizada en las capturas de la Etapa 4.2.
   - Deje la cámara fija en la posición de referencia habitual.
4. **Estabilización de Recursos**:
   - Permanezca inmóvil en la escena durante 20 a 30 segundos para permitir que cualquier carga de fondo y caché de shaders se complete.
5. **Iniciar la Captura**:
   - Presione la tecla **F9**.
   - El overlay cambiará a:
     `[F9] Audit: RECORDING (XXXs / 180s)` en color rojo.
   - Mantenga el juego en esa escena durante los 180 segundos.
6. **Finalización Automática y Exportación**:
   - Transcurridos los 180 segundos exactos, el profiler cerrará la sesión automáticamente y el overlay mostrará:
     `[F9] Audit: COMPLETED` en color verde.
   - Se habrán generado en el directorio raíz los archivos:
     - `stage4_3_cadence_partial.json`
     - `stage4_3_cadence_partial.csv`
7. **Cierre**:
   - Cierre el emulador.
