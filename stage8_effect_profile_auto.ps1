param(
    [string]$GameDirectory = 'C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app',
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
$replay = Join-Path $PSScriptRoot 'tools\benchmarks\astro_sky_garden_effects.json'
if ($Preview) {
    & (Join-Path $PSScriptRoot 'stage8_4f_run.ps1') -Profile A -Transfers Baseline -EffectProfile -KeyboardPlayer 1 -GameDirectory $GameDirectory -CaptureSeconds 40 -MaxRuntimeSeconds 550 -Preview
    Get-Content -LiteralPath $replay
    return
}
$previousReplay = $env:KYTY_BENCH_REPLAY
$started = Get-Date
try {
    $env:KYTY_BENCH_REPLAY = $replay
    & (Join-Path $PSScriptRoot 'stage8_4f_run.ps1') -Profile A -Transfers Baseline -EffectProfile -KeyboardPlayer 1 -GameDirectory $GameDirectory -CaptureSeconds 40 -MaxRuntimeSeconds 550
    if ($LASTEXITCODE -ne 0) { throw "El emulador termino con salida $LASTEXITCODE" }
} finally { $env:KYTY_BENCH_REPLAY = $previousReplay }
$run = Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '_Build\windows\stage8_4f_runs') -Directory |
    Where-Object LastWriteTime -ge $started | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$run) { throw 'No se encontro la ejecucion de efectos.' }
$gameLog = Get-Content -LiteralPath (Join-Path $run.FullName 'stdout.log') -Raw
if ($gameLog -notmatch '(StartLevel aerial_garden|Level has started: aerial_garden)') {
    throw "La repeticion no confirmo Sky Garden; descartar esta ejecucion: $($run.FullName)"
}
$sample = Get-Content -LiteralPath (Join-Path $run.FullName 'control1.json') -Raw | ConvertFrom-Json
if ($sample.metadata.capture_state -ne 'Completed' -or $sample.graphics_workload.draws_per_frame -lt 3500) {
    throw "La prueba no alcanzo la escena esperada: $($run.FullName)"
}
& python (Join-Path $PSScriptRoot 'tools\benchmarks\analyze_effect_profile.py') $run.FullName
if ($LASTEXITCODE -ne 0) { throw 'Fallo el analisis por shader.' }
Write-Host "Diagnostico por shader: $($run.FullName)"
