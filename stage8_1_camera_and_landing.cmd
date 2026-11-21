@echo off
setlocal
cd /d "%~dp0"
set "GAME_DIR=C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app"

set "EMU_EXE=%~dp0_Build\windows\kyty_emulator.exe"
if not exist "%EMU_EXE%" set "EMU_EXE=C:\Users\Recko\Documents\github\kyty\_Build\windows\kyty_emulator.exe"

if not exist "%EMU_EXE%" (
    echo [ERROR] No se encontro el ejecutable en %EMU_EXE%
    pause
    exit /b 1
)

if "%1"=="1" goto run_opt1
if "%1"=="2" goto run_opt2
if "%1"=="3" goto run_opt3
if "%1"=="4" goto run_opt4
if "%1"=="5" goto run_opt5
if "%1"=="6" goto run_opt6
if "%1"=="7" exit /b 0
if "%1"=="exit" exit /b 0
if "%1"=="wide_opt" goto run_opt1
if "%1"=="wide_base" goto run_opt2
if "%1"=="landing_opt" goto run_opt3
if "%1"=="landing_base" goto run_opt4
if "%1"=="landing_shaders" goto run_opt5
if "%1"=="landing_memory" goto run_opt6

:menu
cls
echo =====================================================================
echo    KytyPS5 - Etapa 8.1: TRANSICION DE ATERRIZAJE Y OPTIMIZACIONES
echo =====================================================================
echo.
echo Optimizaciones de Etapa 8.1:
echo   [+] Fast-path EnsureGraphicsPipeline MRU (--ensure-pipeline-cache)
echo   [+] Fast-path Descriptor Binding Cache (--descriptor-cache)
echo   [+] Optimizaciones Etapa 8 (--pipeline-key-cache, --draw-state-dirty-check)
echo   [+] Optimizaciones Etapa 7 (--redundant-bind-elision, --pipeline-cache-mru)
echo   [+] Optimizaciones Etapa 5 (--partial-buffer-refresh, --gpu-submit-cadence)
echo.
echo Controles en Gameplay:
echo   F9  : Iniciar / Detener captura (modo continuo con duration 0)
echo   F10 : Avanzar fase de aterrizaje (WorldEntryPrep -^> DescentStart -^>
echo         ActiveDescent -^> GroundApproach -^> Touchdown -^> EarlyGameplay)
echo.
echo Ejecutable: %EMU_EXE%
echo.
echo Seleccione la prueba a ejecutar:
echo   [1] Vista Amplia: Todas las optimizaciones activas (30 seg con F9)
echo       Salida: stage8_1_wide_optimized.json / .csv / _frames.csv
echo.
echo   [2] Vista Amplia: Control / Baseline sin Etapa 8 / 8.1 (30 seg con F9)
echo       Salida: stage8_1_wide_baseline.json / .csv / _frames.csv
echo.
echo   [3] Aterrizaje: Modo Continuo con optimizaciones activas (F9 inicio/fin, F10 fases)
echo       Salida: stage8_1_landing_optimized.json / .csv / _frames.csv
echo.
echo   [4] Aterrizaje: Modo Continuo Baseline (F9 inicio/fin, F10 fases)
echo       Salida: stage8_1_landing_baseline.json / .csv / _frames.csv
echo.
echo   [5] Aterrizaje: Analisis detallado de shaders y pipelines (F9 inicio/fin, F10)
echo       Salida: stage8_1_landing_shaders.json / .csv / _frames.csv
echo.
echo   [6] Aterrizaje: Analisis de memoria y sincronizacion (ExtBuf 256 MiB, F9/F10)
echo       Salida: stage8_1_landing_memory.json / .csv / _frames.csv
echo.
echo   [7] Salir
echo.
set /p "CHOICE=Elija una opcion [1-7]: "

if "%CHOICE%"=="1" goto run_opt1
if "%CHOICE%"=="2" goto run_opt2
if "%CHOICE%"=="3" goto run_opt3
if "%CHOICE%"=="4" goto run_opt4
if "%CHOICE%"=="5" goto run_opt5
if "%CHOICE%"=="6" goto run_opt6
if "%CHOICE%"=="7" exit /b 0
goto menu

