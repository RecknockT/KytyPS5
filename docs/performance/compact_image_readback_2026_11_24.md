# Descargas compactas de imágenes lineales

## Investigación

Se comparó el código publicado, no solo promesas de FPS:

- Oficial: https://github.com/KytyPS5/KytyPS5/blob/main/src/graphics/host_gpu/renderer/cache/textureCache.cpp — DownloadImageMemory prefilla y publica toda la reserva, conservando padding. Se parece a nuestra ruta actual.
- Jetsku/U59 (base de la GPU de AstroHybrid): https://github.com/Jetsku/KytyPS5/blob/main/src/graphics/host_gpu/renderer/cache/textureCache.cpp — conserva el prefill general y añade guardas contra escrituras CPU durante una publicación pendiente. No se copió todo el fork ni su infraestructura de publicación.
- shadPS4: https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/texture_cache/texture_cache.cpp — construye transferencias a partir del tamaño/layout de mipmaps. Se toma como referencia la separación entre transferencia y reserva, sin trasladar supuestos de layouts PS4 a PS5.
- Erlou: https://github.com/Erlou93/KytyPS5-Erlou/releases — documenta sensibilidad de lecturas BAR y mantiene readback para corrección. No se importan sus resultados numéricos de otro juego/equipo ni los flags de omisión de trabajo.

La búsqueda de PRs abiertos/cerrados no identificó un parche directo verificable para aplicar a esta ruta. No constituye un censo completo. La adaptación es propia y conserva las condiciones de descarga del motor.

## Cambio

`--compact-image-readback` / `--no-compact-image-readback`, inicialmente apagado. `CompactLinearReadbackSize` demuestra que una copia de un mip, una capa y un formato lineal empaquetado sobrescribe un prefijo contiguo completo. Solo se aceptan formatos explícitos cuyo tamaño host coincide con bytes_per_block y cuyo backing Vulkan mantiene el mismo formato. Se excluyen depth/stencil, BC, tiling, metadata, conversiones de color, volumen, MSAA, capas múltiples, mipmaps y filas con padding.

En ese caso se valida la cobertura de backing sin copiar sus bytes, se reserva solo el prefijo, se omite el prefill/flush host y se publica únicamente el contenido de texels. El padding permanece en guest y una escritura CPU posterior en ese padding sobrevive. Barreras, invalidación de caché host, tick de publicación, protección de páginas y orden de finalización se conservan. No cambia cuándo se exige el readback ni el resultado de los texels. No soluciona toda carrera preexistente de escrituras concurrentes sobre texels.

Layouts excluidos conservan el camino completo. El flag puede cambiarse al inicio de una operación GPU para A/B. `stage8_live_play.ps1 -ManualAB -ABMode compact_image_readback` conserva el joystick del usuario; no inyecta botones ni cierra el juego. El armado requiere confirmar la escena fija. Existe además un ensayo exclusivo del título sin botones (`stage8_compact_readback_title_auto.ps1`), que NO se ejecutó: el usuario indicó que hará la prueba de juego con su joystick.

## Corrección del significado de los contadores

TextureDownloadBytes contabilizaba info.data.size, incluida reserva/padding que Vulkan no necesariamente transfiere. Los 107 MiB/cuadro de la sesión anterior son bytes de reservas solicitadas, no ancho de banda PCIe medido. Se conserva ese contador para comparar ambos modos sin falsear una reducción. Los nuevos contadores independientes son CompactReadbackCopies/Fallbacks, ReadbackPrefillAvoidedBytes y ReadbackStagingSavedBytes; se exportan como deltas de cada captura JSON/CSV.

## Verificación

Release compilado. Seis tests aprobados: compact_image_readback, audit_profiler, read_buffer_reuse, resource_prep_reuse, stream_buffer_ring y cadence_runtime. `_Build/windows/compact_readback_tests.log`.

El test Vulkan compara encendido/apagado: texels de R32Uint, padding por fila y cola de reserva, escritura en padding después de grabar la copia, rechazo de CPU dirty, cobertura de backing y liberación después del unmap. Verifica que la protección de lectura de storage queda armada hasta terminar y se libera. El plan rechaza layouts no admitidos y tamaños host incompatibles. AuditProfiler verifica cambio diferido, reset de configuración y baselines por captura.

Prueba aislada ABBA: 64 descargas por muestra de una imagen 512×512 R32Uint (1 MiB de texels) dentro de una reserva de 4 MiB. Controles: 19,986 / 19,706 ms; candidatos: 4,860 / 4,730 ms. Promedios 19.846 / 4.795 ms. Incluye las finalizaciones GPU del caso sintético y comprueba que las 64 llamadas realmente usan la ruta compacta cuando se habilita. Evidencia `_Build/windows/compact_readback_microbenchmark.json` y `.log`.

**No son FPS de Astro Bot y no prueban una ganancia de Sky Garden.** No se ha realizado el A/B de juego de esta optimización. No se cambiaron los ajustes persistentes del launcher ni relojes, VBlank, audio o calidad.

Ejecutable SHA256: `ba48453169035d859ee42f651a7c14dbe0da2570bdd381e9adc1b0efb6c3d36f`.
