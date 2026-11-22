<#
.SYNOPSIS
    KytyPS5 - Perfil A: Baseline Estable para Astro Bot (PPSA21567)
.DESCRIPTION
    Ejecuta el emulador KytyPS5 con la configuracion base estable probada:
    submit cadence, bind elision, caches de pipeline/descriptores activadas,
    y ejecucion sincronica monohilo en Thread_Gpu.
.PARAMETER GamePath
    Ruta al archivo eboot.bin o al directorio del juego Astro Bot.
.PARAMETER EmuExe
    Ruta opcional al ejecutable kyty_emulator.exe (por defecto _Build/windows/kyty_emulator.exe).
#>
param(
    [Parameter(Position=0)]
    [string]$GamePath = $env:ASTROBOT_GAME_PATH,
    [Parameter()]
    [string]$EmuExe = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path

if (-not $EmuExe) {
    $candidates = @(
        "$RepoRoot\_Build\windows\kyty_emulator.exe",
        "$RepoRoot\kyty_emulator.exe"
    )
    foreach ($cand in $candidates) {
        if (Test-Path $cand) {
            $EmuExe = $cand
            break
        }
    }
}

if (-not (Test-Path $EmuExe)) {
    Write-Error "No se encontro el ejecutable kyty_emulator.exe. Compile con Ninja primero."
}

if (-not $GamePath) {
    Write-Host "Ingrese la ruta completa al eboot.bin o directorio de Astro Bot:" -ForegroundColor Cyan
    $GamePath = Read-Host "> "
}

$GamePath = $GamePath.Trim('"')
if (-not (Test-Path $GamePath)) {
    Write-Error "La ruta especificada no existe: $GamePath"
}

$LogDir = "$RepoRoot\_TestLogs"
if (-not (Test-Path $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir | Out-Null
}

$Timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
$LogFile = "$LogDir\astrobot_profile_a_baseline_$Timestamp.json"

$GitRev = "desconocido"
try {
    $GitRev = (git -C $RepoRoot rev-parse --short HEAD 2>$null).Trim()
} catch {}

Write-Host "============================================================================" -ForegroundColor Green
Write-Host "  KytyPS5 - PERFIL A: BASELINE ESTABLE (CONTROL)" -ForegroundColor Green
Write-Host "============================================================================" -ForegroundColor Green
Write-Host "  Commit Git:        $GitRev"
Write-Host "  Ejecutable:        $EmuExe"
Write-Host "  Juego:             $GamePath"
Write-Host "  Log Telemetria:    $LogFile"
Write-Host "  Configuracion:     PM4 Sincrono, DrawPrep Desactivado, Dirty Check Desactivado"
Write-Host "============================================================================" -ForegroundColor Green

$arguments = @(
    "--game", $GamePath,
    "--gpu-submit-cadence",
    "--redundant-bind-elision",
    "--pipeline-cache-mru",
    "--pipeline-key-cache",
    "--ensure-pipeline-cache",
    "--descriptor-cache",
    "--fast-pending-ops",
    "--priority-spin-wait",
    "--no-draw-state-dirty-check",
    "--no-cp-sequencer",
    "--no-draw-prep",
    "--audit-faults",
    "--audit-export", $LogFile
)

$process = Start-Process -FilePath $EmuExe -ArgumentList $arguments -PassThru -NoNewWindow -Wait
Write-Host "`nSesion finalizada con codigo de salida: $($process.ExitCode)"
if (Test-Path $LogFile) {
    Write-Host "[OK] Telemetria exportada a: $LogFile" -ForegroundColor Cyan
}
exit $process.ExitCode
