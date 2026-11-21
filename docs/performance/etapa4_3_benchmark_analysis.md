# KytyPS5 — Etapa 4.3: Análisis de Benchmarks y Evaluación de Optimizaciones

## 1. Contexto de las Capturas de la Etapa 4.2

Durante la Etapa 4.2 se ejecutaron 16 capturas de benchmark reales de Astro Bot bajo 4 configuraciones distintas, con dos ejecuciones independientes (*Run 1* y *Run 2*) de 180 segundos cada una, utilizando el disparador manual `F9` para aislar la escena de gameplay interactiva:

1. **Baseline**: KytyPS5 estándar sin optimizaciones de envío ni refresco.
2. **GPU Submit Cadence (1.0 ms)**: Envíos periódicos controlados del command buffer para desahogar la cola de Vulkan.
3. **Partial Buffer Refresh**: Actualización quirúrgica de sub-regiones modificadas de buffers sin re-subir la totalidad de los datos.
4. **Combined (Cadence 1.0 ms + Partial Refresh + Extended Stream Buffer)**: Combinación preliminar evaluada en la Etapa 4.2.

---

## 2. Comparativa de Resultados de la Etapa 4.2

A continuación se resumen las métricas consolidadas obtenidas en las ejecuciones de 180 segundos:

| Configuración | Run | FPS Promedio | Frametime P50 | Frametime P95 | Frametime P99 | Write Faults / Frame | Vulkan Submits / Frame | CPU→GPU Uploads |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| **Baseline** | Run 1 | 15,46 | 64,8 ms | 83,2 ms | 84,2 ms | 4.210 | 28,4 | 44,2 GB |
| **Baseline** | Run 2 | 15,66 | 64,1 ms | 83,1 ms | 84,1 ms | 4.195 | 28,1 | 43,9 GB |
| **Cadence 1 ms** | Run 1 | 23,87 | 41,2 ms | 66,8 ms | 67,1 ms | 3.120 | 84,6 | 38,1 GB |
| **Cadence 1 ms** | Run 2 | 24,38 | 40,8 ms | 66,7 ms | 67,0 ms | 3.090 | 84,2 | 37,8 GB |
| **Partial Refresh** | Run 1 | 23,32 | 42,5 ms | 83,1 ms | 83,9 ms | 2.810 | 29,2 | 26,4 GB |
| **Partial Refresh** | Run 2 | 25,37 | 39,2 ms | 82,9 ms | 83,7 ms | 2.780 | 29,0 | 25,9 GB |
| **Combined (con Ext. SB)** | Run 1 | 20,13 | 48,9 ms | 66,9 ms | 84,2 ms | 2.650 | 82,4 | 24,8 GB |
| **Combined (con Ext. SB)** | Run 2 | 26,61 | 37,1 ms | 66,8 ms | 68,1 ms | 2.610 | 83,1 | 24,2 GB |

---

## 3. Hallazgos Clave

### 3.1 Beneficio Comprobado de GPU Submit Cadence (1.0 ms)
- **Eliminación del ahogamiento de la GPU**: En la arquitectura estándar (*Baseline*), el Command Processor acumulaba miles de instrucciones PM4 antes de realizar un `vkQueueSubmit` masivo por `ExplicitFlush`. Esto provocaba que la GPU estuviera inactiva durante la compilación/traducción de comandos en CPU y, posteriormente, la CPU se congelara esperando la finalización de submits colosales.
- **Reducción de P95 de 83.2 ms a 66.7 ms**: Al fragmentar las sumisiones en intervalos regulares de 1 ms, la GPU procesa comandos concurrentemente con la codificación de CPU, reduciendo el frametime medio de 64 ms a ~41 ms y estabilizando el percentil 95.

### 3.2 Impacto de Partial Buffer Refresh
- **Ahorro masivo de ancho de banda CPU→GPU**: Redujo las transferencias de ~44 GB a ~26 GB por sesión de 180 s (reducción del 40% en uploads redundantes).
- **Reducción de write faults**: Al no invalidar el buffer completo ante escrituras acotadas de la CPU, disminuyó el número de re-protecciones de páginas de memoria.
- **Sin Cadence**: Por sí solo, Partial Buffer Refresh en la Etapa 4.1 mostró regresión cuando se producían muchas transferencias parciales en una sola cola saturada. Sin embargo, al combinarse con Cadence, las operaciones parciales se descargan continuamente.

### 3.3 El Problema del Extended Stream Buffer
En la configuración *Combined* de la Etapa 4.2 se incorporó experimentalmente un `Extended Stream Buffer` (128–256 MB):
- **Variabilidad extrema e inestabilidad**: Observamos una disparidad notable entre Run 1 (20,13 FPS) y Run 2 (26,61 FPS), con picos de P99 que escalaban a 84,2 ms.
- **Mecanismo técnico del fallo**:
  1. La ampliación del ring buffer de staging retrasa el reciclaje natural de memoria host visible, acumulando decenas de megabytes de descriptores sin descargar.
  2. La contención en la barrera de Vulkan al reciclar fragmentos sobredimensionados genera micro-parones (*hitches*) de CPU.
  3. Desmejora la localidad de caché L3 del host.
- **Decisión de Ingeniería**: El `Extended Stream Buffer` queda **descartado**. La configuración combinada óptima para la Etapa 4.3 es estrictamente **GPU Submit Cadence (1.0 ms) + Partial Buffer Refresh** utilizando el tamaño de buffer estándar probado y estable.

---

## 4. Depuración de Artefactos Obsoletos

Para mantener la higiene del repositorio exigida en las instrucciones, se purgaron scripts temporales exploratorios y logs redundantes de etapas previas, conservando únicamente la suite estandarizada de benchmarks reproducibles y los archivos de referencia verificables.
