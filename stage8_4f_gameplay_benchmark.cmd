@echo off
setlocal
cd /d "%~dp0"

:: -----------------------------------------------------------------------------
:: KytyPS5 - Protocolo de Validacion de Gameplay Etapa 8.4F (Sky Garden)
:: Perfiles:
::   [A] Baseline:   CpSequencer OFF / DrawPrep OFF
::   [B] Sequencer:  CpSequencer ON  / DrawPrep OFF
::   [C] DrawPrep:   CpSequencer ON  / DrawPrep ON (4 workers)
:: -----------------------------------------------------------------------------

set "GAME_DIR=F:\ps5\PPSA21567\PPSA21567-app"
if not exist "%GAME_DIR%" set "GAME_DIR=C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app"
if not exist "%GAME_DIR%" set "GAME_DIR=%~dp0PPSA21567-app"

set "EMU_EXE=%~dp0_Build\windows\install\kyty_emulator.exe"
if not exist "%EMU_EXE%" set "EMU_EXE=%~dp0_Build\windows\kyty_emulator.exe"
if not exist "%EMU_EXE%" set "EMU_EXE=C:\Users\Recko\Documents\github\kyty\_Build\windows\kyty_emulator.exe"

if not exist "%EMU_EXE%" (
    echo [ERROR] No se encontro el ejecutable kyty_emulator.exe.
    echo Asegurese de haber compilado el proyecto en Release.
    pause
    exit /b 1
)

if not exist "%GAME_DIR%" (
    echo [ERROR] No se encontro la carpeta de Astro Bot en %GAME_DIR%.
    pause
    exit /b 1
)

if "%1"=="1" goto run_profile_a
if "%1"=="2" goto run_profile_b
if "%1"=="3" goto run_profile_c
if "%1"=="compare" goto run_compare

:menu
cls
echo =====================================================================
echo    KytyPS5 - Etapa 8.4F: VALIDACION DE GAMEPLAY EN ASTRO BOT
echo =====================================================================
echo.
echo Ejecutable: %EMU_EXE%
echo Juego:      %GAME_DIR%
echo Escenario:  Sky Garden (Camara Amplia - aprox. 5.000 draws/frame)
echo Resolucion: 1280x720 (Mailbox)
echo.
echo PROTOCOLO DE MEDICION:
echo   1. Inicie el perfil deseado desde este menu.
echo   2. El emulador iniciara en modo WARMUP (esperando hotkey).
echo   3. Cargue la partida guardada en Sky Garden.
echo   4. Situe al personaje en el mirador mirando al horizonte.
echo   5. Espere 10-15 segundos para estabilizar shaders y streaming.
echo   6. Presione F9 para capturar 30 segundos de telemetria limpia.
echo   7. Al finalizar los 30 s, se exportaran automaticamente los reportes.
echo.
echo =====================================================================
echo Perfiles de Concurrencia a Evaluar:
echo.
echo   [1] Perfil A: Baseline Control (CpSequencer OFF / DrawPrep OFF)
echo       Salida: stage8_4f_profile_a_baseline.json / .csv
echo.
echo   [2] Perfil B: CpSequencer Solo (CpSequencer ON / DrawPrep OFF)
echo       Salida: stage8_4f_profile_b_cpseq.json / .csv
echo.
echo   [3] Perfil C: CpSequencer + DrawPrep (ON / 4 Workers)
echo       Salida: stage8_4f_profile_c_drawprep.json / .csv
echo.
echo   [4] Comparar telemetria de los 3 perfiles exportados
echo.
echo   [5] Salir
echo =====================================================================
set /p "choice=Seleccione una opcion [1-5]: "

if "%choice%"=="1" goto run_profile_a
if "%choice%"=="2" goto run_profile_b
if "%choice%"=="3" goto run_profile_c
if "%choice%"=="4" goto run_compare
if "%choice%"=="5" exit /b 0
goto menu

