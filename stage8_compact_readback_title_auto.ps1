param([switch]$Preview)
$ErrorActionPreference='Stop'
$replay = Join-Path $PSScriptRoot 'tools/benchmarks/astro_compact_readback_title_replay.json'
if ($Preview) { Get-Content -LiteralPath $replay; return }
$started=Get-Date
$previousReplay=$env:KYTY_BENCH_REPLAY
$previousArm=$env:KYTY_BENCH_ARM_FILE
try {
    $env:KYTY_BENCH_REPLAY=$replay
    $env:KYTY_BENCH_ARM_FILE=$null
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'stage8_4f_run.ps1') -Profile A -PerfOverlay -Transfers Baseline -SubmitCadence -ScreenWidth 2560 -ScreenHeight 1440 -RelaxedShaderFloat -ReadbackLinear -NoGpuTimestamps -KeyboardPlayer 0 -CaptureSeconds 40 -MaxRuntimeSeconds 400
    if ($LASTEXITCODE -ne 0) { throw "La prueba terminó con salida $LASTEXITCODE" }
} finally { $env:KYTY_BENCH_REPLAY=$previousReplay; $env:KYTY_BENCH_ARM_FILE=$previousArm }
$run=Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '_Build/windows/stage8_4f_runs') -Directory | Where-Object LastWriteTime -ge $started | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$run) { throw 'No se encontró la ejecución.' }
$log=Get-Content -LiteralPath (Join-Path $run.FullName 'stdout.log') -Raw
if ($log -notmatch 'Level has started: title_controller_ship' -or $log -match 'StartLevel (worldmap|aerial_garden|intro) ') { throw 'La escena de título cambió; descartar la comparación.' }
& python (Join-Path $PSScriptRoot 'tools/benchmarks/compare_mode_benchmark.py') $run.FullName compact_image_readback
if ($LASTEXITCODE -ne 0) { throw 'La comparación no cumple las condiciones de medición.' }
Write-Host "Prueba exclusiva del título; no representa FPS de Sky Garden: $($run.FullName)"
