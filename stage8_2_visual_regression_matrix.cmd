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
if "%1"=="7" goto run_opt7
if "%1"=="8" goto run_opt8
if "%1"=="9" exit /b 0
if "%1"=="exit" exit /b 0

:menu
cls
echo =====================================================================
echo    KytyPS5 - Etapa 8.2: MATRIZ DE AISLAMIENTO DE REGRESIONES GRAFICAS
echo =====================================================================
echo.
echo Sintomas reportados en Sky Garden:
echo   - Sombras de objetos inexistentes / sombras provenientes del cielo.
echo   - Destellos y parpadeos de iluminacion excesiva.
echo   - Congelamientos al rotar la camara hacia el horizonte/agua.
echo.
echo Ejecutable: %EMU_EXE%
echo.
echo Seleccione la prueba A/B para aislar la causa:
echo.
echo   [1] TODAS ACTIVAS (Estado actual de Etapa 8.1 - Reproduce regresiones)
echo       Flags: --pipeline-key-cache --draw-state-dirty-check
echo              --ensure-pipeline-cache --descriptor-cache
echo.
echo   [2] CONTROL BASELINE LIMPIO (Sin Etapa 8 ni 8.1 - Referencia limpia)
echo       Flags: --no-pipeline-key-cache --no-draw-state-dirty-check
echo              --no-ensure-pipeline-cache --no-descriptor-cache
echo.
echo   [3] AISLAR DESCRIPTOR CACHE (Desactiva solo --descriptor-cache)
echo       Flags: --pipeline-key-cache --draw-state-dirty-check
echo              --ensure-pipeline-cache --no-descriptor-cache
echo.
echo   [4] AISLAR DRAW STATE DIRTY CHECK (Desactiva solo --draw-state-dirty-check)
echo       Flags: --pipeline-key-cache --no-draw-state-dirty-check
echo              --ensure-pipeline-cache --descriptor-cache
echo.
echo   [5] AISLAR ETAPA 8.1 COMPLETA (Desactiva optimizaciones de Etapa 8.1)
echo       Flags: --pipeline-key-cache --draw-state-dirty-check
echo              --no-ensure-pipeline-cache --no-descriptor-cache
echo.
echo   [6] AISLAR ETAPA 8 COMPLETA (Desactiva optimizaciones de Etapa 8)
echo       Flags: --no-pipeline-key-cache --no-draw-state-dirty-check
echo              --ensure-pipeline-cache --descriptor-cache
echo.
echo   [7] TEST COMBINADO (Sin Dirty Check ni Descriptor Cache)
echo       Flags: --pipeline-key-cache --no-draw-state-dirty-check
echo              --ensure-pipeline-cache --no-descriptor-cache
echo.
echo   [8] AISLAR PIPELINE KEY CACHE (Desactiva solo --pipeline-key-cache)
echo       Flags: --no-pipeline-key-cache --draw-state-dirty-check
echo              --ensure-pipeline-cache --descriptor-cache
echo.
echo   [9] Salir
echo =====================================================================
set /p test_opt="Elija una opcion [1-9]: "

if "%test_opt%"=="1" goto run_opt1
if "%test_opt%"=="2" goto run_opt2
if "%test_opt%"=="3" goto run_opt3
if "%test_opt%"=="4" goto run_opt4
if "%test_opt%"=="5" goto run_opt5
if "%test_opt%"=="6" goto run_opt6
if "%test_opt%"=="7" goto run_opt7
if "%test_opt%"=="8" goto run_opt8
if "%test_opt%"=="9" exit /b 0
goto menu

:run_opt1
cls
echo [EJECUTANDO OP 1] Todas las optimizaciones activas (Estado actual Etapa 8.1)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache
pause
goto menu

:run_opt2
cls
echo [EJECUTANDO OP 2] Control Baseline Limpio (Sin Etapa 8 ni 8.1)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --no-pipeline-key-cache --no-draw-state-dirty-check --no-ensure-pipeline-cache --no-descriptor-cache
pause
goto menu

:run_opt3
cls
echo [EJECUTANDO OP 3] Aislando Descriptor Cache (--no-descriptor-cache)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --ensure-pipeline-cache --no-descriptor-cache
pause
goto menu

:run_opt4
cls
echo [EJECUTANDO OP 4] Aislando Draw State Dirty Check (--no-draw-state-dirty-check)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --no-draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache
pause
goto menu

:run_opt5
cls
echo [EJECUTANDO OP 5] Aislando Etapa 8.1 (--no-ensure-pipeline-cache --no-descriptor-cache)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --draw-state-dirty-check --no-ensure-pipeline-cache --no-descriptor-cache
pause
goto menu

:run_opt6
cls
echo [EJECUTANDO OP 6] Aislando Etapa 8 (--no-pipeline-key-cache --no-draw-state-dirty-check)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --no-pipeline-key-cache --no-draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache
pause
goto menu

:run_opt7
cls
echo [EJECUTANDO OP 7] Test Combinado Seguro (--no-draw-state-dirty-check --no-descriptor-cache)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --pipeline-key-cache --no-draw-state-dirty-check --ensure-pipeline-cache --no-descriptor-cache
pause
goto menu

:run_opt8
cls
echo [EJECUTANDO OP 8] Aislando Pipeline Key Cache (--no-pipeline-key-cache)...
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --redundant-bind-elision --pipeline-cache-mru --no-pipeline-key-cache --draw-state-dirty-check --ensure-pipeline-cache --descriptor-cache
pause
goto menu
