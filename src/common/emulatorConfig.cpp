#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/gpuStall.h"

#include <algorithm>
#include <memory>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	Common::DebugCounters::SetEnabled(false);
	// A backstop that lets any audio thread still held go, so none waits through process exit.
	Common::GpuStall::Configure(false);
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));
	EXIT_IF(cfg.controller_speaker_volume > 100 || cfg.controller_vibration_intensity > 100);
	EXIT_IF(cfg.keyboard_player > static_cast<uint32_t>(MAX_PLAYERS));

	*g_config = cfg;
	Common::DebugCounters::SetEnabled(cfg.perf_overlay_enabled);
	// Pushed here rather than read by the audio code, so audio tests need no config.
	Common::GpuStall::Configure(cfg.audio_compile_hold_enabled);
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

const std::string& GetAudioInputDevice() {
	return g_config->audio_input_device;
}

const std::optional<ControllerColor>& GetControllerColor() {
	return g_config->controller_color;
}

uint32_t GetControllerSpeakerVolume() {
	return g_config->controller_speaker_volume;
}

uint32_t GetControllerVibrationIntensity() {
	return g_config->controller_vibration_intensity;
}

uint32_t GetKeyboardPlayer() {
	return g_config->keyboard_player;
}

int32_t GetPlayerUserId(int player) {
	return g_config->user_id + player * PLAYER_USER_ID_STEP;
}

int GetPlayerFromUserId(int32_t user_id) {
	for (int player = 0; player < MAX_PLAYERS; player++) {
		if (user_id == GetPlayerUserId(player)) {
			return player;
		}
	}
	return -1;
}

bool IsGuestUserId(int32_t user_id) {
	return GetPlayerFromUserId(user_id) > 0;
}

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

bool HdrEnabled() {
	return g_config->hdr_enabled;
}

bool HideCursorEnabled() {
	return g_config->hide_cursor_enabled;
}

bool StretchEnabled() {
	return g_config->stretch_enabled;
}

bool VrEnabled() {
	return g_config->vr_enabled;
}

bool Ps5ProEnabled() {
	return g_config->pro_enabled;
}

PssrUpscaler GetPssrUpscaler() {
	return g_config->pssr_upscaler;
}

const std::filesystem::path& GetDlssFolder() {
	return g_config->dlss_folder;
}

uint32_t GetDlssPreset() {
	return g_config->dlss_preset;
}

const std::filesystem::path& GetFsrFolder() {
	return g_config->fsr_folder;
}

bool AmdCpuEnabled() {
	return g_config->amd_cpu_enabled;
}

