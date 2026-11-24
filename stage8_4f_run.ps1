param(
    [ValidateSet('A','B','C')][string]$Profile = 'A',
    [string]$GameDirectory = 'C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app',
    [ValidateRange(0,3600)][int]$BootSeconds = 0,
    [switch]$TraceOps,
    [switch]$ColdBoot,
    [ValidateRange(1,3600)][int]$CaptureSeconds = 30,
    [ValidateSet('Baseline','Hot','Neighbours','Wide','FaultAhead','Combined')]
    [string]$Transfers = 'Baseline',
    [switch]$BindingReuse,
    [int]$ScreenWidth = 0,
    [int]$ScreenHeight = 0,
    [switch]$RelaxedShaderFloat,
    [switch]$ReadbackLinear,
    [switch]$EffectProfile,
    [switch]$SubmitCadence,
    [switch]$PerfOverlay,
    [switch]$AutomationGates,
    [switch]$SynchronousShaders,
    [switch]$NoGpuTimestamps,
    [ValidateRange(0,4)][int]$KeyboardPlayer = 0,
    [switch]$Preview,
    [ValidateRange(0,3600)][int]$MaxRuntimeSeconds = 0
)
$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$emuPath = Join-Path $repoRoot '_Build\windows\kyty_emulator.exe'
if (!(Test-Path -LiteralPath $emuPath)) { throw "Falta Release: $emuPath" }
if (!(Test-Path -LiteralPath (Join-Path $GameDirectory 'eboot.bin'))) { throw 'No se encontro eboot.bin.' }
$runDirectory = Join-Path $repoRoot ('_Build\windows\stage8_4f_runs\' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '_' + $Profile)
$profileFlags = switch ($Profile) {
    'A' { @('--no-cp-sequencer','--no-draw-prep') }
    'B' { @('--cp-sequencer','--no-draw-prep') }
    'C' { @('--cp-sequencer','--draw-prep','--draw-prep-workers','4') }
}
$emuArguments = @('--game', ('"' + $GameDirectory + '"'), '--no-draw-state-dirty-check', '--shader-cache', '--audit-faults', '--audit-export', ('"' + (Join-Path $runDirectory 'audit.json') + '"')) + $profileFlags
$emuArguments += @('--no-batch-hot-readbacks','--no-readback-neighbours','--no-wide-readback-window','--no-fault-ahead')
$transferArguments = @(switch ($Transfers) {
    'Hot' { '--batch-hot-readbacks' }
    'Neighbours' { '--readback-neighbours' }
    'Wide' { '--wide-readback-window' }
    'FaultAhead' { '--fault-ahead' }
    'Combined' { @('--batch-hot-readbacks','--readback-neighbours','--wide-readback-window','--fault-ahead') }
    default { @() }
})
$emuArguments += $transferArguments
if ($ScreenWidth -gt 0 -and $ScreenHeight -gt 0) { $emuArguments += @('--screen-width', [string]$ScreenWidth, '--screen-height', [string]$ScreenHeight) }
if ($RelaxedShaderFloat) { $emuArguments += '--relaxed-shader-float' }
if ($ReadbackLinear) { $emuArguments += '--readback-linear-images' }
$emuArguments += $(if ($SubmitCadence) { '--gpu-submit-cadence' } else { '--no-gpu-submit-cadence' })
if ($PerfOverlay) { $emuArguments += '--perf-overlay' }
if ($SynchronousShaders) { $emuArguments += '--no-async-shader-compile' }
if ($NoGpuTimestamps) { $emuArguments += '--no-gpu-timestamps' }
$emuArguments += $(if ($EffectProfile) { '--effect-profile' } else { '--no-effect-profile' })
$emuArguments += @('--keyboard-player', [string]$KeyboardPlayer)
$emuArguments += $(if ($BindingReuse) { '--binding-reuse' } else { '--no-binding-reuse' })
if ($BootSeconds -eq 0) {
    $emuArguments += @('--audit-manual-trigger','--audit-capture-duration',[string]$CaptureSeconds)
    if (!$Preview) { Write-Host "F9 inicia captura de $CaptureSeconds segundos; F10 avanza las fases del aterrizaje. Mantener la misma escena y configuracion A/B/C." }
} else {
    $emuArguments += @('--audit-capture-duration', [string]$BootSeconds)
}
if ($ColdBoot) { $emuArguments += '--no-shader-cache' }
if ($Preview) {
    [pscustomobject]@{profile=$Profile; transfers=$Transfers; bindingReuse=[bool]$BindingReuse; effectProfile=[bool]$EffectProfile; arguments=$emuArguments} | ConvertTo-Json -Depth 5
    return
}
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$oldDiagnostics = $env:KYTY_CP_SEQ_DIAGNOSTICS
$oldTraceOps = $env:KYTY_CP_SEQ_TRACE_OPS
$oldCpSeq = $env:KYTY_CP_SEQ
$env:KYTY_CP_SEQ_DIAGNOSTICS = '1'
$env:KYTY_CP_SEQ_TRACE_OPS = if ($TraceOps) { '1' } else { '0' }
$env:KYTY_CP_SEQ = if ($Profile -eq 'A') { 'off' } else { 'thread' }
$process = $null
$ownedChildren = @{}
if (!('Stage84fNativeProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Stage84fNativeProbe {
 [StructLayout(LayoutKind.Sequential)] public struct IoCounters {
  public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
 }
 [StructLayout(LayoutKind.Sequential)] public struct PerformanceInfo {
  public uint Size;
  public UIntPtr CommitTotal, CommitLimit, CommitPeak, PhysicalTotal, PhysicalAvailable, SystemCache, KernelTotal, KernelPaged, KernelNonpaged, PageSize;
  public uint HandleCount, ProcessCount, ThreadCount;
 }
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetProcessIoCounters(IntPtr process, out IoCounters counters);
 [DllImport("psapi.dll", SetLastError=true)] public static extern bool GetPerformanceInfo(out PerformanceInfo info, uint size);
}
'@
}
try {
    $metadata = [ordered]@{ profile=$Profile; transfers=$Transfers; sha=(& git -C $repoRoot rev-parse HEAD); arguments=$emuArguments; bootSeconds=$BootSeconds; captureSeconds=$CaptureSeconds; maxRuntimeSeconds=$MaxRuntimeSeconds; coldBoot=[bool]$ColdBoot; traceOps=[bool]$TraceOps; started=(Get-Date).ToString('o'); executableHash=(Get-FileHash -LiteralPath $emuPath).Hash }
    $metadata.benchmarkReplay = $env:KYTY_BENCH_REPLAY
    if ($env:KYTY_BENCH_REPLAY -and (Test-Path -LiteralPath $env:KYTY_BENCH_REPLAY)) {
        Copy-Item -LiteralPath $env:KYTY_BENCH_REPLAY -Destination (Join-Path $runDirectory 'replay.json')
    }
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDirectory 'run.json')
    & git -C $repoRoot diff --ignore-submodules -- src tests CMakeLists.txt docs | Set-Content -LiteralPath (Join-Path $runDirectory 'source.patch')
    $process = Start-Process -FilePath $emuPath -ArgumentList $emuArguments -WorkingDirectory $repoRoot -PassThru -WindowStyle Normal -RedirectStandardOutput (Join-Path $runDirectory 'stdout.log') -RedirectStandardError (Join-Path $runDirectory 'stderr.log')
    $metadata.pid = $process.Id
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDirectory 'run.json')
    Write-Host "Perfil $Profile; PID $($process.Id); logs: $runDirectory"
    $runtimeLimit = if ($MaxRuntimeSeconds -gt 0) { $MaxRuntimeSeconds } else { $BootSeconds }
    $deadline = if ($runtimeLimit -gt 0) { (Get-Date).AddSeconds($runtimeLimit) } else { [datetime]::MaxValue }
    $nextInventory = Get-Date
    $gateWaitStarts = @{}
    while (!$process.WaitForExit(500)) {
        $process.Refresh()
        $sample = [ordered]@{ time=(Get-Date).ToString('o'); pid=$process.Id; cpuSeconds=$process.TotalProcessorTime.TotalSeconds; workingSet=$process.WorkingSet64; privateBytes=$process.PrivateMemorySize64; windowTitle=$process.MainWindowTitle }
        $ioCounters = New-Object Stage84fNativeProbe+IoCounters
        if ([Stage84fNativeProbe]::GetProcessIoCounters($process.Handle, [ref]$ioCounters)) {
            # Logical process I/O includes cached reads and log writes; it is not physical SSD traffic.
            $sample.ioReadTransferBytes = $ioCounters.ReadBytes
            $sample.ioWriteTransferBytes = $ioCounters.WriteBytes
        }
        $systemMemory = New-Object Stage84fNativeProbe+PerformanceInfo
        if ([Stage84fNativeProbe]::GetPerformanceInfo([ref]$systemMemory, [Runtime.InteropServices.Marshal]::SizeOf($systemMemory))) {
            $pageBytes = $systemMemory.PageSize.ToUInt64()
            $sample.systemCommitBytes = $systemMemory.CommitTotal.ToUInt64() * $pageBytes
            $sample.systemCommitLimitBytes = $systemMemory.CommitLimit.ToUInt64() * $pageBytes
            $sample.systemAvailableRamBytes = $systemMemory.PhysicalAvailable.ToUInt64() * $pageBytes
        }
        $sample | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $runDirectory 'process.jsonl')
        if ($AutomationGates) {
            $navigationLog = Get-Content -LiteralPath (Join-Path $runDirectory 'stdout.log') -Raw
            $gatePatterns = @{
                worldmap_ready='(StartLevel worldmap|Level has started: worldmap)'
                galaxy_loaded='LoadLevelResources: hub_crashsite :'
                sky_garden_started='(StartLevel aerial_garden|Level has started: aerial_garden)'
            }
            foreach ($gateName in $gatePatterns.Keys) {
                $gatePath = Join-Path $runDirectory $gateName
                if (!(Test-Path -LiteralPath $gatePath) -and $navigationLog -match $gatePatterns[$gateName]) {
                    (Get-Date).ToString('o') | Set-Content -LiteralPath $gatePath
                }
            }
            $waitingGates = [regex]::Matches([string]$navigationLog, 'BenchmarkReplay: waiting for load gate ([a-z_]+)')
            if ($waitingGates.Count -gt 0) {
                $waitingGate = $waitingGates[$waitingGates.Count-1].Groups[1].Value
                if (!(Test-Path -LiteralPath (Join-Path $runDirectory $waitingGate))) {
                    if (!$gateWaitStarts.ContainsKey($waitingGate)) { $gateWaitStarts[$waitingGate] = Get-Date }
                    if (((Get-Date) - $gateWaitStarts[$waitingGate]).TotalSeconds -ge 180) {
                        $metadata.navigationFailure = "Timed out waiting for $waitingGate; no performance result is valid."
                        Write-Warning $metadata.navigationFailure
                        break
                    }
                }
            }
        }
        # Inventory only descendants of this task-owned process; never kill by executable name.
        if ((Get-Date) -ge $nextInventory) {
          try {
            foreach ($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($process.Id)" -OperationTimeoutSec 1 -ErrorAction Stop)) {
                $ownedChildren[$child.ProcessId] = @{ started=$child.CreationDate; path=$child.ExecutablePath }
            }
          } catch { }
          $nextInventory = (Get-Date).AddSeconds(5)
        }
        if ((Get-Date) -ge $deadline) { break }
    }
    if (!$process.HasExited) {
        $null = $process.CloseMainWindow()
        if (!$process.WaitForExit(10000)) {
            $process.Kill()
            $process.WaitForExit()
            $metadata.forcedTermination = $true
        }
    }
    $metadata.exitCode = $process.ExitCode
    $metadata.exited = $process.HasExited
    $metadata.finished = (Get-Date).ToString('o')
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDirectory 'run.json')
    Get-Content -LiteralPath (Join-Path $runDirectory 'stdout.log') -Tail 18
    Get-Content -LiteralPath (Join-Path $runDirectory 'stderr.log') -Tail 12
} finally {
    if ($process -and !$process.HasExited) {
        $null = $process.CloseMainWindow()
        if (!$process.WaitForExit(10000)) { $process.Kill(); $process.WaitForExit() }
    }
    foreach ($childId in $ownedChildren.Keys) {
        $childProcess = Get-Process -Id $childId -ErrorAction SilentlyContinue
        if (!$childProcess) { continue }
        $identity = $ownedChildren[$childId]
        if ($childProcess.Path -eq $identity.path -and [math]::Abs(($childProcess.StartTime - $identity.started).TotalSeconds) -lt 2) {
            $null = $childProcess.CloseMainWindow()
            if (!$childProcess.WaitForExit(5000)) { $childProcess.Kill(); $childProcess.WaitForExit() }
        } else { Write-Warning "No se pudo verificar identidad del descendiente PID $childId; se conserva." }
    }
    $env:KYTY_CP_SEQ_DIAGNOSTICS = $oldDiagnostics
    $env:KYTY_CP_SEQ_TRACE_OPS = $oldTraceOps
    $env:KYTY_CP_SEQ = $oldCpSeq
}
if ($process) { exit $process.ExitCode }
