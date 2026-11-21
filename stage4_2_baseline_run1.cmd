@echo off
setlocal
set "GAME_DIR=C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app"

set "EMU_EXE=%~dp0kyty_emulator.exe"
if not exist "%EMU_EXE%" set "EMU_EXE=%~dp0_Build\windows\kyty_emulator.exe"
if not exist "%EMU_EXE%" set "EMU_EXE=C:\Users\Recko\Documents\github\kyty\_Build\windows\kyty_emulator.exe"

echo =====================================================================
echo    KytyPS5 - Etapa 4.2 Benchmark: BASELINE (Ronda 1 - Test 1)
echo =====================================================================
echo.
echo Ejecutable: %EMU_EXE%
echo Parametros: --audit-faults --audit-manual-trigger --audit-capture-duration 180
echo Salida:     stage4_2_baseline_run1.json / .csv
echo.
echo PROTOCOLO DE BENCHMARK RIGUROSO:
echo 1. Se abrira el juego en modo WARMUP (el profiler NO grabara al inicio).
echo 2. Espere a que termine de cargar y llegue a la ESCENA DE PRUEBA interactiva.
echo 3. Mantenga la camara fija en la misma posicion.
echo 4. Espere entre 20 y 30 segundos para estabilizar la carga de recursos.
echo 5. Presione F9 para INICIAR la captura de 180 segundos.
echo    (El overlay mostrara [F9] Audit: RECORDING en rojo).
echo 6. Tras 180s se exportara automaticamente y el overlay mostrara COMPLETED.
echo 7. Cierre el emulador.
echo.
pause

"%EMU_EXE%" --game "%GAME_DIR%" --shader-cache --audio-compile-hold --perf-overlay --audit-faults --audit-manual-trigger --audit-capture-duration 180 --audit-export "stage4_2_baseline_run1.json"
