# Revisión de forks y PR sin fusionar — 23 de noviembre de 2026

## Alcance y criterio

Investigación de fuentes públicas del proyecto oficial y forks, incluidas colecciones de PR abiertos y cerrados y comentarios de cierre de los candidatos relevantes. No es un censo de toda la red ni una auditoría completa de cada diff. Los resultados ajenos son declaraciones de sus autores; no son mediciones de nuestra compilación.

Base local observada: `14185ebd9feed76015ebf8760ffad391e3c3ef50`, con los cambios locales de 8.4F y transferencias todavía sin commit. En esta investigación no se incorporó código externo ni se ejecutó el juego.

Se consultaron las colecciones de PR de Jetsku, chenxiao07 (dos páginas, 129 entradas), Senaxx, Erlou93, simnJS/GTA, usa-ratt/GT7, pcportastrobot, JonJon1992, mauroalvare/AstroBot, RedMadKnight/BC5 y dmorazasanchez/Native-beta. TheCruZ/GTA y mitschud/Fork-BK no devolvieron una colección utilizable por el conector; su cobertura de PR queda pendiente. También se buscaron PR oficiales por rendimiento, renderizador, caché y sincronización y se hizo búsqueda web para descubrir otros forks.

Un `merged_at=null` no demuestra que el código siga ausente: puede haberse incorporado manualmente, reescrito o reemplazado. Se distinguieron esos casos leyendo comentarios y contrastando partes del código local.

## Candidatos para nuestra siguiente etapa

