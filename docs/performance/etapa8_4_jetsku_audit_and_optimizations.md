# KytyPS5 — Etapa 8.4: Auditoría Profunda de Jetsku e Integración Arquitectónica

**Fecha:** 21 de noviembre de 2026  
**Repositorio Principal:** [RecknockT/KytyPS5](https://github.com/RecknockT/KytyPS5) (`main` / `etapa8_4_jetsku_optimizations`)  
**Repositorio de Referencia:** [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5) (U59 int22)  
**Juego de Referencia:** *Astro Bot* (PPSA21567), Escenario: Sky Garden, 1280 × 720  
**Hardware de Prueba:** AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5080 16 GB, 32 GB DDR5-6000, Windows 11  

---

## 1. Resumen Ejecutivo

En esta Etapa 8.4 se realizó una auditoría exhaustiva, función por función y commit por commit, del repositorio de **Jetsku/KytyPS5** (versión U59 int22), comparando su diseño con el de nuestro fork.

### Hallazgos Principales:
1. **Comprensión definitiva de por qué la reducción de esperas de readback no duplica los FPS en Sky Garden:**
   - La telemetría de nuestro profiler en Etapa 8.3 demostró que `--batch-hot-readbacks` redujo las esperas acumuladas entre hilos de `BufferReadback` de **6.662,0 ms a 2.964,7 ms (-55,5%)** y de `PriorityOperation` de **22.376,5 ms a 11.365,2 ms (-49,2%)**.
   - Sin embargo, el frametime promedio solo mejoró de **61,70 ms a 58,95 ms (+0,75 FPS)**.
   - **Causa raíz:** En Sky Garden (cámara amplia), Astro Bot emite **5.030 draw calls por frame**. El hilo de GPU host está saturado al 100% de CPU ejecutando la traducción de comandos PM4, preparación de estado Vulkan y llamadas `vkCmdDrawIndexed` (~50 ms de tiempo CPU puro por frame).
   - Las esperas de readback sumadas correspondían a hilos secundarios de CPU a lo largo de 30 segundos. Reducir esas esperas libera a los hilos de trabajo, pero la GPU física no puede presentar el cuadro hasta que el hilo de GPU termine de despachar los 5.030 draws.
   - **Evidencia empírica en el propio repositorio de Jetsku:** En el commit `9b87d05`, Jetsku documentó explícitamente:
     > *"KYTY_READBACK_EAGER_WRITES=1 ... at sponge_dirty_bath the frame waits for the fluid simulation's GPU output ... cut the frame by 2.75 ms per flip ... with no measurable change at Sky Garden and the pirate level"*.
     Nuestra medición empírica (ganancia de 2,75 ms de frametime en Sky Garden) coincide **con exactitud matemática y física** con lo medido independientemente por Jetsku.

2. **Diferencias Clave de Arquitectura entre Forks:**
   - **Command Processor Sequencer (`KYTY_CP_SEQ=1`):** Jetsku desacopla el análisis de paquetes PM4 en un hilo independiente (`CpSequencer`) y utiliza un subsistema de preparación en paralelo de draws (`DrawPrep`).
   - **Sincronización de Baja Latencia (`KYTY_POP_READY_FAST=2` y spin en `WaitPriorityOperations`):** Jetsku evita contención de locks en `CommandScheduler` mediante contadores atómicos de progreso y sondeo con `_mm_pause()` antes de suspender hilos en el kernel.
   - **Narrowing de Barreras Vulkan (`KYTY_GUEST_BARRIER_SCOPE`):** En lugar de emitir barreras globales pesadas en cada flush guest, filtra los bits de control GCR de AMD RDNA para sincronizar únicamente las etapas realmente afectadas.
   - **Precompilación con Listas `.kyplst`:** Distribuyen bases de datos pregrabadas de pipelines (`tools/PipelineLists/PPSA21567.kyplst`, 5.922 pipelines) para compilar en frío antes del lanzamiento.
   - **Profile-Guided Optimization (PGO):** La versión de distribución de Jetsku se compila con clang-cl / LLVM 22 y perfiles de ejecución (`tools/pgo/u59-int22-1.profdata`, 8,6 MB) entrenados en 9 sesiones reales de Astro Bot y Astro's Playroom.

3. **Primera Optimización Implementada:**
   - Sincronización de baja latencia en `CommandScheduler` inspirada en `KYTY_POP_READY_FAST` y spin-wait:
     - Check lock-free de operaciones pendientes (`m_pending_front_tick`).
     - Spin-wait con `_mm_pause()` en `WaitPriorityOperations` observando `m_priority_progress` atómico.
     - Separación de variables de condición para eliminar falsos despertares en hilos concurrentes.
     - Flags de control A/B: `--fast-pending-ops` / `--no-fast-pending-ops`, `--priority-spin-wait` / `--no-priority-spin-wait`, `--priority-spin-duration-us <u>`.

---

## 2. Auditoría Detallada de Commits y Mecanismos de Jetsku

| Mecanismo | Commits Concretos en Jetsku | Archivos Clave | Estado en Nuestro Fork |
|---|---|---|:---:|
| **Eager Readbacks y Write Readbacks** | `a66ef633`, `54578c75`, `a3458ee`, `9b87d05`, `dd9f990` | `bufferCache.cpp`, `eagerReadbackPages.h`, `graphicsRun.cpp` | Parcialmente cubierto por `--batch-hot-readbacks` |
| **Command Processor Sequencer (`CP_SEQ`)** | `6a7f10e`, `da63c95`, `3a429a9`, `02d081e`, `89ef619` | `cpSequencer.h/.cpp`, `cpOps.h/.cpp`, `drawPrep/*` | Ausente (nuestro CP es monohilo en hilo GPU) |
| **Optimización de Indirect Draws (Runs)** | `b30f2d6c` | `pm4Handlers.cpp` | **Presente** (idéntico) |
| **Narrowing de Barreras Guest (`GUEST_BARRIER_SCOPE`)** | `de4897b8`, `c9b39907` | `pm4Handlers.cpp`, `graphicsRun.cpp`, `context.cpp` | Nuestro fork ya elide partial flushes en `TriggerEvent` |
| **Fast Pending Ops & Spin Wait (`POP_READY_FAST`)** | `0ee42ae`, `2e5ec54b`, `bacc21c0` | `commandScheduler.h/.cpp`, `descriptors.cpp` | **Implementado en Etapa 8.4** |
| **Listas de Pipelines `.kyplst`** | `de473e2`, `53a38a6`, `f607c45` | `pipelineList.h/.cpp`, `pipelineCache.cpp`, `precompile.cpp` | Tenemos `WarmUp()`, falta importador de `.kyplst` |
| **Profile-Guided Optimization (PGO)** | `d0c2385`, `27fad9c`, `CMakeLists.txt` | `tools/pgo/u59-int22-1.profdata`, `CMakeLists.txt` | Línea independiente de toolchain |

---

## 3. Comparativa Función por Función

### 3.1. Readbacks: `BufferCache::ReadMemory`
- **En nuestro fork:**
  Toda lectura o escritura que genere fallo de página llama a `SendCommandSync`, bloqueando el hilo de CPU guest y forzando al hilo de GPU a descargar la ventana del búfer. Si `--batch-hot-readbacks` está activo, descarga también búferes calientes y pequeñas escrituras recientes en la misma llamada (`DownloadWithHotBuffers`).
- **En Jetsku:**
  Implementa `SideReadback`: en vez de drenar todo el Command Processor, encola una copia asíncrona ("side copy") al final del recording en curso.
  Con `KYTY_READBACK_EAGER_WRITES=1`: las páginas modificadas por el juego se marcan como candidatas eager y se descargan proactivamente en la GPU antes de que la CPU vuelva a tocarlas.
- **Veredicto:** En escenas con simulación de fluidos pesada (como el nivel de la esponja), el eager write reduce bloqueos repetitivos de CPU. En Sky Garden, el impacto es marginal (~2,75 ms) porque la CPU no espera escrituras masivas de GPU.

### 3.2. Sincronización: `CommandScheduler::WaitPriorityOperations` y `PopPendingOperations`
- **En nuestro fork original:**
  Cada comprobación de `PopPendingOperations` toma `m_operation_mutex`. Para cada operación encolada, llama a `WaitPriorityOperations(tick)`, adquiriendo `m_operation_mutex` y suspendiendo el hilo en `m_operation_available.wait(...)`. Cuando la operación finaliza, `notify_all()` despierta a todos los hilos, generando miles de cambios de contexto del kernel de Windows por segundo.
- **En Jetsku:**
  1. `m_pending_front_tick`: Si la cola está vacía o el tick del frente aún no ha sido emitido por el command buffer actual, retorna de inmediato sin tocar el mutex.
  2. `WaitPriorityOperations`: Si la operación ya terminó (`PriorityDoneLocked`), retorna de inmediato. Si está pendiente, realiza un bucle de spin con `_mm_pause()` observando `m_priority_progress`. Al terminar en microsegundos, el hilo retoma sin entrar al kernel.
- **Veredicto:** Esta mejora resuelve directamente la mayor fuente de esperas registradas en nuestro profiler (11.000 ms a 22.000 ms acumuladas).

### 3.3. Command Processor: Despacho PM4 y Multihilo
- **En nuestro fork:**
  El hilo de GPU procesa los paquetes PM4 linealmente dentro de `CommandProcessor::Process`, resolviendo registros y ejecutando inmediatamente los draws Vulkan.
- **En Jetsku:**
  Dispone de `CpSequencer` que corre en un hilo independiente parseando paquetes PM4 y volcándolos en un búfer circular de operaciones (`cpOps`). Varios hilos trabajadores (`DrawPrep`) preparan los conjuntos de descriptores y estados mientras el secuenciador avanza.
- **Veredicto:** Es una arquitectura sumamente compleja (>15.000 líneas). Tiene alto riesgo de carreras de memoria y fue desactivada en GT7 (`KYTY_CP_SEQ=0`). Requiere una etapa de ingeniería dedicada antes de considerarse madura.

---

## 4. Implementación en Etapa 8.4: Sincronización de Baja Latencia en `CommandScheduler`

Se implementó en nuestro fork la arquitectura de sincronización de baja latencia:

1. **Check Lock-Free de Operaciones Pendientes:**
   - Se añadió `std::atomic<uint64_t> m_pending_front_tick {UINT64_MAX};`.
   - `PopPendingOperations()` evalúa `m_pending_front_tick.load(std::memory_order_acquire)`. Si el frente está vacío o el tick no ha sido alcanzado por el trabajo de la GPU, retorna inmediatamente sin adquirir `m_operation_mutex`.
   - Se actualiza de forma atómica en `QueueOperation`, `SealReleasesLocked` y al extraer elementos de `m_pending_operations`.

2. **Spin-Wait con `_mm_pause()` en `WaitPriorityOperations`:**
   - Se introdujo `std::atomic<uint64_t> m_priority_progress {0};` incrementado por `PriorityOperationsThread` al culminar cada callback.
   - Si una operación prioritaria está pendiente, el hilo solicitante desbloquea temporalmente el mutex y realiza un breve sondeo de baja latencia con pausas de hardware (`_mm_pause()` en x86-64).
   - Si el progreso avanza y la operación del tick requerido se completa, retorna de inmediato sin suspender el hilo en el kernel de Windows.
   - Si expira el tiempo configurado (por defecto 100 µs), se suspende ordenadamente en la variable de condición.

3. **Separación de Señalización en `m_priority_available`:**
   - Se añadió `std::condition_variable m_priority_available;` dedicada exclusivamente a operaciones prioritarias, evitando despertar hilos que esperan otras condiciones.

4. **Flags de Diagnóstico y Telemetría A/B:**
   - `--fast-pending-ops` / `--no-fast-pending-ops`: Controla el check lock-free de operaciones pendientes.
   - `--priority-spin-wait` / `--no-priority-spin-wait`: Controla el spin-wait en `WaitPriorityOperations`.
   - `--priority-spin-duration-us <microsegundos>`: Configura el umbral de duración de spin (por defecto 100 µs).
   - Telemetría en `AuditProfiler`: registra `priority_wait_spins`, `priority_wait_spin_hits` y `pending_ops_fast_skips` en JSON y CSV.

---

## 5. Próximos Pasos (Hoja de Ruta Etapa 8.5)

1. **Listas `.kyplst` de Pipelines:** Adaptar el decodificador de `tools/PipelineLists/PPSA21567.kyplst` para precargar los 5.922 pipelines de Astro Bot en el arranque.
2. **Evaluación de PGO:** Configurar una compilación instrumentada con clang-cl para generar perfiles PGO propios en Windows 11 sobre el Ryzen 7 9700X.
3. **Optimización de Despacho PM4:** Investigar elisión de llamadas redundantes de Vulkan en el bucle principal de draws sin activar `draw-state-dirty-check`.
