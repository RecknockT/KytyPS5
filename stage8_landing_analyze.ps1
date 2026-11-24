param(
    [Parameter(Mandatory=$true)][string]$Capture,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$capturePath = (Resolve-Path -LiteralPath $Capture).Path
$framePath = Join-Path ([IO.Path]::GetDirectoryName($capturePath)) ([IO.Path]::GetFileNameWithoutExtension($capturePath) + '_frames.csv')
if (!(Test-Path -LiteralPath $framePath)) { throw "Falta la serie por cuadro: $framePath" }
$summary = Get-Content -LiteralPath $capturePath -Raw | ConvertFrom-Json
$frames = @(Import-Csv -LiteralPath $framePath)
if (!$frames.Count -or !$summary.frametime_summary.total_frames) {
    throw 'Captura vacia: no permite evaluar el aterrizaje aunque exista un CSV antiguo.'
}
if ($summary.frametime_summary.total_frames -ne $frames.Count) {
    throw 'El numero de filas no coincide con el resumen; revisar truncamiento o mezcla de archivos.'
}
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot '_Build/windows/landing_analysis'
}
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$culture = [Globalization.CultureInfo]::InvariantCulture
function Number($value) { return [double]::Parse([string]$value, $culture) }
function Average($rows, $field, $scale=1) {
    return (($rows | ForEach-Object { Number $_.$field } | Measure-Object -Average).Average / $scale)
}
$phaseRows = @(foreach ($group in ($frames | Group-Object phase)) {
    $rows = @($group.Group)
    $durations = @($rows | ForEach-Object { Number $_.frame_ms } | Sort-Object)
    $sum = ($durations | Measure-Object -Sum).Sum
    [pscustomobject][ordered]@{
        phase = $group.Name
        frames = $rows.Count
        recorded_frame_seconds = $sum/1000
        mean_frame_ms = $sum/$rows.Count
        reciprocal_mean_fps = 1000*$rows.Count/$sum
        p50_ms = $durations[[Math]::Min($rows.Count-1,[int][Math]::Floor($rows.Count*0.50))]
        p95_ms = $durations[[Math]::Min($rows.Count-1,[int][Math]::Floor($rows.Count*0.95))]
        max_ms = $durations[-1]
        draws_per_frame = Average $rows draws
        submits_per_frame = Average $rows submits
        descriptor_ms_per_frame = Average $rows descriptor_prep_us 1000
        render_state_ms_per_frame = Average $rows render_state_prep_us 1000
        sync_thread_accumulated_ms_per_frame = Average $rows sync_waits_us 1000
        stream_wait_ms_per_frame = Average $rows stream_buffer_wait_us 1000
        write_faults_per_frame = Average $rows write_faults
    }
})
$warnings = @(
    'Las fases son etiquetas manuales F10: no prueban cuando termino la carga del juego.'
    'Los tiempos de etapas son anidados y las esperas acumulan varios hilos; no sumarlos como partes exclusivas de un cuadro.'
    'Los contadores de compilacion son globales de la captura, no estan separados por fase.'
    'No comparar capturas como A/B si difieren recorrido, calidad, revision u opciones.'
)
if (!$summary.cpu_gpu_separation.gpu_timestamps_supported) {
    $warnings += 'Sin tiempos de ejecucion Vulkan medidos: no descarta limitacion de GPU fisica.'
}
if ($summary.configuration.draw_state_dirty_check) {
    $warnings += 'Esta captura usa Draw State Dirty Check: actualmente desactivado por regresion visual conocida.'
}
if (!($phaseRows.phase -contains 'ActiveDescent')) {
    $warnings += 'No hay fase ActiveDescent identificada: no clasificar esta captura como aterrizaje completo.'
}
$report = [ordered]@{
    source = $capturePath
    series = $framePath
    metadata = $summary.metadata
    configuration = $summary.configuration
    workload = $summary.graphics_workload
    gpu_waits = $summary.gpu_timeline_waits
    memory_transfers = $summary.memory_transfers
    phases = $phaseRows
    warnings = $warnings
}
$stem = [IO.Path]::GetFileNameWithoutExtension($capturePath)
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory ($stem + '_analysis.json'))
$phaseRows | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $OutputDirectory ($stem + '_phases.csv'))
$phaseRows | Format-Table phase,frames,recorded_frame_seconds,mean_frame_ms,p95_ms,draws_per_frame,descriptor_ms_per_frame,sync_thread_accumulated_ms_per_frame
$warnings | ForEach-Object { Write-Host $_ }
