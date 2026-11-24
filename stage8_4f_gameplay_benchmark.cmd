@echo off
setlocal
cd /d "%~dp0"
set "PROFILE=%~1"
if /i "%PROFILE%"=="A" goto run
if /i "%PROFILE%"=="B" goto run
if /i "%PROFILE%"=="C" goto run
echo Etapa 8.4F: A = control, B = CpSequencer, C = CpSequencer + DrawPrep
set /p "PROFILE=Perfil [A/B/C]: "
:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0stage8_4f_run.ps1" -Profile "%PROFILE%"
set "RESULT=%ERRORLEVEL%"
echo Resultado: %RESULT%. Los logs se guardan en _Build\windows\stage8_4f_runs.
pause
exit /b %RESULT%
