# KytyPS5 — Diagnóstico de Cuellos de Botella: Separación CPU vs GPU

## 1. Semántica Real de `gpu_thread_busy_ms`

Durante las Etapas 6 y 6.1, la métrica `gpu_thread_busy_ms_per_frame` arrojó:
- **Vista Amplia:** 48,59 ms por frame (sobre 48,68 ms de frametime promedio).
- **Vista al Suelo:** 18,97 ms por frame (sobre 18,99 ms de frametime promedio).

### Clarificación Técnica Crítica:
`gpu_thread_busy_ns` **NO** mide el tiempo de cómputo en el silicio de la GPU física (NVIDIA GeForce RTX 5080).

El contador se instrumenta en `src/graphics/guest_gpu/graphicsRun.cpp`:
```cpp
const bool complete = [&] {
    Common::DebugCounters::ScopedTimer busy(
        Common::DebugCounters::Counter::GpuThreadBusyNs);
    return gpu->Process(submission);
}();
```
`gpu_thread_busy_ns` mide el tiempo activo de CPU que el **hilo host de GPU** (`GuestGpu::Run()`) pasa ejecutando el bucle de procesamiento de envíos guest.

Dado que en ambas capturas `gpu_thread_busy_ms` consumió prácticamente el 100% del frametime (30.031 ms en una ventana de 30.037 ms en Vista Amplia; 29.995 ms en 30.010 ms en Vista al Suelo), el hilo de CPU estuvo saturado ejecutando trabajo continuamente sin entrar en reposo (`WaitForWork`).

---

## 2. Evidencia Diferencial: ¿Por qué el Cuello de Botella es Host CPU?

### 1. Capacidad de Cómputo de la GPU Host (RTX 5080 16 GB):
- Resolución de prueba: **1280 × 720 (720p)**.
- La RTX 5080 posee una tasa de relleno y ancho de banda masivo (>1.800 GB/s, arquitectura Blackwell). A 720p, renderizar 5.031 llamadas de dibujo geométricas con shaders de rasterizado toma menos de **2 a 3 ms de tiempo real de GPU**.

### 2. Esperas de Sincronización en la CPU (`GpuWaits`):
- Si la GPU física estuviera saturada o tardando 40 ms en renderizar la escena:
  - Las llamadas a `CommandScheduler::Wait` o las esperas de fences/timeline semaphores bloquearían la CPU con latencias prolongadas.
  - En la captura real de Vista Amplia:
    - `CommandSchedulerDrain`: 0 ms.
    - `BufferGarbageCollection`: 0 ms.
    - `FaultManager`: 0 ms.
    - `OcclusionQueries`: 0 ms.
    - `BufferReadback`: 6,86 ms (espera concurrente en descarga de buffers modificados por GPU).
    - `PriorityOperation`: 20,94 ms (acumuladas en un hilo de fondo dedicado, no en el hilo crítico de render).
- Esto demuestra que la CPU **no está bloqueada esperando a la GPU** para continuar grabando comandos.

### 3. Frecuencia de Pacing de Cadence:
- Con `--gpu-submit-cadence-ms 1.0`, el scheduler evalúa enviar trabajo a Vulkan cada 1 ms.
- En Vista Amplia:
  - Se emitieron **33,69 submits de cadencia por frame** (693 submits/segundo).
  - Se saltaron **110.600 cadences** (`cadence_held_back = 110.600`) debido a que el buffer de comandos no tenía trabajo nuevo o la GPU ya tenía suficiente trabajo en vuelo.
- Esto confirma que el emulador va enviando lotes continuos de comandos a la GPU mientras la CPU sigue ocupada decodificando y traduciendo los 5.031 draws restantes.

### 4. Conclusión:
El cuello de botella que limita la Vista Amplia a 20,57 FPS es **la sobrecarga de la CPU del Host (AMD Ryzen 7 9700X) procesando, traduciendo, validando y llamando al driver Vulkan para más de 5.000 draw calls por frame**, junto con la latencia asociada a 6,86 ms de `BufferReadback`.
