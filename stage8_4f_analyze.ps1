param([string]$RunDirectory)
$ErrorActionPreference = 'Stop'
if (!$RunDirectory) {
    $RunDirectory = (Get-ChildItem (Join-Path $PSScriptRoot '_Build\windows\stage8_4f_runs') -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
}
if (!$RunDirectory) { throw 'No hay ejecuciones para analizar.' }
$metadata = Get-Content -LiteralPath (Join-Path $RunDirectory 'run.json') -Raw | ConvertFrom-Json
$events = @()
foreach ($line in Get-Content -LiteralPath (Join-Path $RunDirectory 'stderr.log')) {
    if ($line.StartsWith('{')) {
        try { $events += ($line | ConvertFrom-Json) } catch { }
    }
}
$heartbeats = @($events | Where-Object { $_.type -eq 'cpseq' -and $_.phase -eq 'watchdog' } | Sort-Object ns)
$longestUnchanged = 0.0
$unchangedSince = $null
$previousExecuted = $null
foreach ($beat in $heartbeats) {
    if ($beat.executed -ne $previousExecuted) { $unchangedSince = $beat.ns }
    if ($null -ne $unchangedSince) {
        $longestUnchanged = [math]::Max($longestUnchanged, ($beat.ns - $unchangedSince) / 1e9)
    }
    $previousExecuted = $beat.executed
}
$processSamples = @(Get-Content -LiteralPath (Join-Path $RunDirectory 'process.jsonl') -ErrorAction SilentlyContinue | ForEach-Object { $_ | ConvertFrom-Json })
$milestones = @(Get-Content -LiteralPath (Join-Path $RunDirectory 'stdout.log') | Select-String 'StartLevel|GAME: Level has started:' | ForEach-Object { $_.Line })
$maxOutstanding = 0
foreach ($beat in $heartbeats) {
    $maxOutstanding = [math]::Max($maxOutstanding, $beat.snapshots_acquired - $beat.snapshots_released)
}
$summary = [ordered]@{
    profile=$metadata.profile; sha=$metadata.sha; executableHash=$metadata.executableHash
    exitCode=$metadata.exitCode; exited=$metadata.exited; forcedTermination=$metadata.forcedTermination
    milestones=$milestones; diagnosticHeartbeats=$heartbeats.Count
    longestSampledUnchangedExecutedSeconds=[math]::Round($longestUnchanged,3)
    maxSampledOutstandingSnapshots=$maxOutstanding
    lastHeartbeat=($heartbeats | Select-Object -Last 1)
    lastMemoryWait=($events | Where-Object type -eq 'cp_wait_mem' | Select-Object -Last 1)
    lastDrawPrepState=($events | Where-Object type -eq 'drawprep' | Select-Object -Last 1)
    peakWorkingSetBytes=($processSamples | Measure-Object workingSet -Maximum).Maximum
    lastProcessSample=($processSamples | Select-Object -Last 1)
    interpretation='La falta de progreso muestreada es una pista, no una demostracion de deadlock. No usar el titulo de ventana ni VBlank como FPS medidos.'
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $RunDirectory 'diagnostics_summary.json')
if ($heartbeats.Count) { $heartbeats | Export-Csv -LiteralPath (Join-Path $RunDirectory 'diagnostics_cp.csv') -NoTypeInformation }
$summary | ConvertTo-Json -Depth 10
