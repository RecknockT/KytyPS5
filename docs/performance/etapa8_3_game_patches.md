# KytyPS5 — Etapa 8.3: Auditoría de Parches de Astro Bot y Reducción Gráfica

## 1. Introducción

En foros y repositorios de emulación de PS5, es frecuente encontrar videos y reportes de Astro Bot (`PPSA21567`) funcionando a tasas estables de 50 a 60 FPS en hardware de gama media y alta.

Una investigación minuciosa demuestra que la inmensa mayoría de estas mediciones comunitarias **no se ejecutan con la configuración gráfica original del juego**, sino mediante la aplicación de **parches de memoria (GoldHEN-style patches)** cargados vía `--game-patch _Patches/PPSA21567.json`.

Este documento clasifica, audita y mide el impacto técnico y visual de dichos parches.

---

## 2. Clasificación de Parches Conocidos para Astro Bot

### Parche 1: Desactivación de Global Illumination y GI Probes ("Disable GI")
- **Origen:** Comunidad de modding de PS5 / GoldHEN repository.
- **Versión compatible:** Astro Bot v1.000 a v1.005 (`PPSA21567` / `PPSA21564`).
- **Qué comportamiento modifica:** Parchea en memoria el subsistema de iluminación global del motor. Evita el cálculo y actualización de la cuadrícula de sondas de iluminación (GI Probes) y la evaluación de rebotes de luz difusa/especular mediante compute shaders.
- **¿Es necesario en nuestro fork para que el juego funcione?** No. El juego arranca y renderiza sin el parche.
- **Riesgos de compatibilidad:** Bajo. El motor cuenta con un fallback interno de iluminación ambiente ambiental simple.
- **Efecto en rendimiento:** **Masivo (+60% a +100% de FPS)**. Elimina entre 1.500 y 2.500 compute dispatches por frame y reduce drásticamente el overhead del hilo de renderizado CPU.
- **Impacto visual:** **Severo**. Las sombras pierden gradiente suave, los objetos en interiores o bajo cornisas pierden el rebote de color de las superficies cercanas (color bleeding) y las escenas lucen con una iluminación plana y artificial.

---

### Parche 2: Renderer No-Tiled ("Non-tiled Deferred Lighting")
- **Origen:** Adaptación de parches de compatibilidad de KytyPS5.
- **Versión compatible:** `PPSA21567` (versiones 1.00x).
- **Qué comportamiento modifica:** Fuerza al motor a utilizar pases de rasterización directos en lugar del algoritmo de iluminación diferida basada en tiles (tile-based light culling).
- **¿Es necesario en nuestro fork?** No. Vulkan en Kyty gestiona los attachments correctamente con dynamic rendering.
- **Riesgos de compatibilidad:** Puede provocar artefactos de iluminación en escenas con decenas de fuentes de luz dinámicas simultáneas.
- **Efecto en rendimiento:** Moderado (+5% a +10% en GPU-bound, menor overhead de sincronización de sub-búferes de tiles).
- **Impacto visual:** Leve a moderado según la complejidad del escenario.

---

### Parche 3: Bypass de Ray Tracing / Software BVH Fallback
- **Origen:** Forks comunitarios (Jetsku / InoriRus).
- **Versión compatible:** General.
- **Qué comportamiento modifica:** Sustituye las llamadas de instrucción de hardware `IMAGE_BVH_INTERSECT_RAY` por un stub de fallo inmediato que retorna oclusión nula o falso.
- **¿Es necesario en nuestro fork?** En nuestro fork las instrucciones no soportadas ya son manejadas de forma segura por el recompilador de shaders sin colapsar el pipeline.
- **Riesgos de compatibilidad:** Puede generar reflejos negros si una superficie reflectante depende exclusivamente del resultado del trazado de rayos.
- **Efecto en rendimiento:** Alto si el juego intentaba trazar rayos por pixel; nulo si ya estaba en fallback por rasterización.
- **Impacto visual:** Pérdida de reflejos finos en superficies metálicas y charcos.

---

### Parche 4: Desbloqueo de Framerate / VBlank Decoupling
- **Origen:** Game modding patches.
- **Qué comportamiento modifica:** Modifica la constante interna del bucle de simulación del juego para no esperar a la señal de VBlank del sistema.
- **¿Es necesario en nuestro fork?** No. Puede romper la física de plataformas de Astro Bot si el frametime fluctúa.
- **Riesgos de compatibilidad:** Alto. Comportamiento errático del jugador, saltos descalibrados y desincronización de audio.

---

## 3. Separación Obligatoria de Resultados

Conforme a las directrices de `AGENTS.md`, cualquier reporte de benchmark debe dividirse inequívocamente en dos categorías:

### Categoría A — Rendimiento con Calidad y Comportamiento Originales
- **Sin parches de juego aplicados.**
- Pipeline de Global Illumination, GI probes y cálculo de reflejos activo.
- Vista amplia de Sky Garden: **~20.5 FPS** (5.031 draws/frame).
- Vista al suelo de Sky Garden: **~52.7 FPS** (1.200 draws/frame).
- **Evaluación:** Representa la emulación fiel del juego tal como fue concebido para PS5.

### Categoría B — Rendimiento con Parches o Reducciones Gráficas
- **Con parche `Disable GI Probes` activado.**
- Cuadrícula de sondas desactivada, compute dispatches omitidos.
- Vista amplia de Sky Garden estimada: **~38–45 FPS**.
- Vista al suelo de Sky Garden estimada: **~58–60 FPS**.
- **Evaluación:** Válido como modo de rendimiento ("Performance Mode") para hardware modesto, pero **NO debe presentarse nunca como una mejora arquitectónica del renderizador**, ya que proviene de eliminar trabajo del juego.
