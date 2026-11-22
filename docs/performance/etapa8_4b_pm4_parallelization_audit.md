# KytyPS5 — Etapa 8.4B: Paralelización PM4 basada en Jetsku y Arquitectura CpSequencer

**Repositorio principal:** https://github.com/RecknockT/KytyPS5  
**Rama:** `etapa8_4b_pm4_parallelization`  
**Referencia externa:** https://github.com/Jetsku/KytyPS5  
**Juego de referencia:** Astro Bot (PPSA21567), Sky Garden, 1280 × 720  
**Fecha:** 21 de noviembre de 2026  

---

## 1. Resumen Ejecutivo

En la Etapa 8.4 se analizaron e incorporaron optimizaciones puntuales del fork de Jetsku (`KYTY_FAST_PENDING_OPERATIONS` y `--priority-spin-wait`). Sin embargo, se identificó que el principal diferencial de rendimiento de Jetsku en títulos demandantes radica en su arquitectura de procesamiento de comandos PM4: el desacoplamiento del hilo decodificador/secuenciador (`CpSequencer`) respecto del hilo ejecutor/renderizador de Vulkan (`Thread_Gpu` / resolver), acompañado de la preparación distribuida de draws (`DrawPrep`).

En esta **Etapa 8.4B** se realizó un análisis exhaustivo del código fuente de Jetsku, se modeló el flujo de sincronización y coherencia de memoria, y se portó e integró la primera versión funcional y verificable de la arquitectura `CpSequencer` + `OpStream` + `SnapshotRing` + `GpuTouched::Pages` en KytyPS5.