| Prioridad | Fuente y estado verificado | Qué aporta y evidencia publicada | Aplicación local y condiciones |
|---|---|---|---|
| Alta, diagnóstico | [Jetsku #115](https://github.com/Jetsku/KytyPS5/pull/115), abierto; [oficial #1851](https://github.com/KytyPS5/KytyPS5/pull/1851), cerrado sin merge registrado | Esperas de timeline por hilo y motivo; el segundo añade sitio de llamada, regiones que fallan y shader propietario. No implican una ganancia de FPS. | Ya tenemos motivos de espera en `masterSemaphore.cpp` y AuditProfiler. Ampliar ese sistema con hilo, sitio y propietario; no duplicarlo con otro registrador. Faltan timestamps reales de GPU conectados al renderizador. |
| Alta, estabilidad | [Jetsku #137](https://github.com/Jetsku/KytyPS5/pull/137), abierto | Elimina envíos idénticos de efectos de gatillos DualSense. El autor encontró una cola SDL creciente y degradación tras más de una hora en Astro 01.019.000, Linux/Bluetooth. | `controller.cpp::SendTriggerEffect` local todavía envía cada petición. Candidato concreto si se usa DualSense; probar desconexión, cambio de intensidad, efecto de parada y reenvío tras error. No explica por sí solo nuestros ~16 FPS desde el comienzo. Separar esta corrección del parche de buffers guest incluido en el mismo PR. |
| Alta, estabilidad | [Jetsku #127](https://github.com/Jetsku/KytyPS5/pull/127), abierto | Amplía a 16 bits el contador de observadores de escritura de una página para evitar desbordar 255; el autor declara conservar `PageState` de 4 bytes. | Localmente sigue siendo `uint8_t` en `src/graphics/host_gpu/pageManager.cpp`. Verificar layout, incrementos/decrementos y prueba con más de 255 observadores. Previene un cierre; no promete acelerar. |
| Media, rendimiento CPU | [Jetsku #114](https://github.com/Jetsku/KytyPS5/pull/114), abierto | Usa dirty state actual y rangos exactos en vez de "alguna vez tocado por GPU"; deja pasar esperas ya satisfechas. UFC 5: ~7.500 paradas/s a 150–200/s; mejores pruebas 25–26 a 30–33 FPS en rama combinada, RTX 4090/i9-12900K. | Nuestro secuenciador tiene diferencias y correcciones de orden/deadlock propias. Medir primero A/B/C y contar causas de parada; portar solo si se observa ese problema. No permitir que una lectura del parser adelante escrituras pendientes del resolver. |
| Media, rendimiento CPU/GPU | [Jetsku #107](https://github.com/Jetsku/KytyPS5/pull/107), abierto | Separa presentación de bloqueos de VideoOut y evita cortar pases por submits opcionales. El autor publica ~2 FPS adicionales en Astro, equipo y sistema distintos. | Medir bloqueos de presentación y cortes de pases antes de adaptar. Mantener submits necesarios para progreso, visibilidad, esperas guest y otras colas. No adoptar cambios de frecuencia ni temporización. |
| Media, diseño para DrawPrep | [Erlou93 #23](https://github.com/Erlou93/KytyPS5-Erlou/pull/23), abierto, SHA `2316662b49aa56f23373fc8d06c944af87ff7c6a` | El helper agrupa lecturas contiguas para que el hilo de ejecución compare menos fragmentos. Autor: 3.026.324 verificaciones sin diferencias; último A/B 16,94 a 17,13 FPS y 57,3 a 56,7 ms de hilo de ejecución, RTX 4090/7950X. | Nuestra ruta no contiene `FastVerifyLoggedReads`/ese `ReadLog`. Es una idea para reducir validación en el hilo que confirma el trabajo, no un parche directo. Solo priorizar si nuestras mediciones muestran un coste equivalente. |
| Baja para el cuello actual, shader | [Erlou93 #3](https://github.com/Erlou93/KytyPS5-Erlou/pull/3), abierto | Cargas GLC de programas demostrablemente de solo lectura, fuera de bucles, pueden usar caché L1. RTX 5080/9800X3D: ~0,5 ms menos GPU; 3.883.971.142 comparaciones sin diferencias, pero cargas de 16 bits no verificadas. El autor no midió mejora de FPS porque limitaba la CPU. | Requiere análisis de escrituras/atomics/bucles y revisión de concurrencia y coherencia. No quitar decoraciones globalmente ni barreras. Cambia SPIR-V y debe separar/invalidar correctamente la caché de shaders según el modo. No resuelve el coste CPU dominante medido aquí. |
| Condicional, otros juegos | [oficial #2501](https://github.com/KytyPS5/KytyPS5/pull/2501), abierto | Agrupa y cachea escaneos de tablas de descriptores seleccionados por GPU. GTA V: hit de ~0,22 ms frente a ~26 ms por escaneo; la escena pasa del camino lento ~2 FPS a 12–13 FPS. | No se encontró `scan_cache`/`ScanCacheEntry` local. Necesita confirmar que Astro usa esa ruta; no es una solución general para todos los descriptores. El autor deja pendiente validar una normalización de OOB contra hardware. No incorporar todo el PR sin resolver esa advertencia. |

## Experimentos abiertos que no conviene importar ahora

Todos los siguientes están abiertos en Jetsku y declaran ser experimentos apagados por defecto, no preparados para fusionar tal cual. Se midieron en UFC 5 y no prueban qué sucedería en Astro:

- [#120: grabación multihilo de command buffers](https://github.com/Jetsku/KytyPS5/pull/120): ~91% de dibujos fuera del hilo original, pero **14 frente a 24 FPS**. Más paralelismo puede competir con los hilos que sí limitan.
- [#116: comparación paralela/hash de páginas](https://github.com/Jetsku/KytyPS5/pull/116): ambas alternativas más lentas que `memcmp`.
- [#117: importar memoria host](https://github.com/Jetsku/KytyPS5/pull/117): uploads bajan de 1,9 GB/s a 0,2–0,6 GB/s, sin mejora de FPS. Es evidencia contra optimizar solo contadores de transferencia.
- [#122: MEM_WRITE_WATCH](https://github.com/Jetsku/KytyPS5/pull/122): en su juego el 100% de visitas calientes era memoria directa, no elegible; cero visitas evitadas.
- [#119](https://github.com/Jetsku/KytyPS5/pull/119), [#121](https://github.com/Jetsku/KytyPS5/pull/121) y [#118](https://github.com/Jetsku/KytyPS5/pull/118): sin mejora medible. Los subtemporizadores de #121 sí sirven como referencia de diagnóstico.
- [#123: continuar secuencias al cambiar shaders](https://github.com/Jetsku/KytyPS5/pull/123): el autor indica que todavía no lo probó dentro del juego. No usar como base estable del siguiente lote.
- [AstroHybrid #185](https://github.com/imnotsharingmyname/pcportastrobot/pull/185): el propio autor pide no publicarlo y advierte problemas. No importar su paquete denominado Stability Mode.

## Cerrados sin merge registrado: qué ocurrió realmente

| PR | Resultado de leer los comentarios | Decisión |
|---|---|---|
| [Senaxx #17](https://github.com/Senaxx/KytyPS5/pull/17#issuecomment-6109421044) | Se retiró la ampliación del granulado de fallos porque congelaba el juego tras 1–4 minutos; la pausa del spinlock ya estaba upstream. | No portar. Nuestro `fault_ahead` tiene implementación y límites propios, no es este parche; mantenerlo apagado y exigir pruebas prolongadas si se vuelve a evaluar. |
| [Jetsku #63](https://github.com/Jetsku/KytyPS5/pull/63#issuecomment-6384596020) | El mantenedor dice que ya se incorporó y salió en una release. | Localmente `RangeSet::Add` ya conserva el nodo al extender su extremo derecho. No duplicar. |
| [chenxiao07 #356](https://github.com/chenxiao07/KytyPS5/pull/356#issuecomment-7012723859) | Cerrado a favor de #349: el bitmap `LabelPages::Set` reemplaza el conjunto que rehasheaba y causaba hitches. | Estudiar la solución vigente [#349](https://github.com/chenxiao07/KytyPS5/pull/349), no introducir la alternativa abandonada. Nuestra arquitectura no usa ese sistema de etiquetas especulativas. |
| [chenxiao07 #342](https://github.com/chenxiao07/KytyPS5/pull/342#issuecomment-6935159061) | Incorporado manualmente como `1bcf96af`, excluyendo el perfil PGO. GitHub no lo reconoce como merge del PR. | No considerarlo un paquete pendiente. Son 69 archivos de otra arquitectura; estudiar piezas justificadas por telemetría. |

Atención a la atribución: el autor de #342 [corrigió sus cifras posteriores](https://github.com/chenxiao07/KytyPS5/pull/342#issuecomment-6937182900): el control con la espera antigua también mejoró tras actualizar BIOS. Esas pruebas no demuestran que el cambio de lock produjera la ganancia inicialmente atribuida.

## Lo que ya tenemos y límites de comparabilidad

- La corrección de lecturas de archivo hacia páginas protegidas ya está representada en `src/kernel/guestFileRead.h`; no volver a importar [oficial #2356](https://github.com/KytyPS5/KytyPS5/pull/2356) como novedad.
- Descriptor cache, MRU y caché de claves ya existen. Encontrar un nombre parecido en otro fork no prueba una mejora adicional.
- Las variantes locales de readback vecino, ventana amplia y fault-ahead ya se probaron: [validación local](transfer_prefetch_gameplay_validation.md). El lote no mejoró apreciablemente los FPS y permanece apagado.
- Los resultados [Jetsku int22](https://github.com/Jetsku/KytyPS5/blob/u59-windows-20261118-int22/docs/EXPERIMENTAL.md) usan condiciones diferentes, incluidas configuraciones de VBlank y variantes con parches visuales. No atribuir una cámara exacta ni una reducción concreta de draws por GI sin una captura que lo demuestre. El documento antiguo `etapa8_3_jetsku_comparison.md` hace afirmaciones más fuertes que la evidencia revisada aquí.
- Los caminos GPU nativos para AMD BC-250 no son reemplazos directos del renderizador Vulkan de una RTX 5080. Frame generation, cambios de relojes y recortes de calidad tampoco cuentan como ganancias de emulación equivalentes.

## Orden propuesto

1. Completar A/B/C local con transferencias nuevas apagadas, misma cámara y aterrizaje delimitado; medir tiempo total, p95/p99, draws y compilaciones.
2. Añadir atribución de esperas por hilo/sitio y costes exclusivos de materialización, búsqueda y preparación de descriptores. Conectar timestamps GPU sin esperar sincrónicamente por cada consulta.
3. Revisar por separado #137 (solo controlador) y #127 como correcciones de estabilidad, con pruebas de su comportamiento y switches A/B cuando corresponda.
4. Elegir una sola optimización del hilo gráfico según el coste observado: reutilización de estado/recursos, menos paradas del secuenciador o menos cortes de pase. Adaptar y verificar, no fusionar forks completos.
5. Pruebas de escena estable, aterrizaje, agua/esponja y sesión larga; conservar solo ganancias reproducibles con imagen y sincronización correctas. PGO debe entrenarse después sobre nuestro código y cargas representativas.

Este informe no establece que ninguna de estas propuestas lleve nuestra máquina a 30 o 60 FPS. El objetivo inmediato es reducir el trabajo CPU real que actualmente supera el presupuesto de 33,3 ms por cuadro.
