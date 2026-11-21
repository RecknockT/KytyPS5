# KytyPS5 — Etapa 5: Análisis Profundo de Hot RIPs, Páginas Calientes y Mecanismo de Reprotección

**Fecha:** 21 de Noviembre de 2026  
**Rama:** `etapa5_optimization`  
**Juego de referencia:** Astro Bot (`PPSA21567`)  
**Hardware:** AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5080, 32 GB DDR5-6000  

---

## 1. Análisis de Hot RIPs (Puntos Calientes de Ejecución Huésped)

Durante los 180 segundos de la captura de referencia se registraron **27.198.992 write faults**. El profiler identificó con precisión las direcciones de instrucción huésped (`RIP`) que originaron estos fallos.

### 1.1 Tabla de los 15 RIPs más calientes

| Posición | RIP Huésped | Módulo Estimado | Total de Fallos | % del Total | % Acumulado |
|:---:|:---:|---|---:|---:|---:|
| **#1** | `0x0000000920003e55` | `libc.prx` (bloque de copia rápida) | **4.762.160** | **17,51%** | 17,51% |
| **#2** | `0x00000009004b5e48` | `eboot.bin` (motor / render loop) | **1.332.875** | **4,90%** | 22,41% |
| **#3** | `0x0000000907170928` | `eboot.bin` (actualización de entidades) | **1.123.232** | **4,13%** | 26,54% |
| **#4** | `0x00000009071734a0` | `eboot.bin` (transformaciones / esqueletos) | **1.095.705** | **4,03%** | 30,57% |
| **#5** | `0x00000009072d5e90` | `eboot.bin` (buffers de vértices dinámicos) | **944.310** | **3,47%** | 34,04% |
| **#6** | `0x0000000920003db6` | `libc.prx` (bloque de copia rápida) | **830.962** | **3,06%** | 37,09% |
| **#7** | `0x0000000920003fe2` | `libc.prx` (bloque de copia rápida) | **787.377** | **2,89%** | 39,99% |
| **#8** | `0x000000090709d87c` | `eboot.bin` (gestor de partículas / staging) | **672.689** | **2,47%** | 42,46% |
| **#9** | `0x00000009072b4030` | `eboot.bin` (uniform buffers dinámicos) | **600.973** | **2,21%** | 44,67% |
| **#10** | `0x0000000920003ef0` | `libc.prx` (bloque de copia rápida) | **569.834** | **2,10%** | 46,77% |
| **#11** | `0x000000090709e604` | `eboot.bin` (partículas / render context) | **449.808** | **1,65%** | 48,42% |
| **#12** | `0x0000000900253a13` | `eboot.bin` (cola de comandos / submission) | **448.454** | **1,65%** | 50,07% |
| **#13** | `0x00000009072d5eb8` | `eboot.bin` (buffers de vértices dinámicos) | **446.128** | **1,64%** | 51,71% |
| **#14** | `0x000000090709e5a4` | `eboot.bin` (partículas / staging) | **445.699** | **1,64%** | 53,35% |
| **#15** | `0x0000000907175458` | `eboot.bin` (esqueletos / mallas animadas) | **444.697** | **1,63%** | 54,98% |
| — | **Total Top 15** | — | **14.954.903** | **54,98%** | — |

### 1.2 Patrones y Clústeres de Instrucciones

