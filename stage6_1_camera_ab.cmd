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

:menu
cls
echo =====================================================================
echo    KytyPS5 - Etapa 6.1: DIAGNOSTICO A/B DE CAMARA EN ASTRO BOT
echo =====================================================================
echo.
echo Configuracion de Motor fija (Etapa 5):
echo   [+] GPU Submit Cadence: 1.0 ms
echo   [+] Partial Buffer Refresh: ON
echo   [+] Coalesced Multi-Page Write Faults: ON
echo   [+] Empty Submit Elision: ON
echo.
echo Ejecutable: %EMU_EXE%
echo Duracion de cada captura: 30 segundos
echo.
echo Seleccione la prueba a ejecutar:
echo   [1] Captura A: Vista Amplia (Camara hacia gran parte del mapa)
echo       Salida: stage6_1_camera_wide.json / .csv
echo.
echo   [2] Captura B: Vista Reducida (Camara apuntando al suelo / menos objetos)
echo       Salida: stage6_1_camera_ground.json / .csv
echo.
echo   [3] Salir
echo.
set /p "CHOICE=Elija una opcion [1, 2, 3]: "

if "%CHOICE%"=="1" goto run_wide
if "%CHOICE%"=="2" goto run_ground
if "%CHOICE%"=="3" exit /b 0
goto menu

:run_wide
echo.
echo =====================================================================
echo    INICIANDO CAPTURA A: VISTA AMPLIA (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO RIGUROSO:
echo 1. Se abrira el emulador en modo WARMUP (NO grabara al iniciar).
echo 2. Vaya a la escena Aztec Sky en la posicion de prueba.
echo 3. Deje al personaje inmóvil y oriente la camara mostrando GRAN PARTE DEL MAPA.
echo 4. Espere 20-30 segundos para estabilizar shaders y streaming.
echo 5. Presione F9 para iniciar la captura de 30 segundos.
echo 6. El emulador finalizara y exportara automaticamente a:
echo    stage6_1_camera_wide.json y stage6_1_camera_wide.csv
echo 7. Cierre el emulador tras ver [COMPLETED] en pantalla/consola.
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage6_1_camera_wide.json"
goto finish

:run_ground
echo.
echo =====================================================================
echo    INICIANDO CAPTURA B: VISTA REDUCIDA / SUELO (30 SEGUNDOS)
echo =====================================================================
echo PROTOCOLO RIGUROSO:
echo 1. Se abrira el emulador en modo WARMUP (NO grabara al iniciar).
echo 2. Vaya a la misma escena Aztec Sky en la EXACTA MISMA POSICION del personaje.
echo 3. Oriente la camara apuntando AL SUELO o hacia los costados (pocos objetos).
echo 4. Espere 20-30 segundos para estabilizar shaders y streaming.
echo 5. Presione F9 para iniciar la captura de 30 segundos.
echo 6. El emulador finalizara y exportara automaticamente a:
echo    stage6_1_camera_ground.json y stage6_1_camera_ground.csv
echo 7. Cierre el emulador tras ver [COMPLETED] en pantalla/consola.
echo.
pause
"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --gpu-submit-cadence --gpu-submit-cadence-ms 1.0 --partial-buffer-refresh --audit-faults --audit-manual-trigger --audit-capture-duration 30 --audit-export "stage6_1_camera_ground.json"
goto finish

:finish
echo.
echo =====================================================================
echo    Prueba finalizada. Verifique los archivos generados.
echo =====================================================================
pause
