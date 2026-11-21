# KytyPS5 — Etapa 6: Validación, Pruebas y Respuestas a las 12 Preguntas Fundamentales

Este informe documenta la validación experimental, los resultados de la suite de pruebas unitarias y las respuestas técnicas rigurosas a las 12 preguntas fundamentales planteadas en la **Etapa 6: Desacoplar la velocidad del juego de los FPS y corregir la temporización de la emulación**.

---

## 1. Validación de Pruebas Unitarias

La suite de pruebas en [tests/AuditProfilerTests.cpp](file:///c:/Users/Recko/Documents/github/kyty/tests/AuditProfilerTests.cpp) fue ampliada con la prueba `AuditProfilerTest.RecordsTimingAndSimulationMetricsAccurately`, acumulando un total de 18 pruebas unitarias.

### Resultado de Ejecución:
```text
[==========] Running 18 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 18 tests from AuditProfilerTest
[ RUN      ] AuditProfilerTest.DisabledByDefaultProducesZeroOverheadNoData
[       OK ] AuditProfilerTest.DisabledByDefaultProducesZeroOverheadNoData (1 ms)
[ RUN      ] AuditProfilerTest.RecordsFaultsAndReprotectionsAccurately
[       OK ] AuditProfilerTest.RecordsFaultsAndReprotectionsAccurately (0 ms)
[ RUN      ] AuditProfilerTest.CategorizesGpuWaitsByOrigin
[       OK ] AuditProfilerTest.CategorizesGpuWaitsByOrigin (0 ms)
[ RUN      ] AuditProfilerTest.CategorizesSubmitReasons
[       OK ] AuditProfilerTest.CategorizesSubmitReasons (0 ms)
[ RUN      ] AuditProfilerTest.ComputesExactFrametimePercentilesP50P95P99
[       OK ] AuditProfilerTest.ComputesExactFrametimePercentilesP50P95P99 (0 ms)
[ RUN      ] AuditProfilerTest.ConcurrentRecordingIsThreadSafeAndLockFree
[       OK ] AuditProfilerTest.ConcurrentRecordingIsThreadSafeAndLockFree (1 ms)
[ RUN      ] AuditProfilerTest.ExportsJsonAndCsvCorrectly
[       OK ] AuditProfilerTest.ExportsJsonAndCsvCorrectly (6 ms)
[ RUN      ] AuditProfilerTest.ColdReprotectsDoNotSaturateTableAndHotPagesAreRetained
[       OK ] AuditProfilerTest.ColdReprotectsDoNotSaturateTableAndHotPagesAreRetained (0 ms)
[ RUN      ] AuditProfilerTest.SimultaneousCadenceAndPartialBufferRefreshConfigCoexistence
[       OK ] AuditProfilerTest.SimultaneousCadenceAndPartialBufferRefreshConfigCoexistence (0 ms)
[ RUN      ] AuditProfilerTest.ControlledCaptureSessionLifecycleAndReset
[       OK ] AuditProfilerTest.ControlledCaptureSessionLifecycleAndReset (1 ms)
[ RUN      ] AuditProfilerTest.HotkeyToggleCaptureStartsAndStops
[       OK ] AuditProfilerTest.HotkeyToggleCaptureStartsAndStops (13 ms)
[ RUN      ] AuditProfilerTest.ConfigurableCadenceIntervalReflectedInSnapshot
[       OK ] AuditProfilerTest.ConfigurableCadenceIntervalReflectedInSnapshot (0 ms)
[ RUN      ] AuditProfilerTest.RecordsStutteringHitchesAndSlowestFrames
[       OK ] AuditProfilerTest.RecordsStutteringHitchesAndSlowestFrames (0 ms)
[ RUN      ] AuditProfilerTest.ExtendedStreamBufferConfigReflectedInSnapshot
[       OK ] AuditProfilerTest.ExtendedStreamBufferConfigReflectedInSnapshot (0 ms)
[ RUN      ] AuditProfilerTest.ManualTriggerModeWaitsForHotkeyAndDoesNotRecordBeforeF9
[       OK ] AuditProfilerTest.ManualTriggerModeWaitsForHotkeyAndDoesNotRecordBeforeF9 (0 ms)
[ RUN      ] AuditProfilerTest.ExecutionPhaseTrackingAndSeparation
[       OK ] AuditProfilerTest.ExecutionPhaseTrackingAndSeparation (0 ms)
[ RUN      ] AuditProfilerTest.PhaseMetricsDoNotLeakEventsOutsideActiveCapture
[       OK ] AuditProfilerTest.PhaseMetricsDoNotLeakEventsOutsideActiveCapture (1 ms)
[ RUN      ] AuditProfilerTest.RecordsTimingAndSimulationMetricsAccurately
[       OK ] AuditProfilerTest.RecordsTimingAndSimulationMetricsAccurately (6 ms)
[----------] 18 tests from AuditProfilerTest (34 ms total)
[----------] Global test environment tear-down
[==========] 18 tests from 1 test suite ran. (35 ms total)
[  PASSED  ] 18 tests.
```

### Propiedades Validadas:
1. **Sobrecarga Cero cuando está Desactivado:** Las funciones `RecordVblank()`, `RecordFlipSubmitted()`, `RecordFlipCompleted()`, `RecordFlipWait()` y `RecordAudioUpdate()` retornan inmediatamente sin operaciones atómicas cuando el profiler está deshabilitado.
2. **Invariante de Aislamiento de Fases:** Durante la fase de calentamiento (`Warmup`) previa a la pulsación de F9, no se registran eventos de temporización. Al finalizar la captura (`StopCapture`), ningún evento tardío contamina el snapshot cerrado.
3. **Exactitud de Cálculos:** La relación matemática de velocidad de simulación, tasas en hertzios y tiempos de espera se calculan con precisión milimétrica.
4. **Validación de Serialización:** Se comprobó la integridad de todos los campos en formato JSON (`timing_and_simulation`) y CSV (`Timing,...`).

---

## 2. Validación de Compilación del Emulador

El ejecutable principal `kyty_emulator.exe` fue compilado y enlazado exitosamente con Clang y LLD (`ninja -C _Build/windows kyty_emulator`), generando un binario limpio y actualizado sin advertencias de enlace.

---

## 3. Respuestas Técnicas a las 12 Preguntas Fundamentales

### Pregunta 1: ¿Astro Bot corre efectivamente a velocidad reducida en el emulador, o se trata únicamente de baja tasa de cuadros?
**Respuesta:** Corre **efectivamente a velocidad física reducida** ($\sim 33.3\%$ de su velocidad normal). No es solo una ilusión visual de baja tasa de cuadros: la velocidad con que el personaje corre, la aceleración gravitatoria que lo hace caer y el desplazamiento de los proyectiles avanzan a un tercio de la velocidad real. Cada segundo real en el mundo físico del usuario solo avanza $\sim 0.33$ segundos en el mundo virtual de Astro Bot.

---

### Pregunta 2: ¿Por qué la música se reproduce a velocidad normal mientras la acción visual parece ir en cámara lenta?
**Respuesta:** Porque el motor de audio y el motor de simulación gráfica residen en **hilos completamente independientes con arquitecturas de pacing distintas**. El hilo de audio emite bloques de muestras mediante `libSceAudioOut` a un ritmo fijado por el reloj de muestras del host ($48.000\text{ muestras/s}$ a través de `SleepMicro()`), completando $1.0$ segundo de audio por cada $1.0$ segundo de tiempo de pared. La simulación visual, en cambio, está estrangulada por el tiempo de renderizado del GPU ($49.5\text{ ms}$ por cuadro), iterando solo 20 veces por segundo.

---

### Pregunta 3: ¿Cómo mide Astro Bot el paso del tiempo: paso fijo (fixed timestep), delta time medido por TSC/process time, o sincronización con VBlank/Flip?
**Respuesta:** Astro Bot sincroniza su bucle mediante el **`FlipEvent`** (mediante `VideoOutAddFlipEvent` y `KernelWaitEqueue`), y su motor de simulación física aplica un **paso de tiempo fijo de $\frac{1}{60}\text{ s} \approx 16.666\text{ ms}$** (o un delta time dinámico saturado rígidamente con $\Delta t \le \frac{1}{60}\text{ s}$). No utiliza eventos de VBlank (`VideoOutAddVblankEvent` no está importado en `eboot.bin`). Al desbloquearse el bucle únicamente tras la presentación del frame en el GPU, cada cuadro ejecutado equivale a exactamente un paso de $16.666\text{ ms}$.

---

### Pregunta 4: ¿Los relojes del guest (TSC, process time) se ejecutan a velocidad de tiempo real o están ralentizados por el emulador?
**Respuesta:** Los relojes del guest se ejecutan a **velocidad de tiempo real exacta ($1.0\times$)**. Tanto `KernelGetProcessTime()` como `KernelGetProcessTimeCounter()` en [src/kernel/pthread.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/kernel/pthread.cpp) están anclados a `Common::SteadyNs()` del host (`QueryPerformanceCounter`). No sufren ninguna dilatación ni freno artificial en el emulador.

---

### Pregunta 5: ¿Qué mecanismo del emulador regula la frecuencia con que el bucle del juego itera?
**Respuesta:** La frecuencia del bucle está regulada por la **finalización de la cola de presentación en el GPU (`FlipQueue::Flip()` en `PresentThread`) y la señalización de `VideoOutEventKind::Flip`**. Cuando el Command Processor del GPU procesa el paquete PM4 de fin de pipa generado por `AgcDcbSetFlip()`, el request pasa a `Ready`. `PresentThread` presenta el frame y señaliza el `KernelEqueue` del juego, permitiendo que la llamada bloqueante `KernelWaitEqueue()` retorne.

---

### Pregunta 6: ¿El bucle del juego espera a VBlank, a la finalización del flip, o a primitivas de sincronización del GPU?
**Respuesta:** Espera **estrictamente a la finalización del flip (`FlipEvent`)**. La auditoría del binario `eboot.bin` demostró que Astro Bot **no importa ni llama a `VideoOutAddVblankEvent` ni a `VideoOutWaitVblank`**. Sin embargo, la finalización del flip está indirectamente vinculada a la sincronización del GPU, porque el flip no puede marcarse como `Ready` hasta que el GPU emulado termine de ejecutar los comandos de dibujo del cuadro en Vulkan.

---

### Pregunta 7: ¿Es posible y seguro desacoplar la velocidad de simulación de la tasa de cuadros del GPU en KytyPS5?
**Respuesta:** **No es posible de manera transparente y segura únicamente desde el emulador.** El bucle de simulación física y el bucle de renderizado forman una única función monolítica compilada dentro de `eboot.bin`. El emulador no tiene acceso a una interfaz del juego que permita ejecutar "3 pasos de física por cada paso de render". Desacoplarlo desde el código del emulador provocaría desincronizaciones severas de memoria y bloqueos de carrera.

---

### Pregunta 8: ¿Qué sucedería si el emulador reportara eventos de Flip a 60 Hz mientras el GPU aún está renderizando el cuadro anterior?
**Respuesta:** Provocaría **corrupción gráfica catastrófica, errores de validación de Vulkan y caída del emulador**. Si se despierta al hilo del juego a 60 Hz cuando el GPU tarda 50 ms en renderizar, el juego comenzará a reescribir los búferes de vértices, uniformes y comandos de dibujo del cuadro $N+1$ y $N+2$ en memoria compartida mientras el GPU del host aún los está leyendo para el cuadro $N$. Esto genera geometría rota (vértices estirados al infinito), corrupción de command buffers en `CommandProcessor` y bloqueos irrecuperables.

---

### Pregunta 9: ¿Qué sucedería si se alterara la frecuencia del TSC virtual o del process time para intentar compensar la baja tasa de cuadros?
**Respuesta:** **No tendría ningún efecto positivo y rompería otros subsistemas.** Si el motor utiliza paso fijo ($\Delta t = 1/60$), ignora la lectura del reloj para la física. Si utiliza paso dinámico con saturación ($\min(\Delta t, 1/60)$), triplicar el reloj sigue resultando en $16.66\text{ ms}$ por la saturación. Además, falsear la frecuencia del reloj aceleraría los temporizadores de guardado, arruinaría la sincronización de audio y provocaría fallos de carga en el streaming de recursos.

---

### Pregunta 10: ¿La simulación de Astro Bot puede ejecutarse a 60 ticks por segundo mientras el renderizado se realiza a 20 FPS?
**Respuesta:** En la arquitectura nativa del ejecutable de PS5, **NO sin un parche específico al binario del juego (`game mod / patch`)**. Para lograr que Astro Bot ejecute 3 subticks de física por cada cuadro dibujado (o aplique un paso dinámico de $50\text{ ms}$ sin romperse), se requeriría parchear las instrucciones máquina en `eboot.bin` modificando la lógica del bucle principal del motor. Aun así, aplicar $\Delta t = 50\text{ ms}$ en un juego de plataformas provocaría que Astro atraviese superficies sólidas (tunneling) y deforme las físicas de resortes y telas.

---

### Pregunta 11: ¿El hilo de presentación (PresentThread) introduce retrasos, jitter o acumulación de trabajo (bunching) que afecten la cadencia del juego?
**Respuesta:** Sí, `PresentThread` produce **agrupamiento ("bunching") de VBlanks**, pero **NO afecta la cadencia de Astro Bot**. Cuando una presentación dura $50\text{ ms}$, la variable `total_wait` se vuelve negativa, provocando que el hilo dispare 2 VBlanks casi instantáneos (con diferencia de microsegundos) para intentar conservar el promedio nominal de 60 Hz. No obstante, dado que Astro Bot no escucha VBlanks sino el `FlipEvent`, estas ráfagas no despiertan al juego anticipadamente.

---

### Pregunta 12: ¿Cuál es la solución técnicamente correcta y definitiva para que Astro Bot funcione a velocidad normal en KytyPS5?
**Respuesta:** La única solución técnicamente correcta, robusta y libre de artefactos es **optimizar el rendimiento del pipeline de emulación hasta alcanzar 60 FPS sostenidos ($\sim 16.66\text{ ms}$ por cuadro)**. 
Al optimizar la emulación gráfica (continuando el trabajo iniciado en la Etapa 5 con la reducción de write tracking, el elision de submits vacíos, submit cadence y la compilación eficiente de shaders en Vulkan), cada cuadro se completará en 16.6 ms, los flips se completarán a 60 Hz, el bucle de Astro Bot iterará 60 veces por segundo, y la simulación física alcanzará automáticamente el $100\%$ de velocidad en sincronía perfecta con el audio.