#### A. El Clúster de Rutinas de Memoria (`libc.prx` en `0x0000000920003xxx`)
Los cuatro RIPs en el rango `0x920003xxx` (#1, #6, #7 y #10) residen en el módulo de biblioteca estándar de C cargado a continuación del ejecutable principal. Suman en conjunto:
$$\text{Total Clúster Libc} = 4.762.160 + 830.962 + 787.377 + 569.834 = \mathbf{6.950.333\text{ fallos (25,55\%)}}$$
- **Naturaleza técnica:** Implementaciones vectorizadas de `memcpy`, `memmove` y `memset` que utilizan registros AVX2/SSE (`vmovdqu`, `vmovdqa`, `vmovntdq`).
- **Comportamiento en fallo:** Copian bloques de memoria de tamaño variable hacia memoria directa compartida con GPU. Cada vez que la copia cruza una frontera de página de 4 KB (4096 bytes), la CPU tropieza con una página protegida (`PAGE_READONLY`), detonando una interrupción `#PF`.

#### B. El Clúster del Motor del Juego (`eboot.bin` en `0x0000000907xxxxxx`)
Los RIPs en `0x907xxxxxx` corresponden a funciones internas del motor de Astro Bot:
- **`0x0000000907170928` / `0x00000009071734a0` / `0x0000000907175458`:** Generación de matrices de transformación de esqueletos para animación de personajes (Astro Bot y enemigos).
- **`0x00000009072d5e90` / `0x00000009072d5eb8`:** Ensamblado de vértices en streaming para geometría procedural y deformaciones de mallas.
- **`0x000000090709d87c` / `0x000000090709e5a4` / `0x000000090709e604`:** Simulación de efectos de partículas y emisores visuales en memoria compartida.

---

## 2. Análisis de Hot Pages y Distribución de Memoria

Las páginas más calientes se encuentran en el rango `0x00000005xxxxxxxx`, correspondiente a la memoria directa asignada por `KernelMapDirectMemory` para la interacción entre CPU y GPU.

### 2.1 Tabla de las 15 Páginas más Calientes

| Posición | Dirección de Página | Fallos Registrados | Reprotecciones | Ratio Fallos/Reprot. | Naturaleza del Recurso |
|:---:|:---:|---:|---:|:---:|---|
| **#1** | `0x000000057987f000` | **9.094** | **13.472** | **0,68** | Staging / Uniform Buffer multi-pass |
| **#2** | `0x000000050d547000` | **7.498** | **1.123** | **6,68** | Dynamic Vertex Buffer (Ring A, pág 3) |
| **#3** | `0x000000050d546000` | **7.489** | **1.123** | **6,67** | Dynamic Vertex Buffer (Ring A, pág 2) |
| **#4** | `0x000000050ed47000` | **7.480** | **1.123** | **6,66** | Dynamic Vertex Buffer (Ring B, pág 3) |
| **#5** | `0x000000050ed46000` | **7.472** | **1.123** | **6,65** | Dynamic Vertex Buffer (Ring B, pág 2) |
| **#6** | `0x000000050d548000` | **7.468** | **1.123** | **6,65** | Dynamic Vertex Buffer (Ring A, pág 4) |
| **#7** | `0x000000050ed48000` | **7.458** | **1.123** | **6,64** | Dynamic Vertex Buffer (Ring B, pág 4) |
| **#8** | `0x000000050d545000` | **7.457** | **1.123** | **6,64** | Dynamic Vertex Buffer (Ring A, pág 1) |
| **#9** | `0x000000050ed45000` | **7.445** | **1.123** | **6,63** | Dynamic Vertex Buffer (Ring B, pág 1) |
| **#10** | `0x000000050d566000` | **7.439** | **1.123** | **6,62** | Animation Matrix Buffer (Ring A) |
| **#11** | `0x000000050bd47000` | **7.438** | **1.122** | **6,63** | Dynamic Vertex Buffer (Ring C, pág 3) |
| **#12** | `0x000000050d54e000` | **7.435** | **1.123** | **6,62** | Dynamic Vertex Buffer (Ring A, pág 5) |
| **#13** | `0x000000050d549000` | **7.433** | **1.123** | **6,62** | Dynamic Vertex Buffer (Ring A, pág 6) |
| **#14** | `0x000000050d54c000` | **7.433** | **1.123** | **6,62** | Dynamic Vertex Buffer (Ring A, pág 7) |
| **#15** | `0x000000050d567000` | **7.430** | **1.123** | **6,62** | Animation Matrix Buffer (Ring A) |

### 2.2 Descubrimiento del Esquema de Triple Buffering
Las páginas `0x50bd47000`, `0x50d547000` y `0x50ed47000` presentan una métrica idéntica:
$$\text{Reprotecciones} = 1.123 \approx \frac{3.369\text{ cuadros}}{3}$$
Esto demuestra de manera irrefutable que Astro Bot emplea un esquema de **Triple Buffering cíclico**:
- **Frame $3k$:** Escribe y consume el buffer en la región `0x50bd40000`.
- **Frame $3k+1$:** Escribe y consume el buffer en la región `0x50d540000`.
- **Frame $3k+2$:** Escribe y consume el buffer en la región `0x50ed40000`.

Cada buffer tiene un tamaño contiguo de entre 32 KB y 64 KB (páginas `0x50d545000` a `0x50d54e000`). En cada frame asignado a ese buffer, múltiples hilos escriben sobre sus páginas generando exactamente ~7.480 fallos a lo largo de la sesión (~6,6 fallos por página en cada frame activo).

### 2.3 La Anomalía de la Página `0x000000057987f000`
A diferencia de los buffers de triple buffering (ratio Fallos/Reprot > 6), esta página exhibe:
- **13.472 reprotecciones** en 3.369 frames:
$$\frac{13.472}{3.369} = \mathbf{3,9988 \approx 4\text{ reprotecciones exactas por frame}}.$$
- **9.094 fallos** en 3.369 frames:
$$\frac{9.094}{3.369} = \mathbf{2,70\text{ fallos por frame}}.$$

**Causa técnica:** Esta página contiene un buffer de constantes o descriptor dinámico que la GPU consulta en 4 pases de renderizado diferentes dentro del mismo frame.
1. En cada pase, el scheduler ejecuta `SynchronizeBuffer` o `SynchronizeBda`, y `ForEachUploadRange` marca la página nuevamente como protegida (`PAGE_READONLY`).
2. En 2 o 3 de esos pases, la CPU actualiza valores intermedios (por ejemplo, parámetros de cámara o luces dinámicas), provocando un nuevo fallo y otra desprotección.
3. El resultado es un "efecto ping-pong" de 4 reprotecciones y ~3 fallos **en cada cuadro individual**.

---

## 3. Auditoría del Mecanismo de Reprotección de Memoria

### 3.1 Ciclo de Vida de la Protección
El flujo de reprotección ocurre en dos componentes principales:
1. **`BufferCache::SynchronizeBuffer` y `BufferCache::SynchronizeBda`:**
   En cada preparación de draw o compute dispatch, se invoca [memoryTracker.h](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/memoryTracker.h#L93):
   ```cpp
   manager->ForEachModifiedRange<DirtySource::Cpu, true>(offset, bytes, range_func);
   ```
2. **`RegionManager::ForEachModifiedRange`:**
   En [regionManager.h](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/regionManager.h#L131):
   ```cpp
   if constexpr (clear) {
       Bump();
       bits.UnsetRange(start, end);
       if constexpr (source == DirtySource::Cpu) {
           UpdateProtection<true, false>(mask);
       }
   ...
   ```
   Limpia el bit `CpuDirty` y llama a `UpdateProtection<true, false>`, solicitando a `PageManager` que restaure la vigilancia contra escritura (`write_watchers++`).
3. **`PageManager::Impl::UpdateRegionWatchers`:**
   En [pageManager.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/host_gpu/pageManager.cpp#L200):
   Calcula la máscara de páginas a actualizar y emite la llamada a `Protect(base_addr + range_begin * PAGE_SIZE, range_bytes, perms)` con `perms = PAGE_READONLY`.

### 3.2 Eficiencia del Coalescing Existente
En `PageManager::Impl::UpdateRegionWatchers`:
- Las páginas contiguas dentro de una misma región (2 MB) que cambian al mismo estado se agrupan en un único rango continuo (`range_bytes`) y se envían en una única llamada a `Protect`.
- **Fortaleza:** La reprotección durante la subida a GPU ya está coalescida a nivel de región.
- **Debilidad crítica:**
  1. Durante el **manejo del fallo CPU** (`RenderContext::HandleFault`), la desprotección es estrictamente de 1 página (4 KB). No existe ningún tipo de anticipación o coalescencia en la desprotección.
  2. La reprotección ocurre en cada draw o submit donde se consume el buffer, provocando que una página modificada varias veces por cuadro sufra hasta 4 reprotecciones innecesarias antes del fin del frame.
