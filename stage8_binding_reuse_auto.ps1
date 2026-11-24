param(
    [string]$GameDirectory = 'C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app',
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
$replay = Join-Path $PSScriptRoot 'tools\benchmarks\astro_sky_garden_replay.json'
$commands = @(Get-Content -LiteralPath $replay -Raw | ConvertFrom-Json)
if ($Preview) {
    [pscustomobject]@{ replay=$replay; secondsAfterWarmup=($commands[-1].at_ms / 1000); order='ABBA'; captureSeconds=40; commands=$commands } | ConvertTo-Json -Depth 5
    return
}
$previousReplay = $env:KYTY_BENCH_REPLAY
$started = Get-Date
try {
    $env:KYTY_BENCH_REPLAY = $replay
    & (Join-Path $PSScriptRoot 'stage8_4f_run.ps1') -Profile A -Transfers Baseline -KeyboardPlayer 1 -GameDirectory $GameDirectory -CaptureSeconds 40 -MaxRuntimeSeconds 700
    if ($LASTEXITCODE -ne 0) { throw "El emulador termino con salida $LASTEXITCODE" }
} finally {
    $env:KYTY_BENCH_REPLAY = $previousReplay
}
$run = Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '_Build\windows\stage8_4f_runs') -Directory |
    Where-Object LastWriteTime -ge $started | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$run) { throw 'No se encontro la ejecucion automatica.' }
$gameLog = Get-Content -LiteralPath (Join-Path $run.FullName 'stdout.log') -Raw
if ($gameLog -notmatch '(StartLevel aerial_garden|Level has started: aerial_garden)') {
    throw "La repeticion no confirmo Sky Garden; descartar esta ejecucion: $($run.FullName)"
}
$results = foreach ($label in @('control1','candidate1','candidate2','control2')) {
    $path = Join-Path $run.FullName ($label + '.json')
    if (!(Test-Path -LiteralPath $path)) { throw "Falta captura $label; revisar $($run.FullName)" }
    $j = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($j.metadata.capture_state -ne 'Completed') { throw "Captura incompleta: $label" }
    $expected = $label.StartsWith('candidate')
    if ($j.configuration.binding_reuse -ne $expected) { throw "Modo incorrecto en $label" }
    if ($j.graphics_workload.draws_per_frame -lt 3500) { throw "La repeticion no llego a la escena pesada en $label" }
    [pscustomobject]@{ label=$label; enabled=$expected; frames=$j.frametime_summary.total_frames; seconds=$j.metadata.real_duration_seconds; fps=$j.frametime_summary.effective_fps; p95=$j.frametime_summary.p95_frame_ms; draws=$j.graphics_workload.draws_per_frame; shaders=$j.graphics_workload.shaders_compiled; pipelines=$j.graphics_workload.pipelines_created }
}
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run.FullName 'auto_summary.json')
$results | Format-Table
Write-Host "Resultados y series por cuadro: $($run.FullName)"