bool GuestTscEnabled() {
	return g_config->guest_tsc_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool IsNetworkConnected() {
	return g_config->network_connected;
}

bool PsnEmulationEnabled() {
	return g_config->psn_emulation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

bool ShaderCacheEnabled() {
	return g_config->shader_cache_enabled;
}

bool AsyncShaderCompileEnabled() {
	return g_config->async_shader_compile_enabled;
}

LogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

LogDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

bool ProfilerEnabled() {
	return g_config->profiler_enabled;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool ReadbackTiledImagesEnabled() {
	return g_config->readback_tiled_images;
}

bool ReadbackStorageImagesEnabled() {
	return g_config->readback_storage_images;
}

bool SyncRawImageBuffersEnabled() {
	return g_config->sync_raw_image_buffers;
}

bool SkipRedundantUnmapDrainsEnabled() {
	return g_config->skip_redundant_unmap_drains;
}

bool PartialBufferRefreshEnabled() {
	return g_config->partial_buffer_refresh_enabled;
}

uint32_t GetFlushDraws() {
	return g_config->flush_draws;
}

bool BatchHotReadbacksEnabled() {
	return g_config->batch_hot_readbacks;
}

bool TessellationEnabled() {
	return g_config->tessellation_enabled;
}

bool OcclusionQueriesEnabled() {
	return g_config->occlusion_queries_enabled;
}

bool RelaxedShaderFloatEnabled() {
	return g_config->relaxed_shader_float;
}

bool NativeRtEnabled() {
	return g_config->native_rt;
}

bool LodStatsEnabled() {
	return g_config->lod_stats_enabled;
}

bool AudioCompileHoldEnabled() {
	return g_config->audio_compile_hold_enabled;
}

bool GpuSubmitCadenceEnabled() {
	return g_config != nullptr ? g_config->gpu_submit_cadence_enabled : false;
}

bool TrophyEnabled() {
	return g_config->trophy_enabled;
}

uint32_t GetTrophyDurationSeconds() {
	return std::clamp(g_config->trophy_duration_seconds, 5u, 10u);
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

bool SkipNoticeScreen() {
	return g_config->skip_notice_screen;
}

// Reports the opt-in --keep-gpu-cache-on-reuse setting.
bool KeepGpuCacheOnReuse() {
	return g_config->keep_gpu_cache_on_reuse;
}

const std::string& GetLaunchActivity() {
	return g_config->launch_activity;
}

bool PollBackoffEnabled() {
	return g_config->poll_backoff_enabled;
}

bool AuditFaultsEnabled() {
	return g_config != nullptr ? g_config->audit_faults_enabled : false;
}

bool AuditManualTriggerEnabled() {
	return g_config != nullptr ? g_config->audit_manual_trigger : false;
}

bool ExtendedStreamBufferEnabled() {
	return g_config != nullptr ? g_config->extended_stream_buffer_enabled : false;
}

const std::string& GetAuditExportPath() {
	static const std::string empty;
	return g_config != nullptr ? g_config->audit_export_path : empty;
}

uint64_t GetGpuSubmitCadenceIntervalUs() {
	return g_config != nullptr && g_config->gpu_submit_cadence_interval_us > 0
	           ? g_config->gpu_submit_cadence_interval_us
	           : 1000;
}

uint32_t GetAuditCaptureDelaySec() {
	return g_config != nullptr ? g_config->audit_capture_delay_sec : 0;
}

uint32_t GetAuditCaptureDurationSec() {
	return g_config != nullptr ? g_config->audit_capture_duration_sec : 0;
}

bool RedundantBindElisionEnabled() {
	return g_config != nullptr ? g_config->redundant_bind_elision_enabled : true;
}

bool PipelineCacheMruEnabled() {
	return g_config != nullptr ? g_config->pipeline_cache_mru_enabled : true;
}

bool PipelineKeyCacheEnabled() {
	return g_config != nullptr ? g_config->pipeline_key_cache_enabled : true;
}

bool DrawStateDirtyCheckEnabled() {
	return g_config != nullptr ? g_config->draw_state_dirty_check_enabled : false;
}

bool EnsurePipelineCacheEnabled() {
	return g_config != nullptr ? g_config->ensure_pipeline_cache_enabled : true;
}

bool DescriptorCacheEnabled() {
	return g_config != nullptr ? g_config->descriptor_cache_enabled : true;
}

bool ProgramCacheMruEnabled() {
	return g_config != nullptr ? g_config->program_cache_mru_enabled : true;
}

bool CoalesceReadFaultsEnabled() {
	return g_config != nullptr ? g_config->coalesce_read_faults_enabled : true;
}

bool GpuTimestampsEnabled() {
	return g_config != nullptr ? g_config->gpu_timestamps_enabled : true;
}

bool DiagnosticSkipWaterPassEnabled() {
	return g_config != nullptr ? g_config->diagnostic_skip_water_enabled : false;
}

bool FastPendingOpsEnabled() {
	return g_config != nullptr ? g_config->fast_pending_ops_enabled : true;
}

bool PrioritySpinWaitEnabled() {
	return g_config != nullptr ? g_config->priority_spin_wait_enabled : true;
}

uint32_t PrioritySpinDurationUs() {
	return g_config != nullptr ? g_config->priority_spin_duration_us : 100;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}

void EnableRedZoneProtection() {
	g_config->red_zone_protection_enabled = true;
}
#endif

const Keymap& GetKeymap() {
	return g_config->keymap;
}

const std::vector<std::string>& GetGuestArguments() {
	return g_config->guest_arguments;
}

} // namespace Config