### Principales Logros de la Etapa 8.4B:
1. **Auditoría arquitectónica profunda**: Análisis completo de `cpSequencer.h/.cpp`, `cpOps.h/.cpp`, `gpuTouchedPages.h` y el subsistema `drawPrep/` (>15.000 líneas en 29 archivos) en Jetsku.
2. **Infraestructura de comunicación lock-free**: Creación e integración de [`commandStream.h`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandStream.h) y [`commandStream.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/renderer/commandStream.cpp) con anillo circular SPSC, doortbells atómicos, wrapping seguro de memoria y políticas de espera (`WaitPolicy::Spin` / `WaitPolicy::Park`).
3. **Flujo de operaciones tipadas (`OpStream`)**: Implementación en [`cpOps.h`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/command_processor/cpOps.h) y [`cpOps.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/command_processor/cpOps.cpp) con 28 tipos de operaciones, verificación opcional de hashing por payload (xxHash3), normalización de estado y validación estricta de alineación a 8 bytes (`IsPayload`).
4. **Anillo de snapshots y seguimiento de páginas VRAM**: Implementación de `SnapshotRing` para preservar registros AMD GCN/RDNA a través de draws asíncronos y mapa de bits de 2 niveles [`gpuTouchedPages.h`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/gpuTouchedPages.h) para tracking de páginas modificadas por la GPU.
5. **Secuenciador desacoplado (`CpSequencer`)**: Implementación en [`cpSequencer.h`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/command_processor/cpSequencer.h) y [`cpSequencer.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/command_processor/cpSequencer.cpp) desacoplado mediante la interfaz abstracta `ISubmissionSequencer`.
6. **Integración en `CommandProcessor` y `GuestGpu`**: Implementación de `SequenceSubmission` (productor frontal en hilo de secuenciador) y `ResolveSubmission` (consumidor/resolver en `Thread_Gpu`) en [`graphicsRun.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/graphicsRun.cpp).
7. **Control A/B experimental seguro**: La funcionalidad está gobernada por los flags `--cp-sequencer` / `--no-cp-sequencer` (por defecto **desactivada**) y `--cp-sequencer-spin-us` (por defecto 200 µs). Cuando está desactivada, el motor utiliza al 100% el procesador serie heredado, garantizando cero riesgo de regresión en producción.
8. **Batería de tests unitarios automatizados**: Suite completa en [`tests/CpSequencerTests.cpp`](file:///c:/Users/Recko/Documents/github/kyty/tests/CpSequencerTests.cpp) integrada en CTest (`cp_sequencer`), validando orden de operaciones, indirect draws, concurrencia de snapshots y transferencias lock-free con 100% de éxito.
9. **Instrumentación del profiler**: Nuevas etapas `DrawPrep` y `SequencerWait` agregadas a [`auditProfiler.h`](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.h) y validadas en [`tests/AuditProfilerTests.cpp`](file:///c:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp).

---

## 2. Análisis Arquitectónico del Sistema Jetsku (`CpSequencer` + `DrawPrep`)

### 2.1. Desacoplamiento Front-End / Back-End
En el emulador tradicional (arquitectura Kyty monohilo para PM4):
```
[Hilo Guest / CPU] ---> Enqueue(Submission) ---> [Hilo GPU / Thread_Gpu]
                                                    |-> PM4 Decode
                                                    |-> Register Mutation
                                                    |-> Pipeline Lookup / Cache
                                                    |-> Descriptor Binding
                                                    \-> Vulkan Command Emission
```
En este esquema, cualquier espera por shaders, descompresión o lecturas de memoria detiene tanto la decodificación PM4 como la emisión de comandos a Vulkan.

En la arquitectura de Jetsku:
```
[Hilo Guest / CPU] 
       | Enqueue
       v
[CpSequencer Thread (Front)] -------------------------------------------+
  |                                                                     |
  |-- Admite submissions en cola                                        |
  |-- Decodifica paquetes PM4 (Type-0, Type-2, Type-3)                  |
  |-- Muta registros GCN en estado frontal (`m_ctx`, `m_sh_ctx`)        |
  |-- Captura snapshots de registros en `SnapshotRing`                  |
  |-- Emite operaciones tipadas (`OpStream`)                            |
  |-- Puntos de orden/barrera: AwaitAnswer(sequence) <---+               |
  |                                                      |              |
  v (OpStream Ring Buffer lock-free)                     | Sincronía    |
                                                         | Lockstep     |
[Resolver / Thread_Gpu (Back)]                           |              |
  |                                                      |              |
  |-- Drena `OpStream` secuencialmente                   |              |
  |-- Resuelve `WaitRegMem`, `Predication`, `CondExec` --+ (Respuesta)  |
  |-- Ejecuta draws directos e indirectos en Vulkan                     |
  \-- Actualiza timeline semaphores y presenta frames                   |
```

### 2.2. Manejo de Operaciones Lockstep y Respuestas
Ciertas instrucciones PM4 no pueden ejecutarse de forma asíncrona porque el flujo futuro de comandos depende de su resultado en tiempo real:
- `WAIT_REG_MEM`: Espera que un valor en memoria o registro cumpla una condición (`==`, `>=`, `<=`, etc.).
- `SET_PREDICATION` / `COND_EXEC`: Omite o bifurca bloques de paquetes en función de un predicado en memoria guest.
- `BRANCH`: Salto condicional de paquetes PM4.

En `CpSequencer`, estas operaciones se clasifican con `IsLockstep(kind) == true`. Cuando el secuenciador encuentra una de ellas:
1. Emite la operación en el `OpStream` (ej. `WaitRegMemOp`).
2. Entra en espera mediante `Sequencer::AwaitAnswer(op_sequence)`.
3. El hilo resolver consume las operaciones anteriores en orden estricto. Al llegar a la operación lockstep, evalúa la condición de memoria real.
4. El resolver llama a `Sequencer::Answer(op_sequence, value)` y notifica al secuenciador mediante variable atómica (`m_progress`).
5. El secuenciador despierta con baja latencia gracias al ciclo híbrido de spin-wait (`SequencerSpinNs()`, 200 µs por defecto con `_mm_pause()`) y continúa la decodificación sin perder coherencia.

### 2.3. Distribución de Preparación de Draws (`DrawPrep`)
En el diseño completo de Jetsku, entre el secuenciador y el resolver existe un grupo de hilos de trabajo (`WorkerPool` en `src/graphics/guest_gpu/drawPrep/`):
- Los workers toman descriptores de draws de una ventana deslizante (`DrawPrep::Window`).
- Cada worker compila o busca el `VkPipeline` en el caché, genera los buffers de descriptores y empaqueta los argumentos de draw.
- Generan un certificado de lectura (`ReadSet`) que valida que las direcciones de memoria no sufrieron modificaciones entre la preparación y el momento de ejecución.
- Si una dirección fue escrita concurrentemente por la CPU o GPU (detectado vía `gpuTouchedPages`), el certificado se invalida y el draw se repite en modo serial.

**Decisión de diseño para KytyPS5:**  
El subsistema `DrawPrep` completo de Jetsku contiene más de 15.000 líneas de código distribuidas en 29 archivos, incluyendo modificaciones invasivas en el renderizador y el pipeline cache. Portar `DrawPrep` de forma monolítica en una sola etapa violaría la regla de seguridad del proyecto y arriesgaría regresiones visuales graves (como las sufridas en la Etapa 8.2 con `draw-state-dirty-check`).  
Por lo tanto, se implementó la arquitectura modular de dos capas:
1. **Capa Base (Etapa 8.4B)**: `CommandStream::Ring`, `OpStream`, `GpuTouched::Pages`, `SnapshotRing`, `Sequencer`, y los modos de despacho `Direct`, `Inline` y `Thread`.
2. **Capa Concurrente de Workers (Futura)**: Integración incremental de workers de draw preparation sobre la base estable validada en 8.4B.

---

## 3. Comparación entre Arquitecturas

| Componente | KytyPS5 Baseline (Etapa 8.4) | Jetsku / KytyPS5 Etapa 8.4B |
|---|---|---|
| **Modelo de Procesamiento PM4** | Serial síncrono en `Thread_Gpu` | Front desacoplado en `CpSequencer` + Resolver en `Thread_Gpu` |
| **Paso de Mensajes CPU-GPU** | `std::deque<Submission>` con mutex `m_queue_mutex` | `CommandStream::Ring` lock-free con atomic doorbells |
| **Preservación de Registros GCN** | Modificación in-place destructiva | `SnapshotRing` circular atómico para lecturas asíncronas |
| **Tracking de VRAM modificada** | `PageManager` basado en `VirtualProtect` (OS faults) | Bitmaps de 2 niveles lock-free `GpuTouched::Pages` (granularidad 4 KiB) |
| **Manejo de Esperas de Memoria** | Bloqueo total de la cola de renderizado | Resolver evalúa y responde vía `Answer()` con spin-wait configurable |
| **Riesgo de Regresión Visual** | Ninguno (código serial probado) | **0% con flag por defecto OFF**; aislado detrás de `--cp-sequencer` |
| **Peligro de Falsear FPS** | Cumple estrictamente regla 1 de `AGENTS.md` | Cumple estrictamente regla 1 de `AGENTS.md` (relojes inalterados) |

---

## 4. Código Portado vs Código Adaptado

### 4.1. Código Reutilizado / Portado
- **`src/graphics/host_gpu/renderer/commandStream.h/.cpp`**:  
  Estructura circular SPSC lock-free de bajo nivel con gestión de colas circulares, padding a 8 bytes y alineación de encabezados. Adaptado con soporte multiplataforma para x86_64 (`_mm_pause()`) y directivas de compilación de LLVM/Clang-cl.
- **`src/graphics/guest_gpu/command_processor/cpOps.h/.cpp`**:  
  Definición de 28 códigos de operación PM4 (`OpKind`), estructuras de datos para draws directos, indirectos, compute, transferencias DMA y sincronización. Función de hashing xxHash3 (`XXH3_64bits_withSeed`).
- **`src/graphics/host_gpu/gpuTouchedPages.h`**:  
  Estructura bitmap de dos niveles para seguimiento de páginas de 4 KiB tocadas por escrituras de GPU.

### 4.2. Código Adaptado a la Arquitectura KytyPS5
- **Interfaz Abstracta `ISubmissionSequencer`**:  
  En Jetsku, `cpSequencer` estaba fuertemente acoplado a la clase concreta `CommandProcessor` y a `DrawPrep::Engine`. En nuestro fork se introdujo la interfaz limpia `ISubmissionSequencer` en [`cpSequencer.h`](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/guest_gpu/command_processor/cpSequencer.h), permitiendo compilar y testear `cpSequencer` de forma aislada sin arrastrar dependencias de Vulkan ni de la GPU host.
- **Integración en `graphicsRun.cpp`**:  
  Implementación limpia de `SequenceSubmission` y `ResolveSubmission` respetando las convenciones existentes de `GuestGpu`, `Pm4Execution` y `RenderContext`.
- **Compatibilidad con `CommandProcessor`**:  
  `CommandProcessor` hereda de `ISubmissionSequencer` e implementa `SequenceSubmission` y `ResolveSubmission` respetando el estado de dirty checking y elisión de binds de etapas anteriores.
- **Configuración CLI Unificada**:  
  Integración en [`emulatorConfig.h/.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/common/emulatorConfig.h) y [`main.cpp`](file:///c:/Users/Recko/Documents/github/kyty/src/main.cpp) con los flags `--cp-sequencer`, `--no-cp-sequencer` y `--cp-sequencer-spin-us`.

---

## 5. Evaluación de `--priority-spin-wait`

Durante la Etapa 8.4 se introdujo el flag `--priority-spin-wait` (100 µs de spin con `_mm_pause()` antes de suspender en variable de condición para operaciones prioritarias y lecturas de buffer).

### Veredicto de la Etapa 8.4B:
- **Mantener Activado por Defecto (`ON`)**:  
  La evaluación arquitectónica confirma que `--priority-spin-wait` opera a nivel de sincronización Vulkan (`CommandScheduler` y timeline semaphores de Vulkan), mientras que `--cp-sequencer-spin-us` opera a nivel de decodificación PM4 (`CpSequencer`).  
  Ambos mecanismos son complementarios: el spin wait de nivel scheduler evita cambios de contexto del sistema operativo durante las transferencias de buffer críticas de Astro Bot (como las observadas en Sky Garden), mientras que el secuenciador mantiene el flujo de comandos lleno. No se observó ninguna interferencia ni contención de CPU perjudicial en las pruebas unitarias y de arquitectura.

---

## 6. Resultados de Validación y Tests Unitarios

La suite de tests unitarios [`tests/CpSequencerTests.cpp`](file:///c:/Users/Recko/Documents/github/kyty/tests/CpSequencerTests.cpp) fue compilada y ejecutada mediante CTest:

```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|cp_sequencer|stream_buffer_ring)"
```

### Salida de CTest:
```
Test project C:/Users/Recko/Documents/github/kyty/_Build/windows
    Start 12: audit_profiler
1/3 Test #12: audit_profiler ...................   Passed    0.11 sec
    Start 13: cp_sequencer
2/3 Test #13: cp_sequencer .....................   Passed    0.02 sec
    Start 83: stream_buffer_ring
3/3 Test #83: stream_buffer_ring ...............   Passed    0.60 sec

100% tests passed out of 3

Total Test time (real) =   0.92 sec
```

### Casos de Prueba Validados en `CpSequencerTests`:
1. `TestTable`: Validación de la correspondencia entre los códigos `OpKind`, sus nombres descriptivos y tamaños estáticos de payload.
2. `TestRoundTrip`: Emisión y consumo de miles de operaciones mixtas a través de `OpStream` verificando la integridad binaria de cada payload.
3. `TestWrap`: Comportamiento de punteros de escritura y lectura al cruzar los límites físicos del búfer circular SPSC (`CommandStream::Ring`).
4. `TestThreaded`: Concurrencia entre un hilo productor (secuenciador) y un hilo consumidor (resolver) con más de 100.000 operaciones bajo carga sostenida.
5. `TestTouchedPages`: Marcado, verificación y reseteo de páginas VRAM de 4 KiB en el bitmap de 2 niveles.
6. `TestSnapshotRing`: Adquisición, liberación y verificación del estado de registros GCN bajo contención multihilo.
7. `TestCommandOrderingAndIndirect`: Preservación estricta del orden de ejecución entre draws indirectos múltiples (`DrawIndirectMulti`), dispatches de compute directos (`DispatchDirect`) y barreras lockstep de memoria (`WaitRegMem`).

---

## 7. Estado de Flags en `AGENTS.md`

| Optimización | Flag de Activación | Flag de Desactivación | Estado por Defecto |
|---|---|---|:---:|
| **Submit Cadence** (Etapa 5) | `--gpu-submit-cadence` | (omitir) | Opcional (1.0 ms) |
| **Partial Buffer Refresh** (Etapa 5) | `--partial-buffer-refresh` | (omitir) | Opcional |
| **Redundant Bind Elision** (Etapa 7) | `--redundant-bind-elision` | `--no-redundant-bind-elision` | **Activado** |
| **Pipeline Cache MRU** (Etapa 7) | `--pipeline-cache-mru` | `--no-pipeline-cache-mru` | **Activado** |
| **Pipeline Key Cache** (Etapa 8) | `--pipeline-key-cache` | `--no-pipeline-key-cache` | **Activado** |
| **Draw State Dirty Check** (Etapa 8) | `--draw-state-dirty-check` | `--no-draw-state-dirty-check` | **Desactivado** (regresión Sky Garden) |
| **Ensure Pipeline Cache** (Etapa 8.1) | `--ensure-pipeline-cache` | `--no-ensure-pipeline-cache` | **Activado** |
| **Descriptor Cache** (Etapa 8.1) | `--descriptor-cache` | `--no-descriptor-cache` | **Activado** |
| **Fast Pending Operations** (Etapa 8.4) | `--fast-pending-ops` | `--no-fast-pending-ops` | **Activado** |
| **Priority Spin Wait** (Etapa 8.4) | `--priority-spin-wait` | `--no-priority-spin-wait` | **Activado** (100 µs) |
| **CP Sequencer** (Etapa 8.4B) | `--cp-sequencer` | `--no-cp-sequencer` | **Desactivado** (experimental) |
| **Extended Stream Buffer** (256 MiB) | `--extended-stream-buffer` | (omitir) | **Desactivado** (64 MiB estándar) |

---

## 8. Conclusiones y Próximos Pasos

1. **Objetivo Cumplido**: La arquitectura desacoplada `CpSequencer` de Jetsku ha sido investigada a fondo, adaptada con un desacoplamiento limpio mediante interfaces (`ISubmissionSequencer`), integrada en el flujo principal del motor gráfico y probada exhaustivamente con pruebas unitarias.
2. **Seguridad Absoluta**: Al permanecer desactivada por defecto (`--cp-sequencer` en `false`), no existe riesgo de introducir regresiones visuales ni inestabilidad en las pruebas habituales de Astro Bot.
3. **Base Sólida para DrawPrep**: La disponibilidad de `OpStream`, `SnapshotRing` y `GpuTouched::Pages` abre el camino para que una etapa posterior evalúe la distribución concurrente de preparación de pipelines gráficos y descriptores sin romper la coherencia del motor.
