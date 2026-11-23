# KytyPS5 — Etapa 8.4F.1: Diagnóstico y Corrección del Congelamiento de Arranque con CpSequencer

**Fecha:** 22 de Noviembre de 2026  
**Rama de Trabajo:** `main` (commit base `cab89fd315657ba023563352e0500da807f63cbc`)  
**Repositorio Principal:** [RecknockT/KytyPS5](https://github.com/RecknockT/KytyPS5)  
**Título de Referencia:** *Astro Bot* (`PPSA21567`)  
**Estado:** **CORREGIDO Y VALIDADO**  

---

## 1. Descripción del Problema y Alcance

Durante la validación de arranque y juego en *Astro Bot* (`PPSA21567`) tras la integración de la Etapa 8.4F, se observó el siguiente comportamiento diferencial según los perfiles de ejecución:

- **Perfil A (`--no-cp-sequencer --no-draw-prep`):** El emulador arranca con normalidad, superando los logotipos iniciales y avanzando a la secuencia de intro y menús.
- **Perfil B (`--cp-sequencer --no-draw-prep`):** El emulador se congela de manera determinista en el primer logotipo del juego, antes de alcanzar la pantalla del logotipo de PS5.
- **Perfil C (`--cp-sequencer --draw-prep --draw-prep-workers 4`):** El emulador sufre exactamente el mismo congelamiento en el primer logotipo.

Al compartir ambos perfiles fallidos la activación de `CpSequencer`, el objetivo de la **Etapa 8.4F.1** fue investigar la causa raíz del bloqueo sin recurrir a suposiciones, instrumentar el flujo de ejecución concurrente entre `Thread_CpSequencer` y `Thread_Gpu`, implementar una solución quirúrgica y verificar la corrección en pruebas automatizadas y en el ejecutable real de *Astro Bot*.

---

## 2. Diagnóstico Técnico y Trazabilidad de Causa Raíz

### 2.1. Instrumentación y Captura de Telemetría en Vivo
Se incorporó instrumentación temporal de depuración en [`cpSequencer.h`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/src/graphics/guest_gpu/command_processor/cpSequencer.h) y [`graphicsRun.cpp`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/src/graphics/guest_gpu/graphicsRun.cpp) para monitorear el progreso de secuencias de envío (`ResolveSubmission`), estados del buffer circular de snapshots (`SnapshotRing`) y operaciones procesadas en `Thread_Gpu`.

La traza de telemetría capturada (`boot_diag_b.log`) reveló el momento exacto del bloqueo:

```text
[GPU_PROC] seq=3 type=2 queue=0 (SubmissionType::SuspendPoint)
[RES_SUB] enter seq=5
GraphicsRenderDispatchDirect: compute buffer fill ...
[SNAPSHOT_RING] AcquireBlocking STALLED: acq=0 rel=1 cap=256
[RES_SUB] STALLED waiting for ops in seq=5
```

El secuenciador quedaba bloqueado en `AcquireBlocking()` indicando `acq=0, rel=1, cap=256`, y paralelamente `ResolveSubmission` quedaba en espera infinita de que el secuenciador terminara de publicar las operaciones del lote `seq=5`.

### 2.2. Las Tres Causas Raíz Concatenadas

#### Causa 1: Subdesbordamiento Aritmético Unsigned en `SnapshotRing::HasFree()`
En [`src/graphics/guest_gpu/command_processor/cpSequencer.h`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/src/graphics/guest_gpu/command_processor/cpSequencer.h), `m_acquired` y `m_released` son contadores atómicos/monótonos de tipo `uint64_t`:

```cpp
// Código original vulnerable:
[[nodiscard]] bool HasFree() const noexcept {
    return m_acquired - m_released.load(std::memory_order_seq_cst) < m_entries.size();
}
```

Si por cualquier asimetría de sincronización `m_acquired < m_released` (por ejemplo `m_acquired == 0` y `m_released == 1`), la resta entera sin signo realiza:
$$0\text{ULL} - 1\text{ULL} = 18\,446\,744\,073\,709\,551\,615\text{ULL} \quad (0\text{xFFFF'FFFF'FFFF'FFFF})$$
Como $18.44\times 10^{18} < 256$ es falso, `HasFree()` retorna permanentemente `false`.  
Consecuentemente, el bucle:
```cpp
while (!HasFree()) {
    // spin y yield indefinidos
}
```
en `AcquireBlocking()` entra en un deadlock irreversible de CPU al 100% en ese hilo, sin avanzar jamás.

#### Causa 2: Reseteo no Sincronizado de `SnapshotRing` en Flujo Activo
En [`src/graphics/guest_gpu/graphicsRun.cpp`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/src/graphics/guest_gpu/graphicsRun.cpp), `CommandProcessor::Reset()` ejecutaba incondicionalmente:
```cpp
if (m_sequencer != nullptr) {
    m_sequencer->ResetSnapshots();
}
```
`ResetSnapshots()` ponía a cero `m_acquired = 0; m_released = 0;`.  
Sin embargo, `SnapshotRing` es un búfer circular cuyos slots se indexan mediante `(m_acquired++) % capacity`. Al procesarse paquetes `CpOpDispatchReset` o límites de cuadro (`SuspendPoint`), `Thread_Gpu` todavía tenía operaciones en vuelo en el `OpStream` generadas *antes* del reseteo.  
Cuando `Thread_Gpu` ejecutaba estas operaciones previas y llamaba a `sequencer.Snapshots().Release()`, incrementaba `m_released` de 0 a 1, mientras que `m_acquired` acababa de ser reseteado a 0. Esto inducía deterministamente la condición `acq=0, rel=1` que detonaba el subdesbordamiento de la Causa 1.

#### Causa 3: Carrera de Datos en `SubmissionType::SuspendPoint` sobre `Thread_Gpu`
En `GuestGpu::Process()`:
```cpp
case SubmissionType::SuspendPoint:
    m_renderer.GetCommandScheduler().DeferPriorityOperation(
        [ready = m_suspend_point_ready] { ready->release(); });
    cp.BufferFlush();
    cp.Reset();
    break;
```
Cuando `cp.Sequenced()` está activo, los registros de contexto (`cp.m_ctx`, `cp.m_ucfg`, `cp.m_sh_ctx`) pertenecen y son mutados concurrentemente por `Thread_CpSequencer`.  
Al llamar `cp.Reset()` directamente desde `Thread_Gpu` en cada `SuspendPoint` (límite de cuadro), `Thread_Gpu` pisoteaba los registros de la GPU huésped mientras `Thread_CpSequencer` ya se encontraba decodificando el lote del siguiente cuadro ($N+1$), y reiniciaba los snapshots concurrentemente.

---

## 3. Correcciones Implementadas

Las correcciones se realizaron de manera estrictamente localizada y no invasiva:

### 3.1. Protección contra Subdesbordamiento en `SnapshotRing`
En [`src/graphics/guest_gpu/command_processor/cpSequencer.h`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/src/graphics/guest_gpu/command_processor/cpSequencer.h):
```cpp
[[nodiscard]] bool HasFree() const noexcept {
    const auto rel       = m_released.load(std::memory_order_seq_cst);
    const auto in_flight = m_acquired >= rel ? (m_acquired - rel) : 0ull;
    return in_flight < m_entries.size();
}
```
Si existe desfase temporal donde $rel > acq$, `in_flight` se evalúa a `0ull`, garantizando que `HasFree()` retorne `true` y el secuenciador pueda continuar adquiriendo ranuras sin bloquearse.

### 3.2. Eliminación de Reseteo Espurio de Snapshots
En [`src/graphics/guest_gpu/graphicsRun.cpp`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/src/graphics/guest_gpu/graphicsRun.cpp):
- Se eliminó la llamada a `m_sequencer->ResetSnapshots()` dentro de `CommandProcessor::Reset()`.
- Los snapshots son ahora exclusivamente drenados y reciclados mediante el ciclo de vida natural de `AcquireBlocking()` y `Release()`. El reseteo duro de contadores sólo se realiza al desasociar el secuenciador (`DetachSequencer()`) en el apagado.

### 3.3. Aislamiento de Registros de `Thread_CpSequencer` en `SuspendPoint`
En [`src/graphics/guest_gpu/graphicsRun.cpp`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/src/graphics/guest_gpu/graphicsRun.cpp):
```cpp
case SubmissionType::SuspendPoint:
    m_renderer.GetCommandScheduler().DeferPriorityOperation(
        [ready = m_suspend_point_ready] { ready->release(); });
    cp.BufferFlush();
    if (!cp.Sequenced()) {
        cp.Reset();
    }
    break;
```
En modo secuenciado, el hilo secuenciador es el dueño exclusivo del estado de decodificación PM4, evitando que `Thread_Gpu` interfiera con el contexto del cuadro subsiguiente.

### 3.4. Prueba Unitaria de Regresión
En [`tests/CpSequencerTests.cpp`](file:///F:/IA_LOCAL/GITHUB/KYTYPS5/tests/CpSequencerTests.cpp), se añadió la suite:
- `TestSnapshotRingUnderflowProtection()`:
  - Simula desincronización artificial (`acq=0, rel=1`).
  - Valida que `HasFree()` no subdesborde y permanezca en `true`.
  - Comprueba que `AcquireBlocking()` adquiera slots válidos sucesivamente.
  - Verifica la saturación completa a 256 elementos, la aplicación correcta de backpressure, el reciclaje tras liberaciones y el drenado final a 0.

---

## 4. Resultados de Compilación y Validación de Tests

### 4.1. Compilación
El proyecto compila limpiamente en Release con Ninja sin advertencias ni errores:
```cmd
ninja -C _Build/windows
```

### 4.2. Ejecución de Suites Automatizadas (CTest)
```cmd
ctest --test-dir _Build/windows --output-on-failure -R "^(audit_profiler|stream_buffer_ring|pipeline_cache|cadence|cp_sequencer|draw_prep)"
```

**Resultado:**
```text
    Start 12: audit_profiler
1/4 Test #12: audit_profiler ...................   Passed    0.08 sec
    Start 13: cp_sequencer
2/4 Test #13: cp_sequencer .....................   Passed    0.05 sec
    Start 14: draw_prep
3/4 Test #14: draw_prep ........................   Passed    0.01 sec
    Start 84: stream_buffer_ring
4/4 Test #84: stream_buffer_ring ...............   Passed    0.61 sec

100% tests passed out of 4
Total Test time (real) = 0.78 sec
```

---

## 5. Validación en Vivo en Astro Bot (`PPSA21567`)

Se ejecutaron pruebas de arranque en vivo de 30 segundos sobre el ejecutable real `_Build/windows/kyty_emulator.exe` comparando los tres perfiles:

| Perfil | Configuración CLI | Comportamiento Inicial (Antes) | Comportamiento Final (Etapa 8.4F.1) | Estado |
|---|---|---|---|:---:|
| **Perfil A** | `--no-cp-sequencer --no-draw-prep` | Arranca normalmente | Arranca normalmente, supera logos, audio y gráficos estables | **OK** |
| **Perfil B** | `--cp-sequencer --no-draw-prep` | Congelado en 1er logo (deadlock `AcquireBlocking`) | Supera 1er logo, procesa 626+ sumisiones, flips continuos, renderizado de fuentes y eventos de equeue | **CORREGIDO** |
| **Perfil C** | `--cp-sequencer --draw-prep --draw-prep-workers 4` | Congelado en 1er logo (deadlock `AcquireBlocking`) | Supera 1er logo y pantallas de presentación, procesa draws paralelos y superficies de UI | **CORREGIDO** |

Tanto el Perfil B como el Perfil C han quedado completamente desatascados, ejecutándose de forma fluida y sin deadlocks.

---

## 6. Estado de Configuración y Resumen de Git

### 6.1. Flags por Defecto
De conformidad estricta con las reglas del repositorio (`AGENTS.md`):
- `--cp-sequencer`: **Desactivado por defecto** (requiere activación explícita).
- `--draw-prep`: **Desactivado por defecto** (requiere activación explícita).
- `--draw-state-dirty-check`: **Desactivado por defecto** (regresión Sky Garden).

### 6.2. Resumen de Cambios (`git diff --stat`)
```text
 src/graphics/guest_gpu/command_processor/cpSequencer.h |  4 ++-
 src/graphics/guest_gpu/graphicsRun.cpp                 |  7 ++--
 tests/CpSequencerTests.cpp                             | 41 ++++++++++++++++++++++
 3 files changed, 47 insertions(+), 5 deletions(-)
```

**Conclusión:** El problema de congelamiento en el arranque de Astro Bot con `CpSequencer` y `DrawPrep` ha sido resuelto y validado rigurosamente mediante telemetría, suites de pruebas unitarias y pruebas de ejecución en vivo.
