# KytyPS5 — Etapa 4.3: Registro Técnico de Integración de Pull Requests Upstream

## 1. Resumen de Integración

En la Etapa 4.3 se auditaron e integraron de forma secuencial y controlada los cinco Pull Requests upstream seleccionados para estabilidad, gestión de memoria y robustez gráfica:

| PR # | Rama / Commit | Descripción | Estado de Integración | Verificación / Test |
|---|---|---|---|---|
| **#2357** | `deef97a01` → `ec10f1462` | Fallback para mip views más allá de `MAX_MIP` | **Integrado** | `shader_cfg_tests.exe` (PASSED) |
| **#2356** | `335963704` → `719686e0f` | Reintento de lecturas de archivos guest con bounce buffer ante páginas protegidas de GPU | **Integrado** | `win_read_efault_tests.exe`, `virtual_memory_allocation_tests.exe` (100% PASSED) |
| **#2358** | `2f296ad4e` | Lectura de páginas BDA fuera de tabla como páginas faltantes (`missing`) | **Pre-existente** | `shader_cfg_tests.exe` (PASSED) |
| **#2188** | `016458937` + `ad7a6f77e` → `26da2768c`, `498a3be0e` | Stacks dedicados de 64 KB para contextos de fibra pequeños (Pumpkin Jack / Astro Bot) | **Integrado** | `virtual_memory_allocation_tests.exe --fiber-only` (100% PASSED) |
| **#2012** | `5b6ce5c60..cd8390814` | Sincronización de imágenes de storage con write faults de CPU (Riesgo Alto) | **Auditado / Reemplazado** | Sustituido por `9678c59b7` + `aa5d2cb94` upstream |

---

## 2. Detalle Técnico por Pull Request

### 2.1 PR #2357 — Fallback para mip views past `MAX_MIP` (`ec10f1462`)
- **Problema**: Shaders que consultaban vistas de texturas con niveles de mip superiores a los soportados nativamente por el hardware o Vulkan fallaban al crear la vista de imagen.
- **Solución**: Se implementa un fallback a la cadena almacenada en la memoria del descriptor cuando el índice de mip excede el límite máximo.
- **Resolución de conflictos**: Se armonizó `src/graphics/host_gpu/renderer/pipeline/descriptors.cpp` y `tests/shaderCfgTests.cpp` con las comprobaciones BDA y las optimizaciones de registro vigentes.

### 2.2 PR #2356 — Reintento de lecturas de archivos guest (`719686e0f`)
- **Problema**: Cuando el motor del juego solicita lectura de archivos del sistema de archivos del guest (`sysFileIO`) hacia una región de memoria que está actualmente protegida contra escritura por el Memory Tracker de la GPU (para detectar modificaciones de CPU), la llamada a la API del sistema operativo host (Windows `ReadFile` / Linux `pread`) fallaba de inmediato con error `EFAULT` (0x8002000e) o violación de acceso, ya que el driver del kernel de Windows no ejecuta el manejador de excepciones de usuario durante I/O en buffer de usuario.
- **Solución**: Se introduce un mecanismo de rebote (*bounce buffer*): si `ReadFile` falla por protección contra escritura en una página monitoreada por la GPU, se lee a un buffer intermedio en heap y se copia byte a byte en memoria guest disparando el manejador estructurado de excepciones de Kyty (`HandleFault`), el cual desprotege limpiamente la página y reintenta la transferencia.
- **Resolución de conflictos**: Se integró en 8 archivos (`fileSystem.cpp`, `sysFileIO.h`, `sysWindowsFileIO.cpp`, `sysLinuxFileIO.cpp`, `memory.h`, `memory.cpp`, `CMakeLists.txt`, `VirtualMemoryAllocationTests.cpp`). Se corrigió el coalescing de GPU unmapping en `ReleaseDirectMemoryInternal` y la re-reserva del espacio de direcciones en `TestGuestReservationRace`.

### 2.3 PR #2358 — BDA pages past table read as missing
- **Auditoría**: Se verificó el árbol de git en upstream `origin/main`. El commit `2f296ad4e` ya se encontraba presente en el historial de nuestra rama base (`415b70c06`). Las comprobaciones de límites de tabla BDA en `spirvEmitterMemory.cpp` y sus tests asociados en `tests/shaderCfgTests.cpp` están activos y verificados.

