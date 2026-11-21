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

if "%1"=="wide" goto run_wide
if "%1"=="ground" goto run_ground
if "%1"=="wide_baseline" goto run_wide_baseline
if "%1"=="ground_extbuf" goto run_ground_extbuf

:menu
cls
echo =====================================================================
echo    KytyPS5 - Etapa 7: BENCHMARK Y VALIDACION A/B EN ASTRO BOT
echo =====================================================================
echo.
echo Optimizaciones de Etapa 7 activas por defecto:
echo   [+] Elision de Vulkan Binds Redundantes (Pipeline, Vertex, Index)
echo   [+] PipelineCache Fast-Path MRU (Bypass de XXH3 hash y unordered_map)
echo   [+] Base Etapa 5: Cadence 1.0ms, Partial Buffer Refresh, Shader Cache
echo.
echo Ejecutable: %EMU_EXE%
echo Duracion de captura: 30 segundos (activacion manual con tecla F9)
echo.
echo Seleccione la prueba a ejecutar:
echo   [1] Captura A (Etapa 7): Vista Amplia (Mapa Completo)
echo       Salida: stage7_camera_wide.json / .csv
echo.
echo   [2] Captura B (Etapa 7): Vista Reducida (Suelo)
echo       Salida: stage7_camera_ground.json / .csv
echo.
echo   [3] Comparacion Control: Vista Amplia SIN optimizaciones de Etapa 7
echo       (--no-redundant-bind-elision --no-pipeline-cache-mru)
echo       Salida: stage7_camera_wide_baseline.json / .csv
echo.
echo   [4] Validacion Hitches: Vista al Suelo con Extended Stream Buffer
echo       (--extended-stream-buffer para mitigar StreamBufferWait de 560ms)
echo       Salida: stage7_camera_ground_extbuf.json / .csv
echo.
echo   [5] Salir
echo.
set /p "CHOICE=Elija una opcion [1, 2, 3, 4, 5]: "

if "%CHOICE%"=="1" goto run_wide
if "%CHOICE%"=="2" goto run_ground
if "%CHOICE%"=="3" goto run_wide_baseline
if "%CHOICE%"=="4" goto run_ground_extbuf
if "%CHOICE%"=="5" exit /b 0
goto menu

:run_wide
echo.
echo =====================================================================
echo    INICIANDO CAPTURA A: VISTA AMPLIA (ETAPA 7 - 30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Se abrira el emulador en modo WARMUP.
echo 2. Vaya a Aztec Sky, deje al personaje quieto mirando al horizonte.
echo 3. Espere 20-30 segundos para estabilizar shaders y streaming.
echo 4. Presione F9 para iniciar la captura de 30 segundos.
echo 5. Al finalizar se exportara: stage7_camera_wide.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage7_camera_wide.json"
goto finish

:run_ground
echo.
echo =====================================================================
echo    INICIANDO CAPTURA B: VISTA REDUCIDA / SUELO (ETAPA 7 - 30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Se abrira el emulador en modo WARMUP.
echo 2. En la misma posicion de Aztec Sky, incline la camara hacia el suelo.
echo 3. Espere 20-30 segundos para estabilizar shaders y streaming.
echo 4. Presione F9 para iniciar la captura de 30 segundos.
echo 5. Al finalizar se exportara: stage7_camera_ground.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage7_camera_ground.json"
goto finish

:run_wide_baseline
echo.
echo =====================================================================
echo    INICIANDO CONTROL: VISTA AMPLIA SIN ETAPA 7 (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Mismo punto de vista amplio que la Captura A.
echo 2. Corre con --no-redundant-bind-elision y --no-pipeline-cache-mru.
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage7_camera_wide_baseline.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --no-redundant-bind-elision --no-pipeline-cache-mru --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage7_camera_wide_baseline.json"
goto finish

:run_ground_extbuf
echo.
echo =====================================================================
echo    INICIANDO VALIDACION: VISTA SUELO CON EXTENDED STREAM BUFFER (30S)
echo =====================================================================
echo PROTOCOLO:
echo 1. Mismo punto de vista al suelo que la Captura B.
echo 2. Corre con --extended-stream-buffer (256 MiB vs 64 MiB).
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage7_camera_ground_extbuf.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --extended-stream-buffer --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage7_camera_ground_extbuf.json"
goto finish

:finish
echo.
echo =====================================================================
echo    Prueba finalizada. Verifique los archivos generados.
echo =====================================================================
pause
