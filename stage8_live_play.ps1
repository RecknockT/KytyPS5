param([string]$TitleId = 'PPSA21567', [switch]$Preview, [switch]$ManualAB, [switch]$TrimScratchMemory, [switch]$AllImprovements, [ValidateRange(0,360)][int]$FrameLimit = 30, [ValidateSet('resource_prep_reuse','read_buffer_reuse','compact_image_readback')][string]$ABMode = 'resource_prep_reuse')
$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$settingsPath = 'C:\ProgramData\Kyty\Kyty.ini'
$section = ''
$globalSettings = @{}
$games = @{}
foreach ($line in Get-Content -LiteralPath $settingsPath) {
    if ($line -match '^\[(.+)\]$') { $section = $Matches[1]; continue }
    if ($line -notmatch '^([^=]+)=(.*)$') { continue }
    $key, $value = $Matches[1], $Matches[2]
    if ($section -eq 'GlobalConfiguration') { $globalSettings[$key] = $value }
    if ($section -eq 'GameConfigurations' -and $key -match '^(\d+)\\(.+)$') {
        $index, $gameKey = $Matches[1], $Matches[2]
        if (!$games.ContainsKey($index)) { $games[$index] = @{} }
        $games[$index][$gameKey] = $value
    }
}
$matchesForTitle = @($games.Values | Where-Object { $_['basedir'] -like "*$TitleId*" })
if ($matchesForTitle.Count -ne 1) { throw 'No hay una configuración única para ese juego.' }
$game = $matchesForTitle[0]
$cfg = $globalSettings.Clone()
if ($game['custom_settings'] -eq 'true') { foreach ($key in $game.Keys) { $cfg[$key] = $game[$key] } }
$gameDirectory = $game['basedir']
$exe = Join-Path $repoRoot '_Build/windows/kyty_emulator.exe'
$runDirectory = Join-Path $repoRoot ('_Build/windows/live_play/' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
$emuArgs = [System.Collections.Generic.List[string]]::new()
$emuArgs.Add('--game'); $emuArgs.Add($gameDirectory)
$resolution = $cfg['screen_resolution'] -split 'x'
if ($resolution.Count -ne 2) { throw 'Resolución inválida.' }
$valueOptions = @{
    '--screen-width'=$resolution[0]; '--screen-height'=$resolution[1]
    '--gpu'=$cfg['gpu_index']; '--present-mode'=$cfg['present_mode']
    '--user-name'=$cfg['user_name']; '--user-id'=$cfg['user_id']
    '--vblank-frequency'=$cfg['vblank_frequency']; '--console-language'=$cfg['console_language']
    '--printf-direction'=$cfg['printf_direction']; '--printf-output-file'=$cfg['printf_output_file']
    '--shader-log-direction'=$cfg['shader_log_direction']; '--shader-log-folder'=$cfg['shader_log_folder']
    '--flush-draws'=$cfg['flush_draws']; '--trophy-duration'=$cfg['trophy_duration_seconds']
}
foreach ($key in $valueOptions.Keys) { $emuArgs.Add($key); $emuArgs.Add([string]$valueOptions[$key]) }
$flags = @{
    'fullscreen_enabled'='--fullscreen'; 'hdr_enabled'='--hdr'; 'hide_cursor_enabled'='--hide-cursor'
    'stretch_enabled'='--stretch'; 'readback_linear_images'='--readback-linear-images'
    'readback_tiled_images'='--readback-tiled-images'; 'readback_storage_images'='--readback-storage-images'
    'sync_raw_image_buffers'='--sync-raw-image-buffers'; 'skip_notice_screen'='--skip-notice-screen'
    'network_connected'='--network-connected'; 'psn_emulation_enabled'='--emulate-psn'
    'tessellation_enabled'='--tessellation'; 'occlusion_queries_enabled'='--occlusion-queries'
    'relaxed_shader_float'='--relaxed-shader-float'; 'native_rt'='--native-rt'
    'shader_cache_enabled'='--shader-cache'; 'guest_tsc_enabled'='--guest-tsc'
    'amd_cpu_enabled'='--amd-cpu'; 'red_zone_protection_enabled'='--redzone'
    'vulkan_validation_enabled'='--vulkan-validation'; 'shader_validation_enabled'='--shader-validation'
}
foreach ($key in $flags.Keys) { if ($cfg[$key] -eq 'true') { $emuArgs.Add($flags[$key]) } }
if ($cfg['trophy_enabled'] -eq 'false') { $emuArgs.Add('--no-trophy-notifications') }
$emuArgs.Add($(if ($cfg['gpu_submit_cadence_enabled'] -eq 'true') { '--gpu-submit-cadence' } else { '--no-gpu-submit-cadence' }))
foreach ($entry in @(@('controller_speaker_volume','--controller-volume','50'), @('controller_vibration_intensity','--controller-vibration','100'), @('controller_keyboard_player','--keyboard-player','0'))) {
    $emuArgs.Add($entry[1]); $emuArgs.Add($(if ($cfg.ContainsKey($entry[0])) { $cfg[$entry[0]] } else { $entry[2] }))
}
$patch = Join-Path $repoRoot "_Build/windows/launcher/_Patches/$TitleId.json"
if (Test-Path -LiteralPath $patch) { $emuArgs.Add('--game-patch'); $emuArgs.Add($patch) }
if ($TrimScratchMemory) { $emuArgs.Add('--trim-scratch-memory') }
$emuArgs.Add('--fps-limit'); $emuArgs.Add([string]$FrameLimit)
$emuArgs.AddRange([string[]]@('--audit-faults','--live-diagnostics','--audit-capture-duration','0','--audit-export', (Join-Path $runDirectory 'audit.json'), '--perf-overlay','--effect-profile','--no-gpu-timestamps','--no-tile-layout-cache','--no-binding-reuse','--no-cp-sequencer','--no-draw-prep','--no-draw-state-dirty-check'))
if ($ManualAB) {
    $emuArgs.AddRange([string[]]@('--no-live-diagnostics','--no-effect-profile','--audit-manual-trigger','--audit-capture-duration','40','--no-resource-prep-reuse','--no-read-buffer-reuse','--no-compact-image-readback'))
}
if ($AllImprovements) {
    if ($ManualAB) { throw 'La prueba conjunta no se combina con la comparación A/B.' }
    $emuArgs.AddRange([string[]]@('--trim-scratch-memory','--compact-image-readback','--resource-prep-reuse','--read-buffer-reuse','--binding-reuse','--tile-layout-cache'))
}
# Reject embedded quotes; all current launcher values are plain strings and paths.
if (@($emuArgs | Where-Object { $_.Contains('"') -or $_.EndsWith('\') }).Count) { throw 'Argumento no admitido por el lanzador de diagnóstico.' }
if ($Preview) { [pscustomobject]@{directory=$runDirectory; arguments=$emuArgs} | ConvertTo-Json -Depth 4; return }
if (Get-Process kyty_emulator -ErrorAction SilentlyContinue) { throw 'Ya hay un emulador abierto. Se conserva la sesión existente.' }
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$metadata = [ordered]@{started=(Get-Date).ToString('o'); executable=$exe; sha256=(Get-FileHash -LiteralPath $exe).Hash; arguments=$emuArgs; titleId=$TitleId; automatic_input=$false; automatic_quit=$false}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'run.json')
$previousReplay = $env:KYTY_BENCH_REPLAY
$previousArm = $env:KYTY_BENCH_ARM_FILE
try {
    $env:KYTY_BENCH_REPLAY = $null
    $env:KYTY_BENCH_ARM_FILE = $null
    if ($ManualAB) {
        $commands = @(
            @{at_ms=0;action=$ABMode;enabled=$false},
            @{at_ms=2000;action='capture';label='control1'},
            @{at_ms=47000;action=$ABMode;enabled=$true},
            @{at_ms=52000;action='capture';label='candidate1'},
            @{at_ms=97000;action='capture';label='candidate2'},
            @{at_ms=142000;action=$ABMode;enabled=$false},
            @{at_ms=147000;action='capture';label='control2'}
        )
        $replayPath = Join-Path $runDirectory 'replay.json'
        $armPath = Join-Path $runDirectory 'start_measurement'
        $commands | ConvertTo-Json | Set-Content -LiteralPath $replayPath -Encoding utf8NoBOM
        $env:KYTY_BENCH_REPLAY = $replayPath
        $env:KYTY_BENCH_ARM_FILE = $armPath
        $metadata.benchmarkReplay = $replayPath
        $metadata.manual_arm_file = $armPath
    }
    $quotedArgs = @($emuArgs | ForEach-Object { '"' + $_ + '"' })
    $gameProcess = Start-Process -FilePath $exe -ArgumentList $quotedArgs -WorkingDirectory $repoRoot -WindowStyle Normal -PassThru -RedirectStandardOutput (Join-Path $runDirectory 'stdout.log') -RedirectStandardError (Join-Path $runDirectory 'stderr.log')
    $metadata.pid = $gameProcess.Id
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'run.json')
} finally { $env:KYTY_BENCH_REPLAY = $previousReplay; $env:KYTY_BENCH_ARM_FILE = $previousArm }
# This is the user's interactive session: no timeout, replay, watcher or automatic close.
[pscustomobject]@{pid=$gameProcess.Id; directory=$runDirectory}