### 2.4 PR #2188 — Stacks de 64 KB para fibras con contextos reducidos (`26da2768c`, `498a3be0e`)
- **Problema**: Juegos como *Pumpkin Jack* y títulos que utilizan el scheduler de fibras de Prospero configuran estructuras de contexto de fibra minúsculas ($\le 64\text{ KB}$ o incluso 512 bytes). Cuando el hilo nativo de Windows o Linux ejecuta llamadas anidadas o manejadores de señales dentro de la fibra, se producía un desbordamiento catastrófico de pila (*stack overflow*).
- **Solución**: En `libKernel.cpp` (`FiberInitialize`), si el tamaño de stack proporcionado es inferior a 64 KB, se reserva automáticamente una pila dedicada de 64 KB de memoria privada guest. Al retornar o finalizar la fibra (`FiberRun`, `FiberExitThread`), se restauran con precisión los límites de pila del hilo nativo (`TEB/TIB StackBase` y `StackLimit` en Windows vía `%gs:0x08` y `%gs:0x10`).
- **Resolución de conflictos**: Se resolvió la duplicación de `main()` en `tests/VirtualMemoryAllocationTests.cpp` y se integraron todos los tests de fibra (`TestFiberExceptionStack`, `TestFiberHostStack`, `TestFiberStackExtents`, `TestFiberOwnStackFp`, `TestFiberSwitchReturnStack`, `TestSmallFiberStacksAndMigration`).

---

## 3. Auditoría Exhaustiva de PR #2012 (Storage Image Sync)

El Pull Request #2012 (`cd8390814` y ancestros `5b6ce5c60..2844a62a8`) fue catalogado como de **Alto Riesgo**. La auditoría técnica minuciosa concluyó que **no debe ser integrado** por las siguientes razones de arquitectura:

1. **Obsolescencia Técnica (Superado por commit `9678c59b7`)**:
   - PR #2012 intentaba resolver la sincronización de imágenes de storage (usadas por Astro Bot para reproyección en buffers de 384x216) interceptando `InvalidateMemory` en la CPU y forzando una llamada síncrona al hilo de la GPU:
     ```cpp
     RunGpuCommandSync([this, vaddr, size] {
         m_texture_cache.PublishStorageImages();
         m_buffer_cache.InvalidateMemory(vaddr, size);
         m_texture_cache.InvalidateMemory(vaddr, size);
     });
     ```
   - Este enfoque fue posteriormente descartado y superado upstream en el commit `9678c59b7` (*"graphics: read back images at GL2 writebacks behind the storage readback flag"*) y `aa5d2cb94`.
   - La arquitectura actual en `origin/main` (ya presente en nuestra rama) implementa la sincronización en los puntos nativos de writeback de caché GL2 de Vulkan (`WriteBackImages`) y en el manejador de fallos de página mediante `WaitWriteBacks(fault_vaddr, fault_size)`:
     ```cpp
     bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
         if (!IsMapped(fault_vaddr, fault_size)) return false;
         if (WaitWriteBacks(fault_vaddr, fault_size)) return true;
         // InvalidateMemory estándar sin bloqueos síncronos
         ...
     ```
2. **Incompatibilidad Crítica con Submit Cadence y Partial Refresh**:
   - Forzar `RunGpuCommandSync` desde el manejador de write faults mientras el Command Scheduler ejecuta envíos por cadencia periódica cada 1.0 ms introduce una contención severa de cerrojos, serialización total CPU/GPU y riesgo inminente de *deadlock* si la GPU está en una operación diferida.
3. **Decisión de Arquitectura**:
   - Se preserva el diseño moderno de `9678c59b7` activo en nuestra rama.
   - PR #2012 queda formalmente rechazado por obsolescencia y degradación de rendimiento.

---

## 4. Matriz de Verificación de Suites de Pruebas

Tras la integración y resolución de los 5 PRs, se compilaron y ejecutaron con éxito todas las suites de tests:

| Binario de Test | Cobertura | Resultado |
|---|---|---|
| `audit_profiler_tests.exe` | 17 casos de AuditProfiler, ciclo de vida, WARMUP, F9, JSON/CSV | **17/17 PASSED** (100%) |
| `win_read_efault_tests.exe` | Reintento de lectura de archivos host sobre páginas de GPU protegidas | **PASSED** (100%) |
| `shader_cfg_tests.exe` | Fallback de mips, bounds de tablas BDA, parches de recompiler | **PASSED** (100%) |
| `virtual_memory_allocation_tests.exe` | Fibras (`--fiber-only`), memoria flexible, reservas directas, AMM, ASAN | **ALL CASES PASSED** (100%) |
| `shader_recompiler_compute_tests.exe` | Submit Cadence, Texture Age, BDA Sync Uploads, Fault Buffers | **PASSED** (100%) |
