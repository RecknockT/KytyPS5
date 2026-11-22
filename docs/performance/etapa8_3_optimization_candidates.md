# KytyPS5 — Etapa 8.3: Candidatos de Optimización de Otros Forks y Hoja de Ruta

## 1. Introducción

El análisis cruzado del motor gráfico de KytyPS5 con otros forks comunitarios (especialmente `Jetsku/KytyPS5`) y las mediciones de telemetría de las Etapas 5 a 8.2 permiten identificar qué optimizaciones son viables, cuáles ya están implementadas y cuáles presentan riesgos inaceptables de regresión gráfica.

---

## 2. Matriz de Evaluación de Candidatos de Optimización

| Optimización | Fork de Origen | ¿Existe en nuestro fork? | Riesgo Gráfico | Dificultad de Implementación | Beneficio Estimado / Medido |
|---|:---:|:---:|:---:|:---:|:---:|
| **1. Descriptor Cache (Fast-path bindings)** | Nuestro fork (Etapa 8.1) | **Sí (Activado)** | Nulo (Validado) | Alta | **+3.0% FPS** (medido en 8.1) |
| **2. Pipeline Cache MRU & Key Cache** | Nuestro fork (Etapas 7 y 8) | **Sí (Activado)** | Nulo (Validado) | Media | **+6.5% FPS acumulado** (medido) |
| **3. Redundant Bind Elision** | Nuestro fork (Etapa 7) | **Sí (Activado)** | Nulo (Validado) | Media | **+4.5% FPS** (medido) |
| **4. Eager Fluid Readbacks (`READBACK_EAGER_WRITES`)** | Jetsku (U59 int22) | **Parcial** (`batch_hot_readbacks`) | Nulo (Memoria CPU/GPU) | Media | **+20% a +30% FPS** en niveles de fluidos (esponja) |
| **5. Coalescencia de Read Faults** | Nuestro fork (Etapa 8.2) | **Sí (Implementado)** | Nulo (Validado) | Baja | Reduce un 40% las interrupciones SEH en lecturas |
| **6. Program Cache MRU (`GetGraphicsPrograms`)** | Nuestro fork (Etapa 8.2) | **Sí (Implementado)** | Nulo (Validado) | Baja | **+1.5% a +2% FPS** al elidir lookups de mapa en draws repetidos |
| **7. Decodificación Secuencial PM4 (`CP_SEQ`)** | Jetsku | No | Medio | Alta | +2% a +4% en CPU bound por menor bifurcación en despacho |
| **8. Precompilación de Pipelines (`.kyplst`)** | Jetsku | **Nativo** (`WarmUp` en `_PipelineCache`) | Nulo | Baja | Elimina tirones en primera ejecución |
| **9. Medición Real de Tiempo GPU (Timestamps)** | Nuestro fork (Etapa 8.2) | **Sí (Implementado)** | Nulo (Diagnóstico) | Media | Separa 100% el tiempo de GPU física del hilo host |
| **10. Parche de Desactivación de GI Probes** | Comunidad GoldHEN | **Vía `--game-patch`** | **Alto** (Pérdida de iluminación original) | Nula (Archivo JSON) | **+60% a +100% FPS** (Modo rendimiento visualmente degradado) |
| **11. Draw State Dirty Check** | Nuestro fork (Etapa 8) | **Desactivado** | **Inaceptable** (Sombras del cielo, destellos) | Muy Alta | Inviable sin reescribir todo el subsistema de attachments |

---

## 3. Análisis Técnico de las Principales Oportunidades

### 1. `KYTY_READBACK_EAGER_WRITES` (Readback anticipado de fluidos)
- **Problema:** En el nivel de la esponja de Astro Bot, la simulación física de agua y espuma se computa en la GPU y se escribe en búferes de memoria compartida. Posteriormente, los hilos de juego de la CPU leen dicha memoria. Cada lectura provocaba un fallo de página, obligando al hilo CPU a detenerse y esperar a que la GPU terminara (`BufferReadbackWait`), reduciendo el rendimiento a ~40 FPS.
- **Solución de Jetsku:** Identificar las páginas modificadas frecuentemente por compute shaders y transferirlas de forma proactiva al host inmediatamente tras el submit de compute, antes de que la CPU las solicite.
- **En nuestro fork:** Ya disponemos del flag `--batch-hot-readbacks`, que agrupa estas lecturas. La integración completa de un modo proactivo (eager publish) resolverá definitivamente las caídas en las fases de simulación de fluidos.

### 2. Parche "Disable GI Probes": ¿Por qué no debe ser la optimización por defecto?
- Desactivar las sondas de iluminación global transforma a Astro Bot en un juego con iluminación estática y sombras recortadas.
- Si bien es un parche muy útil para usuarios con hardware antiguo que buscan jugar a 60 FPS a cualquier costo, en nuestro repositorio de desarrollo técnico debemos mantener la calidad original como objetivo primario y ofrecer el parche como una **opción de prueba explícita (Modo B)**.

---

## 4. Hoja de Ruta Priorizada

1. **Prioridad 1 (Completado):** Consolidar la desactivación de `draw-state-dirty-check` y validar la ausencia de regresiones visuales.
2. **Prioridad 2 (Completado):** Implementar la medición directa de tiempo de GPU física (`gpu-timestamps`) para saber con precisión cuántos milisegundos reales consume la RTX 5080.
3. **Prioridad 3:** Crear el script de benchmark interactivo `stage8_3_astrobot_configuration_benchmark.cmd` para permitir al usuario probar de forma instantánea el preset optimizado, el preset conservador, la caché fría/caliente y las comparativas de renderizado.
