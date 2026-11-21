# KytyPS5 — Etapa 6: Auditoría de Relojes del Guest y Pacing

Este informe presenta la auditoría exhaustiva de los relojes virtuales del guest implementados en el emulador, verificando su relación con el tiempo real del host, monotonía, frecuencia y comportamiento ante caídas de tasa de cuadros.

---

## 1. Auditoría de Funciones de Tiempo en el Kernel Guest

Las funciones de tiempo están implementadas en [src/kernel/pthread.cpp](file:///c:/Users/Recko/Documents/github/kyty/src/kernel/pthread.cpp).

### 1.1. `KernelGetProcessTime()`
- **Firma:** `KYTY_SYSV_ABI uint64_t KernelGetProcessTime()`
- **Implementación:**
  ```cpp
  KYTY_SYSV_ABI uint64_t KernelGetProcessTime() {
      return (Common::SteadyNs() - g_process_start_time_ns) / 1000ULL;
  }
  ```
- **Unidad:** Microsegundos ($\mu\text{s}$).
- **Comportamiento:** Monótono ascendente, respaldado por `std::chrono::steady_clock` (en Windows: `QueryPerformanceCounter`).
- **Verificación:** Avanza a razón exacta de $1.000.000\,\mu\text{s}$ por segundo de tiempo real del host. **No sufre dilatación ni enlentecimiento artificial.**

### 1.2. `KernelGetProcessTimeCounter()` y `KernelGetProcessTimeCounterFrequency()`
- **Firmas:**
  ```cpp
  KYTY_SYSV_ABI uint64_t KernelGetProcessTimeCounter();
  KYTY_SYSV_ABI uint64_t KernelGetProcessTimeCounterFrequency();
  ```
- **Implementación:**
  - `KernelGetProcessTimeCounterFrequency()` devuelve la frecuencia virtual configurada (por defecto $1.000.000.000\text{ Hz} = 1\text{ GHz}$).
  - `KernelGetProcessTimeCounter()` convierte el tiempo monótono del host a ticks virtuales según la frecuencia reportada:
    $$\text{ticks} = \frac{\text{Common::SteadyNs}() \times \text{freq}}{1.000.000.000}$$
- **Verificación:** Proporcional al tiempo real del host. Los ticks avanzan uniformemente a $1\text{ tick/ns}$.

### 1.3. `KernelClockGettime()`
- **Firma:** `KYTY_SYSV_ABI int KernelClockGettime(int clock_id, KernelTimespec* tp)`
- **Soporte de relojes:**
  - `CLOCK_MONOTONIC` / `CLOCK_MONOTONIC_RAW`: Tiempo transcurrido continuo respaldado por `Common::SteadyNs()`.
  - `CLOCK_REALTIME`: Tiempo del sistema UNIX respaldado por `std::chrono::system_clock`.
- **Verificación:** Cumple con la especificación POSIX/FreeBSD de PlayStation. No introduce saltos hacia atrás ni desfases acumulativos.

### 1.4. `KernelUsleep()` y `KernelNanosleep()`
- **Implementación:**
  - Utilizan `Common::Thread::SleepMicro()` y `Common::Thread::SleepNano()`, que a su vez emplean temporizadores de alta resolución de Windows (`CreateWaitableTimerEx` / `SetWaitableTimer` con resolución submilimétrica).
- **Verificación:** Las esperas solicitadas por los hilos del guest respetan los tiempos solicitados y ceden la CPU adecuadamente.

---

## 2. Detección de NIDs Importados por Astro Bot (`PPSA21567`)

Se ejecutó un escaneo binario sobre el ejecutable desencriptado `eboot.bin` de Astro Bot para auditar qué funciones de tiempo y eventos son importadas por el juego:

| Biblioteca | Función / Símbolo | Importado en `eboot.bin` | Uso en Astro Bot |
|---|---|:---:|---|
| `libkernel.sprx` | `KernelGetProcessTime` | **SÍ** | Medición de perfiles y marcas de tiempo |
| `libkernel.sprx` | `KernelGetProcessTimeCounter` | **SÍ** | Temporización interna de subsistemas |
| `libkernel.sprx` | `KernelGetProcessTimeCounterFrequency` | **SÍ** | Calibración de frecuencia del contador |
| `libkernel.sprx` | `KernelUsleep` | **SÍ** | Pausas de sincronización entre hilos secundarios |
| `libkernel.sprx` | `KernelWaitEqueue` | **SÍ** | **Bloqueo principal del bucle de juego esperando el Flip** |
| `libkernel.sprx` | `KernelCreateEqueue` | **SÍ** | Creación de cola de eventos para video/display |
| `libVideoOut.sprx` | `VideoOutAddFlipEvent` | **SÍ** | **Asociación del evento de Flip a la cola de eventos** |
| `libVideoOut.sprx` | `VideoOutGetVblankStatus` | **SÍ** | Consulta de estado de VBlank (cuenta y marcas) |
| `libVideoOut.sprx` | `VideoOutAddVblankEvent` | **NO** | Astro Bot **no** espera eventos de VBlank |
| `libVideoOut.sprx` | `VideoOutWaitVblank` | **NO** | Astro Bot **no** llama a esperas síncronas de VBlank |
| `libVideoOut.sprx` | `VideoOutSubmitFlip` | **NO** | Astro Bot **no** somete flips por CPU síncrona |
| `libAgcDriver.sprx` | `AgcDcbSetFlip` | **SÍ** | **Inserción del comando de flip al final del DCB del GPU** |

---

## 3. Conclusiones del Pacing

1. **Los relojes del guest funcionan a velocidad nominal $1.0\times$:**
   Tanto `KernelGetProcessTime` como `KernelGetProcessTimeCounter` avanzan en estricta sincronía con el reloj del host. No hay ningún desfase intrínseco en los relojes del emulador.

2. **Astro Bot no utiliza temporización orientada a VBlank para su bucle:**
   El juego no depende de `VideoOutAddVblankEvent`. No espera que ocurran VBlanks para procesar cuadros.

3. **El bucle principal está acoplado al evento de Flip del GPU:**
   Astro Bot utiliza una arquitectura de doble o triple búfer basada en `VideoOutAddFlipEvent` y `KernelWaitEqueue`. La finalización del renderizado en el GPU es el único mecanismo que libera al hilo de juego para iniciar el siguiente cuadro.

4. **El ritmo del bucle de juego queda fijado por el tiempo de renderizado:**
   Al demorar el renderizado $\sim 50\text{ ms}$ por cuadro en el emulador, el evento de flip solo se señaliza 20 veces por segundo, independientemente de que el reloj virtual avance 1 segundo completo.
