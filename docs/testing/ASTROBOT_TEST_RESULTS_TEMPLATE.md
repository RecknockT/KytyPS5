# Plantilla de Resultados de Pruebas: Astro Bot (PPSA21567) - Etapa 8.4E

Copie esta plantilla para registrar y compartir los resultados de las pruebas independientes de la Etapa 8.4E de KytyPS5.

---

## 1. Informacion del Evaluador y Hardware

- **Evaluador / Nickname**: `[Completar]`
- **Fecha de Prueba**: `[YYYY-MM-DD]`
- **CPU**: `[Ej: AMD Ryzen 7 9700X 8-Core / Intel Core i7-13700K]`
- **GPU**: `[Ej: NVIDIA GeForce RTX 4080 16 GB / AMD Radeon RX 7900 XT 20 GB]`
- **Version del Driver GPU**: `[Ej: NVIDIA 566.14 / AMD 24.10.1]`
- **Memoria RAM**: `[Ej: 32 GB DDR5-6000 CL30]`
- **Sistema Operativo**: `[Ej: Windows 11 Pro 24H2 64-bit]`
- **Resolucion de Renderizado**: `1280x720` (o indicar otra)

---

## 2. Informacion de la Compilacion de KytyPS5

- **Rama Git**: `main`
- **Commit SHA**: `[Ej: eff6f2c38 o posterior]`
- **Compilador utilizado**: `[Ej: Clang-cl 19.1.0 / MSVC 19.40]`
- **Tipo de Compilacion**: `Release (Ninja)`
- **Resultado de Unit Tests** (`ctest`): `[Pass 4/4 o indicar resultado]`

---

## 3. Matriz de Resultados de Rendimiento (A/B)

### Escenario A: Sky Garden — Vista Amplia (Overlook, 60 segundos)

| Metrica | Perfil A (Baseline Control) | Perfil B (PM4 + DrawPrep 8.4E) | Variacion (%) |
|---|:---:|:---:|:---:|
| **FPS Promedio (Avg)** | `XX.X` FPS | `XX.X` FPS | `+X.X%` |
| **1% Low FPS** | `XX.X` FPS | `XX.X` FPS | `+X.X%` |
| **0.1% Low FPS** | `XX.X` FPS | `XX.X` FPS | `+X.X%` |
| **Frametime Promedio (ms)** | `XX.X` ms | `XX.X` ms | `-X.X%` |
| **Hitches > 50 ms (Conteo)** | `XX` | `XX` | `[Variación]` |
| **Hitch Máximo Registrado (ms)** | `XX.X` ms | `XX.X` ms | `[Variación]` |

### Escenario B: Nivel de las Esponjas / Fluidos (Sponge Level, 60 segundos)

| Metrica | Perfil A (Baseline Control) | Perfil B (PM4 + DrawPrep 8.4E) | Variacion (%) |
|---|:---:|:---:|:---:|
| **FPS Promedio (Avg)** | `XX.X` FPS | `XX.X` FPS | `+X.X%` |
| **1% Low FPS** | `XX.X` FPS | `XX.X` FPS | `+X.X%` |
| **Frametime Promedio (ms)** | `XX.X` ms | `XX.X` ms | `-X.X%` |
| **Hitches > 50 ms (Conteo)** | `XX` | `XX` | `[Variación]` |

---

## 4. Telemetria del Modulo DrawPrep (Perfil B)

*(Obtenida del archivo JSON exportado en `_TestLogs/`)*

- **Total Draws Emitidos**: `[Ej: 145023]`
- **Draws Preparados en Paralelo (`draw_prep_published`)**: `[Ej: 144890]`
- **Draws Consumidos por Thread_Gpu (`draw_prep_consumed`)**: `[Ej: 144890]`
- **Porcentaje de Duplicacion (`draw_prep_duplication_pct`)**: `[Debe ser 0.0%]`
- **Tiempo promedio de preparacion por draw (µs)**: `[XX.X µs]`

---

## 5. Lista de Verificacion de Fidelidad Visual

Marque con `[X]` si el comportamiento es correcto, o `[ ]` si se detecta alguna anomalia:

- [ ] **Iluminación Global y Luces Dinámicas**: Idénticas al Perfil A, sin zonas oscuras repentinas ni cambios bruscos de exposición.
- [ ] **Renderizado de Sombras**: Sin roturas de cascadas de sombras, sombras flotantes ni geometría de sombras ausente.
- [ ] **Agua y Fluidos (Nivel Esponja)**: Deformación y transparencia del agua correctas, sin mallas rotas ni texturas corrompidas.
- [ ] **Elementos de HUD y UI**: Textos, medidores de vidas y monedas nítidos y sin parpadeos.
- [ ] **Ausencia de Parpadeos (Flickering)**: La geometría estática y dinámica permanece estable cuadro a cuadro.
- [ ] **Estabilidad de Ejecución**: Sesión completada sin crashes, bloqueos de hilos ni `VK_ERROR_DEVICE_LOST`.

---

## 6. Observaciones Cualitativas y Comentarios Adicionales

- **Sensación de fluidez en juego**: `[Comentarios sobre respuesta del control, microtirones, etc.]`
- **Uso de CPU / GPU (Administrador de Tareas / HWInfo)**: `[Observaciones sobre distribución de carga entre los 4 workers]`
- **Comportamiento en pantallas de carga**: `[Tiempo de carga o transición entre niveles]`
- **Logs de error o advertencias relevantes (si existieran)**:
```
[Pegar cualquier mensaje relevante de la consola del emulador]
```