:run_profile_a
cls
echo =====================================================================
echo [EJECUCION] PERFIL A: BASELINE CONTROL
echo Flags: --no-cp-sequencer --no-draw-prep
echo =====================================================================
echo.
echo Iniciando emulador... Presione F9 en Sky Garden (mirador amplio).
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --audio-compile-hold ^
    --perf-overlay ^
    --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 ^
    --partial-buffer-refresh ^
    --redundant-bind-elision ^
    --pipeline-cache-mru ^
    --pipeline-key-cache ^
    --no-draw-state-dirty-check ^
    --ensure-pipeline-cache ^
    --descriptor-cache ^
    --fast-pending-ops ^
    --priority-spin-wait ^
    --no-cp-sequencer ^
    --no-draw-prep ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_4f_profile_a_baseline.json"
echo.
echo Captura del Perfil A finalizada.
pause
goto menu

:run_profile_b
cls
echo =====================================================================
echo [EJECUCION] PERFIL B: CPSEQUENCER SOLO
echo Flags: --cp-sequencer --no-draw-prep
echo =====================================================================
echo.
echo Iniciando emulador... Presione F9 en Sky Garden (mirador amplio).
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --audio-compile-hold ^
    --perf-overlay ^
    --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 ^
    --partial-buffer-refresh ^
    --redundant-bind-elision ^
    --pipeline-cache-mru ^
    --pipeline-key-cache ^
    --no-draw-state-dirty-check ^
    --ensure-pipeline-cache ^
    --descriptor-cache ^
    --fast-pending-ops ^
    --priority-spin-wait ^
    --cp-sequencer ^
    --no-draw-prep ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_4f_profile_b_cpseq.json"
echo.
echo Captura del Perfil B finalizada.
pause
goto menu

:run_profile_c
cls
echo =====================================================================
echo [EJECUCION] PERFIL C: CPSEQUENCER + DRAWPREP (4 WORKERS)
echo Flags: --cp-sequencer --draw-prep --draw-prep-workers 4
echo =====================================================================
echo.
echo Iniciando emulador... Presione F9 en Sky Garden (mirador amplio).
echo.
"%EMU_EXE%" ^
    --game "%GAME_DIR%" ^
    --screen-width 1280 --screen-height 720 ^
    --present-mode Mailbox ^
    --shader-cache ^
    --audio-compile-hold ^
    --perf-overlay ^
    --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 ^
    --partial-buffer-refresh ^
    --redundant-bind-elision ^
    --pipeline-cache-mru ^
    --pipeline-key-cache ^
    --no-draw-state-dirty-check ^
    --ensure-pipeline-cache ^
    --descriptor-cache ^
    --fast-pending-ops ^
    --priority-spin-wait ^
    --cp-sequencer ^
    --draw-prep --draw-prep-workers 4 ^
    --audit-faults ^
    --audit-manual-trigger ^
    --audit-capture-duration 30 ^
    --audit-export "stage8_4f_profile_c_drawprep.json"
echo.
echo Captura del Perfil C finalizada.
pause
goto menu

:run_compare
cls
echo =====================================================================
echo    COMPARACION DE TELEMETRIA ETAPA 8.4F (JSON EXPORTADOS)
echo =====================================================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$files = @('stage8_4f_profile_a_baseline.json', 'stage8_4f_profile_b_cpseq.json', 'stage8_4f_profile_c_drawprep.json'); " ^
    "foreach ($f in $files) { " ^
    "  if (Test-Path $f) { " ^
    "    $j = Get-Content $f -Raw | ConvertFrom-Json; " ^
    "    [PSCustomObject]@{ " ^
    "      Archivo = $f; " ^
    "      Frames = $j.frametime_metrics_ms.total_frames; " ^
    "      FPS_Promedio = [math]::Round($j.frametime_metrics_ms.effective_fps, 2); " ^
    "      Frametime_P50_ms = [math]::Round($j.frametime_metrics_ms.p50_frame_ms, 2); " ^
    "      Frametime_P95_ms = [math]::Round($j.frametime_metrics_ms.p95_frame_ms, 2); " ^
    "      Draws_Frame = [math]::Round($j.draw_statistics.draws_per_frame, 1); " ^
    "      GpuCpuTime_ms = [math]::Round($j.cpu_gpu_separation.gpu_thread_cpu_time_ms, 1); " ^
    "      Submits_Sec = [math]::Round($j.vulkan_submits.submits_per_second, 1); " ^
    "    } | Format-List; " ^
    "  } else { Write-Host \"[AVISO] Archivo no encontrado: $f\" -ForegroundColor Yellow; } " ^
    "}"
echo.
pause
goto menu
