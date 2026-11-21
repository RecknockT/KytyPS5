# KytyPS5 — Etapa 4.3: Diagnóstico de Sincronización, VBlank y Causa Raíz de la "Cámara Lenta" en Astro Bot

## 1. Planteamiento del Problema

En las ejecuciones reales de Astro Bot en KytyPS5 se observa un fenómeno recurrente:
- El frametime promedio se sitúa entre **48 ms y 60 ms** (equivalente a 16–21 FPS reales).
- Sin embargo, el juego no presenta un salto brusco de frames (*frameskipping*), sino una sensación persistente de **cámara lenta (*slow motion*)**, donde las animaciones, la física y los movimientos del personaje avanzan a aproximadamente un tercio o un cuarto de su velocidad nominal.
- El objetivo de esta investigación es determinar técnicamente:
  1. La relación entre el intervalo de VBlank del emulador y el frametime reportado.
  2. El mecanismo de acoplamiento entre la cola de presentación (`FlipQueue`), la instrucción PM4 `WAIT_FLIP_DONE` y el bucle de simulación del motor de Astro Bot.
  3. Por qué el juego se ralentiza en lugar de omitir frames.
  4. La separación rigurosa entre tiempo de renderizado real y tiempo de espera por sincronización.

---

## 2. Arquitectura de Presentación y Sincronización en KytyPS5

### 2.1 El Hilo de Presentación (`PresentThread`)

