# KytyPS5 — Etapa 6: Diagnóstico de Velocidad de Simulación y Desacoplamiento

Este informe presenta la investigación técnica y matemática definitiva sobre por qué Astro Bot (`PPSA21567`) se ejecuta en cámara lenta ($\sim 33.3\%$ de su velocidad normal) cuando la tasa de cuadros ronda los $\sim 20\text{ FPS}$, mientras la música de fondo se reproduce a velocidad normal ($100\%$). Asimismo, analiza la viabilidad y límites de un desacoplamiento desde el emulador.

---

## 1. Demostración Matemática del Efecto "Cámara Lenta"

### 1.1. Los Datos de Rendimiento
En la Etapa 5 (`stage5_cadence_partial.json`), las mediciones continuas de 180 segundos en Astro Bot arrojaron los siguientes valores en fase de gameplay interactivo:
- **Cuadros capturados:** 3,634 cuadros en 180.05 segundos.
- **Tasa efectiva:** $20.184\text{ FPS}$.
- **Frametime promedio:** $49.56\text{ ms}$.
- **P50:** $49.98\text{ ms}$, **P95:** $50.70\text{ ms}$, **P99:** $66.69\text{ ms}$.

### 1.2. La Arquitectura del Bucle de Simulación de Astro Bot
Como juego exclusivo de PlayStation 5 diseñado para un hardware de rendimiento constante a 60 cuadros por segundo, el motor de Astro Bot utiliza una de las siguientes dos estrategias estándar de temporización de simulación:

1. **Paso de Simulación Fijo (Fixed Timestep):**
   Cada iteración del bucle principal avanza el mundo de juego y la física exactamente:
   $$\Delta t = \frac{1}{60}\text{ segundos} \approx 16.6667\text{ ms}$$

2. **Paso Dinámico con Límite Superior Estricto (Clamped Delta Time):**
   El motor lee el contador de tiempo (`KernelGetProcessTimeCounter()`), calcula $\Delta t = \text{ahora} - \text{anterior}$, y aplica una saturación estricta para evitar inestabilidades numéricas, explosión de muelles y colisiones atravesadas (tunneling):
   $$\Delta t = \min\left(\Delta t_{\text{medido}}, \frac{1}{60}\text{ s}\right)$$

### 1.3. El Cálculo del Tiempo Simulado por Segundo de Tiempo Real
En 1 segundo ($1.000\text{ ms}$) de tiempo real en el host:
1. El GPU emulador tarda $\sim 49.5\text{ ms}$ en procesar los comandos de dibujo y sombreadores de cada cuadro.
2. El bucle de juego se desbloquea a través del `FlipEvent` únicamente al completarse cada cuadro en el GPU.
3. Por consiguiente, en 1 segundo de tiempo real se ejecutan exactamente:
   $$N_{\text{iteraciones}} = \text{FPS} \approx 20.18\text{ cuadros}$$
4. El tiempo virtual acumulado por la física y animaciones del juego en ese segundo real es:
   $$t_{\text{simulado}} = N_{\text{iteraciones}} \times \Delta t = 20.18 \times \left(\frac{1}{60}\text{ s}\right) \approx 0.3364\text{ segundos}$$
5. La relación de velocidad de simulación respecto a la realidad es:
   $$\text{Ratio de Velocidad} = \frac{t_{\text{simulado}}}{t_{\text{real}}} = \frac{20.18}{60.0} \approx 33.6\%$$

**Conclusión:** El mundo de juego (movimiento de Astro, caída de objetos, físicas de plataformas, animaciones de enemigos) avanza exactamente a **un tercio de su velocidad normal**. Esto produce una percepción perfecta de "cámara lenta".

---

## 2. ¿Por qué la Música se Reproduce a Velocidad Normal (100%)?

Contrario a la lógica visual, la música de fondo y los efectos de sonido avanzan en perfecto tiempo real debido a la arquitectura desacoplada de audio del sistema operativo de PS5 (`libSceAudioOut`):

1. **Hilo de Audio Dedicado e Independiente:**
   El motor de sonido ejecuta un hilo independiente del bucle de renderizado.
2. **Pacing por Reloj de Muestras (Sample Clock):**
   Como se auditó en [src/libs/audio.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/libs/audio.cpp), cada bloque de audio somete $S = 512$ muestras a una frecuencia de muestreo de $f_s = 48000\text{ Hz}$.
3. El tiempo de duración de cada bloque es:
   $$T_{\text{bloque}} = \frac{512}{48000} \approx 10.67\text{ ms}$$
4. El emulador regula la entrega mediante `port.clock.Advance()` y duerme con `SleepMicro(deadline - now)` utilizando el reloj de pared del host.
5. En 1 segundo de tiempo real del host, el emulador reproduce exactamente:
   $$\text{Muestras por segundo} = \frac{1.0\text{ s}}{10.67\text{ ms}} \times 512 \approx 48.000\text{ muestras} = 100.0\%\text{ de velocidad}$$

