@echo off
setlocal enabledelayedexpansion

:: ============================================================================
:: KytyPS5 - Astro Bot (PPSA21567) - Perfil B: PM4 + DrawPrep Experimental
:: Etapa 8.4E - Suite de Pruebas de Rendimiento y Fidelidad Grafica
:: ============================================================================

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.."
set "REPO_ROOT=%CD%"
popd

:: Localizar ejecutable de Kyty
set "EMU_EXE=%REPO_ROOT%\_Build\windows\kyty_emulator.exe"
if not exist "%EMU_EXE%" (
    set "EMU_EXE=%REPO_ROOT%\kyty_emulator.exe"
)

if not exist "%EMU_EXE%" (
    echo [ERROR] No se encontro el ejecutable kyty_emulator.exe.
    echo Por favor compile el proyecto antes de ejecutar las pruebas:
    echo   cmake -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
    echo   ninja -C _Build/windows kyty_emulator
    echo.
    pause
    exit /b 1
)

:: Obtener ruta del juego (argumento 1, variable de entorno o prompt)
set "GAME_PATH=%~1"
if "%GAME_PATH%"=="" (
    if defined ASTROBOT_GAME_PATH (
        set "GAME_PATH=%ASTROBOT_GAME_PATH%"
    )
)

if "%GAME_PATH%"=="" (
    echo.
    echo ============================================================================
    echo  KytyPS5 - Seleccion de Juego para Pruebas A/B
    echo ============================================================================
    echo Ingrese la ruta completa al archivo eboot.bin o al directorio de Astro Bot:
    set /p "GAME_PATH=> "
)

:: Quitar comillas si el usuario las introdujo
set "GAME_PATH=%GAME_PATH:"=%"

if not exist "%GAME_PATH%" (
    echo.
    echo [ERROR] La ruta especificada no existe: "%GAME_PATH%"
    echo Asegurese de indicar un eboot.bin o directorio valido de Astro Bot [PPSA21567].
    echo.
    pause
    exit /b 1
)

:: Crear directorio de logs
if not exist "%REPO_ROOT%\_TestLogs" mkdir "%REPO_ROOT%\_TestLogs"

:: Generar timestamp independiente de locale
set "TSTAMP="
for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss" 2^>nul') do set "TSTAMP=%%I"
if "%TSTAMP%"=="" set "TSTAMP=latest"

set "LOG_FILE=%REPO_ROOT%\_TestLogs\astrobot_profile_b_experimental_%TSTAMP%.json"

:: Obtener commit de Git si esta disponible
set "GIT_REV=desconocido"
for /f "tokens=*" %%g in ('git rev-parse --short HEAD 2^>nul') do set "GIT_REV=%%g"

echo.
echo ============================================================================
echo   KytyPS5 - PERFIL B: PM4 + DRAWPREP EXPERIMENTAL (ETAPA 8.4E)
echo ============================================================================
echo   Commit Git:          %GIT_REV%
echo   Ejecutable:          %EMU_EXE%
echo   Juego / eboot:       %GAME_PATH%
echo   Log de Telemetria:   %LOG_FILE%
echo.
echo   Configuracion de Motor:
echo     [x] Submit Cadence (1.0 ms)
echo     [x] Redundant Bind Elision
echo     [x] Pipeline Cache MRU
echo     [x] Pipeline Key Cache
echo     [x] Ensure Pipeline Cache Fast-Path
echo     [x] Descriptor Cache Fast-Path
echo     [x] Fast Pending Operations (lock-free)
echo     [x] Priority Spin Wait (100 us)
echo     [ ] Draw State Dirty Check (DESACTIVADO - Fidelidad Sky Garden)
echo     [x] CP Sequencer (ACTIVADO - Decodificacion PM4 desacoplada)
echo     [x] DrawPrep Workers (ACTIVADO - 4 hilos de preparacion paralela)
echo     [x] Cero duplicacion (100%% resultados reutilizados en Thread_Gpu)
echo ============================================================================
echo.
echo Iniciando emulador... Presione Ctrl+C o cierre la ventana del juego para finalizar.
echo.

"%EMU_EXE%" --game "%GAME_PATH%" ^
  --gpu-submit-cadence ^
  --redundant-bind-elision ^
  --pipeline-cache-mru ^
  --pipeline-key-cache ^
  --ensure-pipeline-cache ^
  --descriptor-cache ^
  --fast-pending-ops ^
  --priority-spin-wait ^
  --no-draw-state-dirty-check ^
  --cp-sequencer ^
  --draw-prep ^
  --draw-prep-workers 4 ^
  --audit-faults ^
  --audit-export "%LOG_FILE%"

set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo ============================================================================
if exist "%LOG_FILE%" (
    echo [OK] Sesion finalizada con codigo %EXIT_CODE%.
    echo Telemetria exportada correctamente a:
    echo   %LOG_FILE%
) else (
    echo [AVISO] Sesion finalizada con codigo %EXIT_CODE%. No se genero archivo de exportacion.
)
echo ============================================================================
echo.
exit /b %EXIT_CODE%