:run_opt1
echo.
echo =====================================================================
echo    OPCION 1: VISTA AMPLIA - OPTIMIZACIONES ACTIVAS (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Se abrira el emulador en modo WARMUP.
echo 2. Vaya a Aztec Sky, deje al personaje quieto mirando al horizonte.
echo 3. Espere 20-30 segundos para estabilizar shaders y streaming.
echo 4. Presione F9 para iniciar la captura fija de 30 segundos.
echo 5. Al finalizar se exportara: stage8_1_wide_optimized.json/.csv/_frames.csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_1_wide_optimized.json"
goto finish

:run_opt2
echo.
echo =====================================================================
echo    OPCION 2: VISTA AMPLIA - CONTROL / BASELINE (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Mismo punto de vista amplio que la Opcion 1.
echo 2. Corre sin --ensure-pipeline-cache, --descriptor-cache, --pipeline-key-cache ni --draw-state-dirty-check.
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage8_1_wide_baseline.json/.csv/_frames.csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --no-pipeline-key-cache --no-draw-state-dirty-check --no-ensure-pipeline-cache --no-descriptor-cache --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_1_wide_baseline.json"
goto finish

:run_opt3
echo.
echo =====================================================================
echo    OPCION 3: ATERRIZAJE - MODO CONTINUO CON OPTIMIZACIONES ACTIVAS
echo =====================================================================
echo PROTOCOLO DE TRANSICION DE ATERRIZAJE:
echo 1. En el mapa general o pantalla previa a entrar al mundo:
echo 2. Presione F9 JUSTO ANTES de que la nave inicie el descenso.
echo 3. Durante el descenso, presione F10 para avanzar las fases:
echo      - Fase 1: WorldEntryPrep (carga inicial)
echo      - Fase 2: DescentStart (entrada a atmosfera)
echo      - Fase 3: ActiveDescent (descenso rapido)
echo      - Fase 4: GroundApproach (aproximacion final a superficie)
echo      - Fase 5: Touchdown (impacto y salida de la nave)
echo      - Fase 6: EarlyGameplay (primeros segundos en tierra)
echo 4. Al tomar control completo del personaje, presione F9 para DETENER la captura.
echo 5. Se exportara: stage8_1_landing_optimized.json/.csv/_frames.csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache --audit-faults --audit-manual-trigger --audit-capture-duration 0 --audit-export "stage8_1_landing_optimized.json"
goto finish

:run_opt4
echo.
echo =====================================================================
echo    OPCION 4: ATERRIZAJE - MODO CONTINUO BASELINE
echo =====================================================================
echo PROTOCOLO DE TRANSICION DE ATERRIZAJE:
echo 1. Mismo recorrido de entrada de la nave y aterrizaje que la Opcion 3.
echo 2. Corre sin --ensure-pipeline-cache, --descriptor-cache, --pipeline-key-cache ni --draw-state-dirty-check.
echo 3. Presione F9 antes del descenso, use F10 entre fases, y F9 al aterrizar.
echo 4. Se exportara: stage8_1_landing_baseline.json/.csv/_frames.csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --no-pipeline-key-cache --no-draw-state-dirty-check --no-ensure-pipeline-cache --no-descriptor-cache --audit-faults --audit-manual-trigger --audit-capture-duration 0 --audit-export "stage8_1_landing_baseline.json"
goto finish

:run_opt5
echo.
echo =====================================================================
echo    OPCION 5: ATERRIZAJE - ANALISIS DETALLADO DE SHADERS Y PIPELINES
echo =====================================================================
echo PROTOCOLO:
echo 1. Mismo flujo de aterrizaje continuo (F9 inicio, F10 fases, F9 parada).
echo 2. Mide la creacion de pipelines y compilacion asincrona durante el descenso.
echo 3. Se exportara: stage8_1_landing_shaders.json/.csv/_frames.csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache --audit-faults --audit-manual-trigger --audit-capture-duration 0 --audit-export "stage8_1_landing_shaders.json"
goto finish

:run_opt6
echo.
echo =====================================================================
echo    OPCION 6: ATERRIZAJE - ANALISIS DE MEMORIA Y SINCRONIZACION
echo =====================================================================
echo PROTOCOLO:
echo 1. Mismo flujo de aterrizaje continuo (F9 inicio, F10 fases, F9 parada).
echo 2. Habilita --extended-stream-buffer (256 MiB) para evaluar la absorcion
echo    de subidas masivas de vertices y buffers durante el aterrizaje.
echo 3. Se exportara: stage8_1_landing_memory.json/.csv/_frames.csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache --extended-stream-buffer --audit-faults --audit-manual-trigger --audit-capture-duration 0 --audit-export "stage8_1_landing_memory.json"
goto finish

:finish
echo.
echo =====================================================================
echo Captura completada exitosamente.
echo Se han exportado los archivos JSON, CSV y la serie temporal _frames.csv
echo en el directorio raiz del emulador.
echo =====================================================================
echo.
pause
goto menu
