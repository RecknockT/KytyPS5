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
if "%1"=="wide" goto run_opt1
if "%1"=="wide_baseline" goto run_opt2
if "%1"=="ground_normal" goto run_opt3
if "%1"=="ground_extbuf" goto run_opt4
if "%1"=="wide_stages" goto run_opt5
if "%1"=="ground_hitches" goto run_opt6

:menu
cls
echo =====================================================================
echo    KytyPS5 - Etapa 8: DIAGNOSTICO TEMPORAL, HITCHE Y OPTIMIZACION
echo =====================================================================
echo.
echo Optimizaciones de Etapa 8:
echo   [+] Cache de Pipeline Keys (--pipeline-key-cache)
echo   [+] Verificacion de Draw State Dirty (--draw-state-dirty-check)
echo   [+] Instrumentacion de 13 Etapas de Pipeline y Buffer Circular de Trazas
echo   [+] Auditoria y Tracking de StreamBuffer (Normal 64 MiB vs Ext 256 MiB)
echo.
echo Ejecutable: %EMU_EXE%
echo Duracion de captura: 30 segundos (activacion manual con tecla F9)
echo.
echo Seleccione la prueba a ejecutar:
echo   [1] Vista Amplia: Todas las optimizaciones de Etapa 8 activas
echo       (--pipeline-key-cache --draw-state-dirty-check)
echo       Salida: stage8_camera_wide.json / .csv
echo.
echo   [2] Vista Amplia: Control / Baseline sin optimizaciones de Etapa 8
echo       (--no-pipeline-key-cache --no-draw-state-dirty-check)
echo       Salida: stage8_camera_wide_baseline.json / .csv
echo.
echo   [3] Vista al Suelo: StreamBuffer normal de 64 MiB
echo       Salida: stage8_camera_ground_normal.json / .csv
echo.
echo   [4] Vista al Suelo: StreamBuffer extendido de 256 MiB
echo       (--extended-stream-buffer)
echo       Salida: stage8_camera_ground_extbuf.json / .csv
echo.
echo   [5] Vista Amplia: Captura detallada de etapas del pipeline (F9)
echo       Salida: stage8_camera_wide_stages.json / .csv
echo.
echo   [6] Vista al Suelo: Captura detallada de hitches y correlacion de bloqueos (F9)
echo       Salida: stage8_camera_ground_hitches.json / .csv
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
echo    OPCION 1: VISTA AMPLIA CON OPTIMIZACIONES ETAPA 8 (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Se abrira el emulador en modo WARMUP.
echo 2. Vaya a Aztec Sky, deje al personaje quieto mirando al horizonte.
echo 3. Espere 20-30 segundos para estabilizar shaders y streaming.
echo 4. Presione F9 para iniciar la captura de 30 segundos.
echo 5. Al finalizar se exportara: stage8_camera_wide.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_camera_wide.json"
goto finish

:run_opt2
echo.
echo =====================================================================
echo    OPCION 2: VISTA AMPLIA CONTROL / BASELINE SIN ETAPA 8 (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Mismo punto de vista amplio que la Opcion 1.
echo 2. Corre con --no-pipeline-key-cache y --no-draw-state-dirty-check.
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage8_camera_wide_baseline.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --no-pipeline-key-cache --no-draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_camera_wide_baseline.json"
goto finish

:run_opt3
echo.
echo =====================================================================
echo    OPCION 3: VISTA AL SUELO - STREAMBUFFER NORMAL 64 MIB (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. En Aztec Sky, incline la camara completamente hacia el suelo.
echo 2. Espere 20-30 segundos de estabilizacion.
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage8_camera_ground_normal.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_camera_ground_normal.json"
goto finish

:run_opt4
echo.
echo =====================================================================
echo    OPCION 4: VISTA AL SUELO - STREAMBUFFER EXTENDIDO 256 MIB (30 SEG)
echo =====================================================================
echo PROTOCOLO:
echo 1. En Aztec Sky, incline la camara completamente hacia el suelo.
echo 2. Corre con --extended-stream-buffer activo.
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage8_camera_ground_extbuf.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --extended-stream-buffer --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_camera_ground_extbuf.json"
goto finish

:run_opt5
echo.
echo =====================================================================
echo    OPCION 5: VISTA AMPLIA - DESGLOSE DETALLADO DE PIPELINE (30 SEG)
echo =====================================================================
echo PROTOCOLO:
echo 1. Vista amplia mirando al horizonte en Aztec Sky.
echo 2. Captura detallada de las 13 etapas del pipeline de renderizado.
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage8_camera_wide_stages.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_camera_wide_stages.json"
goto finish

:run_opt6
echo.
echo =====================================================================
echo    OPCION 6: VISTA AL SUELO - CORRELACION DE HITCHES (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO:
echo 1. Vista al suelo con movimientos para provocar hitches.
echo 2. Registro circular de trazas para correlacion exacta de causas dominantes.
echo 3. Presione F9 para iniciar la captura de 30 segundos.
echo 4. Al finalizar se exportara: stage8_camera_ground_hitches.json y .csv
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_camera_ground_hitches.json"
goto finish

:finish
echo.
echo =====================================================================
echo Captura completada. Los archivos JSON y CSV correspondientes han sido
echo generados en el directorio raiz del emulador.
echo =====================================================================
echo.
pause
goto menu
