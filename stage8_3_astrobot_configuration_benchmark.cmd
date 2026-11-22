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
if "%1"=="9" goto run_opt9
if "%1"=="10" goto run_opt10
if "%1"=="11" goto run_opt11
if "%1"=="12" exit /b 0
if "%1"=="exit" exit /b 0

:menu
cls
echo =====================================================================
echo    KytyPS5 - Etapa 8.3: BENCHMARK DE PRESETS Y CONFIGURACION ASTRO BOT
echo =====================================================================
echo.
echo Ejecutable: %EMU_EXE%
echo Juego:      Astro Bot (PPSA21567)
echo Escenario:  Sky Garden (1280x720)
echo.
echo Presets y Opciones Disponibles:
echo   [1]  Configuracion actual del ejecutable (Flags seguros activados)
echo   [2]  Configuracion equivalente desde Launcher (Valores por defecto GUI)
echo   [3]  Preset Astro Bot Optimizado (Maximo rendimiento seguro)
echo   [4]  Preset Conservador (Baseline de control estricto)
echo   [5]  Shader Cache Fria (--no-shader-cache para medir sobrecarga)
echo   [6]  Shader Cache Caliente (--shader-cache precompilado)
echo   [7]  Configuracion Grafica Original (Modo A: Calidad 100%% original)
echo   [8]  Configuracion con Parches Explicitos (Modo B: _Patches/PPSA21567.json)
echo   [9]  Comparacion de Buffers (64 MiB estandar vs 256 MiB extendido)
echo   [10] Comparacion de Flags de Renderizado (Menu de diagnostico A/B)
echo   [11] Exportar e inspeccionar resultados de capturas
echo   [12] Salir
echo =====================================================================
set /p "choice=Seleccione una opcion [1-12]: "

if "%choice%"=="1" goto run_opt1
if "%choice%"=="2" goto run_opt2
if "%choice%"=="3" goto run_opt3
if "%choice%"=="4" goto run_opt4
if "%choice%"=="5" goto run_opt5
if "%choice%"=="6" goto run_opt6
if "%choice%"=="7" goto run_opt7
if "%choice%"=="8" goto run_opt8
if "%choice%"=="9" goto run_opt9
if "%choice%"=="10" goto run_opt10
if "%choice%"=="11" goto run_opt11
if "%choice%"=="12" exit /b 0
goto menu

:run_opt1
cls
echo [EJECUCION] Opcion 1: Configuracion actual del ejecutable (Flags seguros)
echo Flags: --descriptor-cache --ensure-pipeline-cache --pipeline-key-cache
echo        --pipeline-cache-mru --redundant-bind-elision --no-draw-state-dirty-check
echo.
echo Presione F9 en Sky Garden para capturar 30 segundos de telemetria.
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --redundant-bind-elision ^
    --pipeline-cache-mru ^
    --pipeline-key-cache ^
    --no-draw-state-dirty-check ^
    --ensure-pipeline-cache ^
    --descriptor-cache ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_3_current_opt.json"
echo.
pause
goto menu

:run_opt2
cls
echo [EJECUCION] Opcion 2: Configuracion equivalente desde Launcher (GUI Defaults)
echo Reproduce exactamente los parametros que genera kyty_launcher.exe por defecto.
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --vblank-frequency 60 ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_3_launcher_equiv.json"
echo.
pause
goto menu

:run_opt3
cls
echo [EJECUCION] Opcion 3: Preset Astro Bot Optimizado
echo Activa todas las optimizaciones de Etapas 5 a 8.2 validadas sin regresion.
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --redundant-bind-elision ^
    --pipeline-cache-mru ^
    --pipeline-key-cache ^
    --ensure-pipeline-cache ^
    --descriptor-cache ^
    --batch-hot-readbacks ^
    --partial-buffer-refresh ^
    --gpu-submit-cadence ^
    --gpu-submit-cadence-interval-us 1000 ^
    --no-draw-state-dirty-check ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_3_astrobot_preset_opt.json"
echo.
pause
goto menu

:run_opt4
cls
echo [EJECUCION] Opcion 4: Preset Conservador (Control / Baseline estricto)
echo Desactiva todas las caches avanzadas para pruebas A/B.
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --no-redundant-bind-elision ^
    --no-pipeline-cache-mru ^
    --no-pipeline-key-cache ^
    --no-ensure-pipeline-cache ^
    --no-descriptor-cache ^
    --no-draw-state-dirty-check ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_3_preset_conservative.json"
echo.
pause
goto menu

:run_opt5
cls
echo [EJECUCION] Opcion 5: Shader Cache Fria (--no-shader-cache)
echo Mide el impacto y los tirones provocados por compilacion en caliente.
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --no-shader-cache ^
    --no-draw-state-dirty-check ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_3_shader_cache_cold.json"
echo.
pause
goto menu

:run_opt6
cls
echo [EJECUCION] Opcion 6: Shader Cache Caliente (--shader-cache)
echo Ejecucion limpia con shaders precargados en _PipelineCache.
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --no-draw-state-dirty-check ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_3_shader_cache_hot.json"
echo.
pause
goto menu

:run_opt7
cls
echo [EJECUCION] Opcion 7: Configuracion Grafica Original (Modo A)
echo Calidad visual original de PS5 sin parches (Global Illumination activa).
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --descriptor-cache ^
    --ensure-pipeline-cache ^
    --no-draw-state-dirty-check ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_3_original_graphics.json"
