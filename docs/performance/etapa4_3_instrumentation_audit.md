# KytyPS5 — Etapa 4.3: Auditoría y Corrección de la Instrumentación (AuditProfiler)

## 1. Resumen Ejecutivo

Durante las Etapas 4.1 y 4.2 se identificaron discrepancias en las métricas de rendimiento y en la segmentación de fases de Astro Bot. Una auditoría profunda del subsistema `AuditProfiler` reveló un defecto crítico en la gestión del ciclo de vida de la sesión: la **fuga de acumulación de fases pre-captura**.

Cuando el emulador operaba en modo de espera activa (*WARMUP*), esperando que el usuario alcanzara la escena de prueba reproducible y presionara `F9` para iniciar la captura de 180 segundos, los eventos del motor gráfico y de memoria (fallos de página, esperas GPU, reprotecciones, envíos Vulkan) ocurridos durante la fase de inicialización y carga continuaban acumulándose en el mapa interno de fases (`m_phase_metrics`). Al exportar el snapshot final de gameplay, las métricas de fase contenían datos contaminados acumulados desde el arranque.

En esta Etapa 4.3 se resolvió este defecto de forma definitiva, se garantizó la consistencia matemática entre los artefactos JSON y CSV, y se validó mediante tests unitarios automáticos bajo concurrencia.

---

## 2. Diagnóstico del Defecto en AuditProfiler

### 2.1 Causa Raíz

En `src/common/auditProfiler.cpp`, la estructura `Impl` gestiona el estado global de auditoría. La función `ResetCaptureSessionState()` se diseñó para reiniciar los contadores atómicos y la línea temporal de frametimes al pasar de `SessionState::Warmup` a `SessionState::Recording`.

Sin embargo, en la implementación original:
- Se reiniciaban los contadores de frametimes (`m_frametimes_ms.clear()`), los contadores globales y el timestamp de inicio `m_session_start_time`.
- **No se reiniciaba `m_phase_metrics`**.
- La función de registro por fase (`RecordPhaseEvent` / `RecordPhaseWait`) calculaba los deltas o sumaba directamente a la clave de fase actual (`ExecutionPhase::Warmup`, `ExecutionPhase::Loading`, `ExecutionPhase::Gameplay`).
- Como consecuencia, los eventos ocurridos antes de presionar `F9` persistían en las estructuras de métricas por fase. Al completarse la sesión de 180 segundos, el JSON final reportaba un número de eventos de fase muy superior a la suma de eventos capturados durante el intervalo activo de medición.

### 2.2 Corrección Implementada

Se modificó `Common::AuditProfiler::Impl::ResetCaptureSessionState()` en [src/common/auditProfiler.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/common/auditProfiler.cpp):

```cpp
void AuditProfiler::Impl::ResetCaptureSessionState() {
    // 1. Limpieza de frametimes e histogramas
    m_frametimes_ms.clear();
    m_frametimes_ms.reserve(16384);
    
    // 2. Reinicio de contadores atómicos globales
    m_total_frames.store(0, std::memory_order_relaxed);
    m_write_faults.store(0, std::memory_order_relaxed);
    m_reprotections.store(0, std::memory_order_relaxed);
    m_handler_time_ns.store(0, std::memory_order_relaxed);
    m_cpu_to_gpu_bytes.store(0, std::memory_order_relaxed);
    m_gpu_to_cpu_bytes.store(0, std::memory_order_relaxed);
    m_vulkan_submits.store(0, std::memory_order_relaxed);
    
    // 3. Reinicio riguroso de esperas GPU y razones de submit
    for (auto& wait : m_gpu_waits) {
        wait.count.store(0, std::memory_order_relaxed);
        wait.time_ns.store(0, std::memory_order_relaxed);
    }
    for (auto& reason : m_submit_reasons) {
        reason.store(0, std::memory_order_relaxed);
    }
    
    // 4. Limpieza y aislamiento estricto de métricas por fase (CORRECCIÓN CRÍTICA)
    {
        std::lock_guard<std::mutex> lock(m_phase_mutex);
        m_phase_metrics.clear();
        // Inicializar la fase de gameplay activa con contadores limpios a cero
        m_phase_metrics[m_current_phase] = PhaseAccumulator{};
    }

    // 5. Establecimiento del nuevo punto de referencia temporal
    m_session_start_time = std::chrono::steady_clock::now();
}
```

---

## 3. Validación de Consistencia JSON y CSV

Se auditó el mecanismo de exportación dual (`ExportJson` y `ExportCsv`):

1. **Snapshot Atómico Unificado**:
   Ambos formatos se generan ahora extrayendo una copia congelada (`Snapshot`) bajo el mismo cerrojo de lectura. Se elimina cualquier posibilidad de que frames o eventos registrados entre la generación del JSON y la del CSV produzcan métricas dispares.
2. **Normalización de Hot Pages**:
   Las direcciones de memoria se enmascaran rigurosamente a páginas de 4 KB (`addr & ~0xFFFull`). La tabla hash cerrada con resolución cuadrática evita que reprotecciones frías desalojen páginas con alta frecuencia de write faults.
3. **Métricas de Frametime y Percentiles**:
   El ordenamiento de frametimes para el cálculo de P50, P90, P95, P99 y P99.9 se realiza sobre una copia local inmutable.

---

## 4. Cobertura de Tests Unitarios

Se incorporó y verificó el test unitario `PhaseMetricsDoNotLeakEventsOutsideActiveCapture` en [tests/AuditProfilerTests.cpp](file:///c:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp):

- **Escenario**:
  1. El profiler se inicializa en modo WARMUP (`--audit-manual-trigger`).
  2. Se simulan 1.000 write faults y 50 ms de esperas GPU durante la fase previa a la captura.
  3. Se dispara `F9` (`StartCaptureSession`).
  4. Se registran 2 frames y 10 write faults en la fase activa.
  5. Se cierra la sesión (`StopCaptureSession`).
- **Verificación**:
  - `snapshot.write_faults` reporta exactamente 10.
  - `snapshot.phase_metrics[Gameplay].write_faults` reporta exactamente 10.
  - Ningún evento previo a `F9` se filtra en la sesión exportada.
- **Resultado**: 17 de 17 tests de `AuditProfilerTest` pasaron satisfactoriamente (100% pass).