En [src/graphics/presentation/videoOut.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/presentation/videoOut.cpp#L811-L854), la clase `VideoOutDriver::Impl` mantiene un hilo dedicado `m_present_thread` que ejecuta la función `PresentThread`:

```cpp
void VideoOutDriver::Impl::PresentThread(std::stop_token token) {
    using Clock = std::chrono::steady_clock;
    const auto period = Clock::duration(std::chrono::seconds(1)) / Config::GetVblankFrequency();

    std::chrono::nanoseconds total_wait {};
    while (!token.stop_requested()) {
        const auto sleep_begin = Clock::now();
        if (total_wait.count() > 0) {
            Common::Thread::SleepNano(static_cast<uint64_t>(total_wait.count()));
        }
        ...
        VblankBegin();
        bool presented = m_flip_queue.Flip();
        ...
        VblankEnd();
        total_wait += period - (Clock::now() - frame_begin);
        if (total_wait.count() < 0) {
            Common::DebugCounters::Add(Common::DebugCounters::Counter::LateVblanks);
        }
    }
}
```

- La frecuencia nominal configurada es 60 Hz (`Config::GetVblankFrequency() == 60`), lo que establece un periodo estricto de VBlank de:
  $$\Delta t_{\text{VBlank}} = \frac{1000\text{ ms}}{60} \approx 16,666\text{ ms}$$
- El hilo despierta a intervalos de 16,66 ms. En cada despertar emite los eventos del sistema de `VblankBegin()`, llama a `FlipQueue::Flip()` y notifica a los oyentes de VBlank.

### 2.2 El Mecanismo de Flip y `WaitFlipDone`

Astro Bot utiliza la API gráfica de bajo nivel de PlayStation (AGC):
1. **Buffer Indexing**: El juego gestiona un esquema de doble o triple buffer para la pantalla (índices 0, 1, 2).
2. **Envío del Flip**: Al completar los comandos de dibujado de un frame, el juego inserta un paquete de flip (`sceVideoOutSubmitFlip` o el paquete de control PM4 `USERDATA_TAG_WAIT_FLIP_DONE`).
3. **Punto de Sincronización `WaitFlipDone`**: Antes de reutilizar el buffer $N$ para renderizar el siguiente frame, el hilo de renderizado o el Command Processor de la GPU ejecuta:
   ```cpp
   void VideoOutDriver::WaitFlipDone(const uint64_t* label) {
       auto* ctx = m_impl->GetByLabel(label);
       m_impl->GetFlipQueue().Wait(*ctx, static_cast<int>(label - ctx->labels.data()));
   }
   ```
   En [src/graphics/presentation/videoOut.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/graphics/presentation/videoOut.cpp#L1083-L1095):
   ```cpp
   void FlipQueue::Wait(VideoOutConfig& cfg, int index) {
       Common::LockGuard lock(m_mutex);
       auto has_request = [this, &cfg, index] {
           auto matches = [&cfg, index](const auto& r) { return r.cfg == &cfg && r.index == index; };
           return std::any_of(m_requests.begin(), m_requests.end(), matches) ||
                  std::any_of(m_cpu_requests.begin(), m_cpu_requests.end(), matches);
       };
       while (has_request()) {
           m_done_cond_var.Wait(&m_mutex);
       }
   }
   ```
   Esta llamada bloquea incondicionalmente el hilo solicitante hasta que `FlipQueue::Flip()` tome la solicitud, la presente en pantalla mediante Vulkan y ejecute `m_done_cond_var.SignalAll()`.

---

## 3. Causa Raíz de la "Cámara Lenta"

### 3.1 Acoplamiento Rígido Simulación-Presentación (Fixed Timestep)
El motor de Astro Bot está diseñado bajo la premisa de hardware de consola donde los frames se entregan a 60 FPS fijos. La simulación de físicas, estados de animación y tiempo de juego avanza mediante un paso de tiempo fijo:
$$\Delta t_{\text{sim}} = 16,66\text{ ms por frame}$$

El bucle del juego opera en estricto paso cerrado (*lockstep*):
$$\text{Simular } (t + 16,66\text{ ms}) \longrightarrow \text{Generar Comandos GPU} \longrightarrow \text{Esperar Flip anterior } (\texttt{WaitFlipDone})$$

### 3.2 Cuantización por VBlank cuando el Render Time Excede 16.66 ms
Cuando el emulador ejecuta Astro Bot:
1. El renderizado y traducción de comandos de un frame (CPU write tracking + traducción PM4 + ejecución Vulkan) toma aproximadamente **48 a 52 ms** de tiempo real (*wall-clock time*).
2. Durante ese intervalo, el hilo `PresentThread` despierta a los $16,66\text{ ms}$, pero el frame aún no está en estado `RequestState::Ready` (la GPU o la CPU aún están trabajando).
3. A los $33,33\text{ ms}$ (segundo VBlank), el frame aún no ha terminado.
4. A los $50,00\text{ ms}$ o $66,66\text{ ms}$ (tercer o cuarto VBlank), el frame finalmente alcanza el estado `Ready`. `FlipQueue::Flip()` lo presenta y desbloquea `m_done_cond_var`.
5. Recién en ese momento el juego sale de `WaitFlipDone` y comienza a simular el siguiente frame.

### 3.3 El Efecto Resultante
- En el mundo real transcurrieron **50 a 66 milisegundos**.
- En la lógica interna de Astro Bot, solo avanzó **1 frame de física (16,66 milisegundos)**.
- Razón de velocidad efectiva:
  $$\text{Velocidad} = \frac{16,66\text{ ms simulados}}{50\text{ ms reales}} \approx 33\% \text{ de la velocidad normal}$$
- El juego no descarta tiempo ni hace saltos de posición (*teleports*) porque no implementa delta-time elástico para su simulación principal. Todo el universo del juego se mueve a un tercio de su velocidad normal, produciendo la apariencia exacta de "cámara lenta".

---

## 4. Desglose del Presupuesto de Frame: Render Real vs. Esperas

A partir del análisis de las capturas de la Etapa 4.2 con `AuditProfiler`, el tiempo de un frame típico de 52 ms se distribuye de la siguiente manera:

```
[=============================== Frame: 52 ms ===============================]
| Handler Write Faults | Trad. PM4 & Vulkan | Esperas Readback / PriorityOp | Present/VBlank |
|       14 ms          |       22 ms        |            12 ms              |      4 ms      |
[=============================================================================]
```

- **Tiempo de Ejecución Real (CPU + GPU)**: ~48 ms.
- **Tiempo de Espera de Presentación**: ~4 ms (tiempo residual esperando el tick exacto del siguiente VBlank de 16,66 ms).
- **Conclusión**: El cuello de botella **no es una espera artificial en la presentación**, sino el costo computacional intrínseco del renderizado y la gestión de memoria compartida (write tracking y reprotecciones).

---

## 5. Hoja de Ruta Hacia la Etapa 5

Para eliminar la cámara lenta y alcanzar los 60 FPS nominales en Astro Bot, es indispensable reducir el tiempo de renderizado real de 48 ms a menos de 16,66 ms. La Etapa 5 deberá atacar directamente los mayores consumidores de tiempo identificados en la auditoría:

1. **Optimización del Manejador de Write Tracking**:
   - Actualmente se registran más de 3.000 a 4.000 fallos de página por frame (~14 ms acumulados en el handler de excepciones).
   - El 18,2% de los fallos provienen de la misma instrucción `memcpy/memset` (`0x0000000920003e55`).
   - Implementar agregación de páginas o desprotección diferida (*dirty page coalescing*).
2. **Sincronización Asíncrona de Buffer Readbacks**:
   - Reducir los 12 ms de esperas síncronas de CPU esperando que la GPU complete transferencias de retorno.
3. **Consolidación de GPU Submit Cadence**:
   - Mantener envíos cada 1.0 ms para asegurar que la GPU comience a dibujar mientras la CPU continúa traduciendo comandos de la escena.
