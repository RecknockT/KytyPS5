<#
.SYNOPSIS
    KytyPS5 - Perfil B: PM4 + DrawPrep Experimental para Astro Bot (PPSA21567)
.DESCRIPTION
    Ejecuta el emulador KytyPS5 con la arquitectura de paralelización PM4:
    CpSequencer desacoplado, 4 workers de DrawPrep para decodificación paralela,
    y 100% de reutilización de resultados en Thread_Gpu (Etapa 8.4E).
.PARAMETER GamePath
    Ruta al archivo eboot.bin o al directorio del juego Astro Bot.
.PARAMETER Workers
    Numero de workers de DrawPrep (por defecto 4).
.PARAMETER EmuExe
    Ruta opcional al ejecutable kyty_emulator.exe.
#>
param(
    [Parameter(Position=0)]
    [string]$GamePath = $env:ASTROBOT_GAME_PATH,
    [Parameter()]
    [int]$Workers = 4,
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
$LogFile = "$LogDir\astrobot_profile_b_experimental_$Timestamp.json"

$GitRev = "desconocido"
try {
    $GitRev = (git -C $RepoRoot rev-parse --short HEAD 2>$null).Trim()
} catch {}

Write-Host "============================================================================" -ForegroundColor Magenta
Write-Host "  KytyPS5 - PERFIL B: PM4 + DRAWPREP EXPERIMENTAL (ETAPA 8.4E)" -ForegroundColor Magenta
Write-Host "============================================================================" -ForegroundColor Magenta
Write-Host "  Commit Git:        $GitRev"
Write-Host "  Ejecutable:        $EmuExe"
Write-Host "  Juego:             $GamePath"
Write-Host "  Workers DrawPrep:  $Workers"
Write-Host "  Log Telemetria:    $LogFile"
Write-Host "  Optimizacion:      CpSequencer + DrawPrep ($Workers workers) + Cero Duplicacion"
Write-Host "============================================================================" -ForegroundColor Magenta

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
    "--cp-sequencer",
    "--draw-prep",
    "--draw-prep-workers", $Workers.ToString(),
    "--audit-faults",
    "--audit-export", $LogFile
)

$process = Start-Process -FilePath $EmuExe -ArgumentList $arguments -PassThru -NoNewWindow -Wait
Write-Host "`nSesion finalizada con codigo de salida: $($process.ExitCode)"
if (Test-Path $LogFile) {
    Write-Host "[OK] Telemetria exportada a: $LogFile" -ForegroundColor Cyan
}
exit $process.ExitCode