echo.
pause
goto menu

:run_opt8
cls
echo [EJECUCION] Opcion 8: Configuracion con Parches Explicitos (Modo B)
if exist "_Patches\PPSA21567.json" (
    echo [INFO] Parche detectado en _Patches\PPSA21567.json
    "%EMU_EXE%" ^
        --game "%GAME_DIR%" ^
        --game-patch "_Patches\PPSA21567.json" ^
        --screen-width 1280 --screen-height 720 ^
        --present-mode Mailbox ^
        --shader-cache ^
        --descriptor-cache ^
        --ensure-pipeline-cache ^
        --no-draw-state-dirty-check ^
        --audit-faults ^
        --audit-manual-trigger ^
        --audit-capture-duration 30 ^
        --audit-export "stage8_3_patched_graphics.json"
) else (
    echo [AVISO] No se encontro _Patches\PPSA21567.json.
    echo Si dispone de un parche GoldHEN para Astro Bot, coloquelo en _Patches\PPSA21567.json
    echo Ejecutando en modo normal como control.
    "%EMU_EXE%" ^
        --game "%GAME_DIR%" ^
        --screen-width 1280 --screen-height 720 ^
        --present-mode Mailbox ^
        --shader-cache ^
        --descriptor-cache ^
        --ensure-pipeline-cache ^
        --no-draw-state-dirty-check ^
        --audit-faults ^
        --audit-manual-trigger ^
        --audit-capture-duration 30 ^
        --audit-export "stage8_3_patched_graphics_control.json"
)
echo.
pause
goto menu

:run_opt9
cls
echo [EJECUCION] Opcion 9: Comparacion de Buffers
echo [A] Búfer Estandar (64 MiB)
echo [B] Búfer Extendido (--extended-stream-buffer 256 MiB)
echo.
set /p "buf_choice=Seleccione sub-opcion [A/B]: "
if /i "%buf_choice%"=="B" (
    echo Ejecutando con Búfer Extendido de 256 MiB...
    "%EMU_EXE%" ^
        --game "%GAME_DIR%" ^
        --screen-width 1280 --screen-height 720 ^
        --present-mode Mailbox ^
        --extended-stream-buffer ^
        --no-draw-state-dirty-check ^
        --audit-faults ^
        --audit-manual-trigger ^
        --audit-capture-duration 30 ^
        --audit-export "stage8_3_buffer_extended.json"
) else (
    echo Ejecutando con Búfer Estandar de 64 MiB...
    "%EMU_EXE%" ^
        --game "%GAME_DIR%" ^
        --screen-width 1280 --screen-height 720 ^
        --present-mode Mailbox ^
        --no-draw-state-dirty-check ^
        --audit-faults ^
        --audit-manual-trigger ^
        --audit-capture-duration 30 ^
        --audit-export "stage8_3_buffer_standard.json"
)
echo.
pause
goto menu

:run_opt10
cls
echo =====================================================================
echo    KytyPS5 - Comparacion de Flags de Renderizado
echo =====================================================================
echo [1] Sin Descriptor Cache (--no-descriptor-cache)
echo [2] Sin Ensure Pipeline Cache (--no-ensure-pipeline-cache)
echo [3] Sin Pipeline Key Cache (--no-pipeline-key-cache)
echo [4] Sin Pipeline Cache MRU (--no-pipeline-cache-mru)
echo [5] Sin Redundant Bind Elision (--no-redundant-bind-elision)
echo [6] Regresar al menu principal
echo.
set /p "flag_choice=Seleccione flag a aislar [1-6]: "
if "%flag_choice%"=="1" (
    "%EMU_EXE%" --game "%GAME_DIR%" --screen-width 1280 --screen-height 720 --present-mode Mailbox --no-descriptor-cache --no-draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_3_no_descriptor_cache.json"
)
if "%flag_choice%"=="2" (
    "%EMU_EXE%" --game "%GAME_DIR%" --screen-width 1280 --screen-height 720 --present-mode Mailbox --no-ensure-pipeline-cache --no-draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_3_no_ensure_pipeline.json"
)
if "%flag_choice%"=="3" (
    "%EMU_EXE%" --game "%GAME_DIR%" --screen-width 1280 --screen-height 720 --present-mode Mailbox --no-pipeline-key-cache --no-draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_3_no_key_cache.json"
)
if "%flag_choice%"=="4" (
    "%EMU_EXE%" --game "%GAME_DIR%" --screen-width 1280 --screen-height 720 --present-mode Mailbox --no-pipeline-cache-mru --no-draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_3_no_pipeline_mru.json"
)
if "%flag_choice%"=="5" (
    "%EMU_EXE%" --game "%GAME_DIR%" --screen-width 1280 --screen-height 720 --present-mode Mailbox --no-redundant-bind-elision --no-draw-state-dirty-check --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage8_3_no_bind_elision.json"
)
echo.
pause
goto menu

:run_opt11
cls
echo =====================================================================
echo    KytyPS5 - Resumen de Capturas de la Etapa 8.3
echo =====================================================================
dir /b stage8_3_*.json 2>nul
echo.
if exist "scripts\compare_captures.py" (
    python scripts\compare_captures.py stage8_3_current_opt.json stage8_3_launcher_equiv.json 2>nul
)
echo.
pause
goto menu
