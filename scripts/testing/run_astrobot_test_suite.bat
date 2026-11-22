@echo off
setlocal enabledelayedexpansion

:: ============================================================================
:: KytyPS5 - Suite Interactiva de Pruebas A/B para Astro Bot (PPSA21567)
:: Etapa 8.4E - Baseline Estable vs PM4 + DrawPrep Experimental
:: ============================================================================

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.."
set "REPO_ROOT=%CD%"
popd

set "EMU_EXE=%REPO_ROOT%\_Build\windows\kyty_emulator.exe"
if not exist "%EMU_EXE%" set "EMU_EXE=%REPO_ROOT%\kyty_emulator.exe"

if not exist "%EMU_EXE%" (
    echo [ERROR] No se encontro kyty_emulator.exe.
    echo Compile el proyecto antes de continuar:
    echo   cmake -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
    echo   ninja -C _Build/windows kyty_emulator
    echo.
    pause
    exit /b 1
)

set "GAME_PATH=%~1"
if "%GAME_PATH%"=="" (
    if defined ASTROBOT_GAME_PATH set "GAME_PATH=%ASTROBOT_GAME_PATH%"
)

if "%GAME_PATH%"=="" (
    echo.
    echo Ingrese la ruta al eboot.bin o carpeta del juego:
    set /p "GAME_PATH=> "
)
set "GAME_PATH=%GAME_PATH:"=%"

if not exist "%GAME_PATH%" (
    echo [ERROR] Ruta no encontrada: "%GAME_PATH%"
    pause
    exit /b 1
)

:menu
cls
echo ============================================================================
echo   KytyPS5 - SUITE DE COMPARACION A/B - ASTRO BOT (PPSA21567)
echo ============================================================================
echo   Ejecutable: %EMU_EXE%
echo   Juego:      %GAME_PATH%
echo.
echo   Seleccione el perfil a ejecutar:
echo.
echo     [1] Perfil A: Baseline Estable (Sin paralelizar PM4, control estricto)
echo     [2] Perfil B: Experimental (CpSequencer + DrawPrep 4 workers, Etapa 8.4E)
echo     [3] Abrir carpeta de logs (_TestLogs)
echo     [4] Salir
echo.
echo ============================================================================
set /p "CHOICE=Seleccione una opcion [1-4]: "

if "%CHOICE%"=="1" (
    call "%SCRIPT_DIR%run_astrobot_profile_a_baseline.bat" "%GAME_PATH%"
    echo.
    pause
    goto menu
)
if "%CHOICE%"=="2" (
    call "%SCRIPT_DIR%run_astrobot_profile_b_experimental.bat" "%GAME_PATH%"
    echo.
    pause
    goto menu
)
if "%CHOICE%"=="3" (
    if not exist "%REPO_ROOT%\_TestLogs" mkdir "%REPO_ROOT%\_TestLogs"
    explorer "%REPO_ROOT%\_TestLogs"
    goto menu
)
if "%CHOICE%"=="4" (
    exit /b 0
)

goto menu