**Resultado:** El audio está acoplado al reloj de pared del host ($1.0\times$), mientras que la simulación física del juego está acoplada a la tasa de finalización del GPU ($\sim 0.33\times$).

---

## 3. Análisis de Viabilidad de Desacoplamiento desde el Emulador

A menudo surge la hipótesis de si el emulador puede "desacoplar" la velocidad de simulación de los FPS sin modificar el juego. A continuación se analizan técnicamente todas las opciones posibles:

### Opción A: Señalizar "Flips Falsos" a 60 Hz mientras el GPU sigue renderizando a 20 FPS
- **Idea:** Disparar el `FlipEvent` 60 veces por segundo independientemente de que el GPU tarde 50 ms.
- **Consecuencia Técnica Inmediata:**
  1. Si se señaliza el `FlipEvent` a los 16.6 ms, el hilo de Astro Bot asume que el frame $N$ ha sido presentado y que los búferes de comandos y de vértices/uniforms están libres.
  2. El juego comienza inmediatamente a sobreescribir la memoria del frame $N+1$ mientras el GPU del emulador **aún está leyendo y dibujando los recursos del frame $N$**.
  3. **Resultado:** Colapso total de sincronización. Errores de validación de Vulkan por colisión en `vkQueueSubmit`, texturas corruptas, vértices descontrolados (vértices estirados al infinito) y caída irrecuperable en `CommandProcessor` por paquetes PM4 sobrescritos en el anillo del Command Buffer.

### Opción B: Acelerar los Relojes del Guest (`guest_tsc` / `process_time` $\times 3$)
- **Idea:** Multiplicar artificialmente los valores devueltos por `KernelGetProcessTime()` o `KernelGetProcessTimeCounter()`.
- **Consecuencia Técnica Inmediata:**
  1. Si el motor usa un paso fijo ($\Delta t = 1/60$), la lectura de los relojes es ignorada para la integración física, por lo que acelerar el reloj no altera en absoluto la velocidad de la física.
  2. Si el motor usa paso dinámico pero tiene saturación ($\Delta t = \min(\Delta t, 1/60)$), multiplicar el reloj por 3 hace que $\Delta t_{\text{medido}} = 150\text{ ms}$, pero la función $\min$ lo satura igualmente en $16.66\text{ ms}$. La velocidad sigue idéntica.
  3. Además, acelerar el reloj destruye la sincronización de tiempos en red, guardados de partida, temporizadores de menús y produce desincronizaciones de streaming de activos.

### Opción C: Forzar Múltiples Pasos de Simulación por Cuadro desde el Emulador
- **Idea:** Hacer que el emulador "haga correr la simulación 3 veces" antes de emitir los comandos de dibujo.
- **Consecuencia Técnica Inmediata:**
  1. El bucle de juego está compilado dentro del binario nativo de PlayStation 5 (`eboot.bin`).
  2. El juego **no expone** una API externa como `SimulateTick()`. La simulación física y la emisión de comandos gráficos (`AgcDcbDrawIndexAuto`, etc.) forman un único cuerpo monolítico en la función de actualización de escena del juego.
  3. El emulador es un ejecutor de instrucciones x86_64, no un framework de scripting que pueda aislar y re-ejecutar subfunciones internas arbitrarias del ejecutable sin decompilación inversa.

### Opción D: Modificación del Binario del Juego (Game Patch / 60 FPS Mod)
- **Idea:** Parchear las instrucciones en `eboot.bin` donde se define el paso de tiempo o el multiplicador de velocidad (técnica análoga a los parches de 60 FPS en RPCS3, ShadPS4 o emuladores de consolas previas).
- **Viabilidad:** Es la **única vía** para alterar la velocidad interna sin cambiar la tasa de cuadros del GPU, pero requiere ingeniería inversa estática en IDA Pro / Ghidra sobre el binario `eboot.bin` para localizar las constantes de punto flotante de delta time y anular la saturación.
- **Riesgo:** Un $\Delta t$ de $50\text{ ms}$ en un juego de plataformas con físicas complejas como Astro Bot provoca que el personaje atraviese el suelo, que la gravedad actúe con impulsos excesivos y que los resortes y cuerdas se vuelvan inestables.

---

## 4. La Verdadera Solución de Ingeniería
La única solución robusta, arquitectónicamente correcta y que preserva la fidelidad de la emulación es:

$$\textbf{Incrementar el rendimiento de emulación del pipeline gráfico hacia 60 FPS}$$

Cuando las optimizaciones del emulador (como la reducción de write faults de la Etapa 5, el submit cadence y el soporte eficiente de Vulkan) eleven el framerate a 60 FPS:
- Cada cuadro tardará $\sim 16.66\text{ ms}$.
- Los flips se señalizarán 60 veces por segundo.
- El bucle de juego iterará 60 veces por segundo.
- En 1 segundo real se integrarán $60 \times (1/60\text{ s}) = 1.0\text{ segundo}$ de simulación ($100\%$ de velocidad).
- El audio y el video quedarán sincronizados de manera nativa y sin artificios.
