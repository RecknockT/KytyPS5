#include "common/archive.h"
#include "common/common.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "emulator.h"
#include "graphics/presentation/hostFrameLimiter.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <magic_enum.hpp>
#include <string_view>
#include <vector>

using namespace Common;
using namespace Emulator;

static void PrintUsage() {
	::printf("%s, %s, ver = %s, git = %s, date = %s\n",
	         KYTY_BUILD == KYTY_BUILD_DEBUG ? "Debug" : "Release", KYTY_TOOLCHAIN, KYTY_VERSION,
	         KYTY_GIT_VERSION, __DATE__);
	::printf("kyty_emulator --game <dir|elf|zar> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf|zar>                 Game directory, ELF, or ZArchive to load.\n");
	::printf("  --game-patch <json>                  ETAHen cheat file.\n");
	::printf(
	    "  --fps-limit <0-360>                  Host presentation cap; 0 disables (default).\n");
	::printf("  --no-fps-limit                       Disable the host presentation cap.\n");
	::printf("  --addcont-dir <dir>                  Add-ons (DLC) folder (default: none).\n");
	::printf("  --addcont <label>[=<dir>]            Add-on (DLC) by entitlement label with its "
	         "data in\n"
	         "                                       <dir>, or license only without <dir>. "
	         "Repeatable.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Kyty.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf(
	    "  --mic <name>                        Capture from this microphone; omit for silence.\n");
	::printf("  --controller-color <#RRGGBB>        Override the controller lightbar color.\n");
	::printf("  --controller-volume <0-100>         DualSense speaker volume. Default: 50.\n");
	::printf(
	    "  --controller-vibration <0-100>      DualSense vibration intensity. Default: 100.\n");
	::printf("  --keyboard-player <0-4>             Keyboard player; 0 shares player 1 with the "
	         "gamepad. Default: 0.\n");
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Mailbox.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf("  --hdr                                Enable HDR10 output when supported.\n");
	::printf(
	    "  --hide-cursor                        Hide the cursor after 2 s idle. Default: off.\n");
	::printf(
	    "  --stretch                            Stretch the image to the window. Default: off.\n");
	::printf("  --no-splash                          Disable the startup splash screen.\n");
	::printf("  --vr                                 Enable the virtual VR headset.\n");
	::printf("  --trinity                            Enable experimental PS5 Pro mode for\n"
	         "                                       supported titles. Default: off.\n");
	::printf("  --pssr-upscaler <value>              With --trinity, upscale PSSR with Linear,\n"
	         "                                       DLSS, or FSR. Default: Linear.\n");
	::printf("  --dlss-folder <folder>               DLSS model folder.\n");
	::printf("  --dlss-preset <A-O>                  DLSS preset. Default: the model's own.\n");
	::printf("  --fsr-folder <folder>                FidelityFX DLL folder (Windows).\n");
	::printf("  --amd-cpu                            Apply AMD CPU instruction patches.\n");
	::printf("  --guest-tsc                          Run the guest TSC at the PS5's 1.6 GHz.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation                  Enable Vulkan validation.\n");
	::printf("  --network-connected                  Use the host local network.\n");
	::printf("  --emulate-psn                        Simulate PSN sign-in for compatibility.\n");
	::printf("  --gpu-assisted-validation            Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation                  Enable shader validation.\n");
	::printf("  --shader-cache                       Keep compiled shaders and pipelines between "
	         "boots (default: on).\n");
	::printf("  --no-shader-cache                    Disable shader caching between boots.\n");
	::printf("  --no-async-shader-compile            Compile shaders on the render thread, which "
	         "waits for\n"
	         "                                       them. By default they compile on worker "
	         "threads and\n"
	         "                                       draws using a shader still being compiled are "
	         "skipped.\n");
	::printf(
	    "  --tessellation                      Draw tessellation patches; skipped by default.\n");
	::printf("  --occlusion-queries                  Count occlusion query samples on the GPU.\n");
	::printf("  --relaxed-shader-float               Native GPU float ops in shaders; faster, can "
	         "be inaccurate.\n");
	::printf("  --native-rt                          Use native RT on the GPU.\n");
	::printf("  --lod-stats                          Report bound textures in mip statistics.\n");
	::printf(
	    "  --audio-compile-hold                 Pause game audio while a long pipeline compile\n"
	    "                                       blocks the GPU.\n");
	::printf(
	    "  --gpu-submit-cadence                 Submit GPU work periodically while a long command\n"
	    "                                       stream records, so the GPU starts sooner.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump                Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump                Enable graphics debug dumps.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file.\n");
	::printf("  --profile                            Enable the Tracy profiler.\n");
	::printf("  --spirv-debug-printf                 Enable SPIR-V debug printf.\n");
	::printf("  --sync-raw-image-buffers             Synchronize raw reads of GPU images.\n");
	::printf("  --skip-redundant-unmap-drains        Skip the GPU drain when unmapping an already "
	         "unmapped range.\n");
	::printf("  --partial-buffer-refresh             Refresh only the buffer-written part of an "
	         "image.\n");
	::printf("  --redundant-bind-elision             Elide redundant Vulkan pipeline and buffer "
	         "binds (default: on).\n");
	::printf("  --no-redundant-bind-elision          Disable redundant Vulkan bind elision.\n");
	::printf("  --pipeline-cache-mru                 Enable MRU fast-path for graphics pipeline "
	         "lookups (default: on).\n");
	::printf("  --no-pipeline-cache-mru              Disable MRU fast-path for graphics pipeline "
	         "lookups.\n");
	::printf("  --pipeline-key-cache                 Cache and elide redundant pipeline key builds "
	         "(default: on).\n");
	::printf("  --no-pipeline-key-cache              Disable pipeline key caching.\n");
	::printf("  --draw-state-dirty-check             Elide redundant draw state and render target "
	         "preparations (default: off).\n");
	::printf("  --no-draw-state-dirty-check          Disable draw state dirty checking.\n");
	::printf("  --ensure-pipeline-cache              Fast-path MRU pipeline key check in "
	         "EnsureGraphicsPipeline (default: on).\n");
	::printf(
	    "  --no-ensure-pipeline-cache           Disable EnsureGraphicsPipeline fast-path cache.\n");
	::printf("  --descriptor-cache                   Fast-path descriptor binding caching across "
	         "draws (default: on).\n");
	::printf("  --no-descriptor-cache                Disable descriptor binding caching.\n");
	::printf(
	    "  --no-gpu-submit-cadence              Disable periodic GPU submissions (default).\n");
	::printf("  --effect-profile / --no-effect-profile  Diagnostic shader CPU/GPU ranking "
	         "(default: off).\n");
	::printf("  --live-diagnostics / --no-live-diagnostics  Flush a one-second live journal "
	         "(default: off).\n");
	::printf("  --resource-prep-reuse / --no-resource-prep-reuse  Reuse validated resource "
	         "preparation (default: off).\n");
	::printf("  --gpu-timestamps / --no-gpu-timestamps  Enable/disable diagnostic GPU queries.\n");
	::printf("  --tile-layout-cache / --no-tile-layout-cache  Reuse pure tiled texture layouts "
	         "(default: off).\n");
	::printf("  --binding-reuse                      Reuse unchanged buffer/sampler plans "
	         "(experimental).\n");
	::printf(
	    "  --no-binding-reuse                   Disable buffer/sampler plan reuse (default).\n");
	::printf("  --fast-pending-ops                   Lock-free fast check for pending operations "
	         "(default: on).\n");
	::printf(
	    "  --no-fast-pending-ops                Disable lock-free check for pending operations.\n");
	::printf("  --priority-spin-wait                 Low-latency spin wait for priority GPU "
	         "operations (default: on).\n");
	::printf("  --no-priority-spin-wait              Disable spin wait and sleep directly on "
	         "condition variable.\n");
	::printf("  --priority-spin-duration-us <u>      Maximum duration in microseconds to spin "
	         "(default: 100).\n");
	::printf("  --cp-sequencer                       Decoupled PM4 packet sequencer and op stream "
	         "(default: off, experimental).\n");
	::printf("  --no-cp-sequencer                    Disable decoupled PM4 packet sequencer.\n");
	::printf(
	    "  --cp-sequencer-spin-us <u>           Maximum duration in microseconds for sequencer to "
	    "spin (default: 200).\n");
	::printf("  --draw-prep                          Parallel draw preparation with worker threads "
	         "(default: off, experimental).\n");
	::printf("  --no-draw-prep                       Disable parallel draw preparation.\n");
	::printf(
	    "  --draw-prep-workers <num>            Worker threads for draw preparation (default: 4, "
	    "1-32).\n");
	::printf(
	    "  --batch-hot-readbacks                When a fault readback waits for the GPU, also "
	    "read\n"
	    "                                       back recently faulted buffers and finished small\n"
	    "                                       GPU writes.\n");
	::printf("  --no-batch-hot-readbacks             Disable hot-buffer readback batching.\n");
	::printf("  --readback-neighbours                Download nearby buffers in the same wait "
	         "(experimental, off).\n");
	::printf("  --no-readback-neighbours             Disable neighbouring-buffer readbacks.\n");
	::printf("  --wide-readback-window               Use a 4 MiB readback window (experimental, "
	         "off).\n");
	::printf("  --no-wide-readback-window            Restore the 512 KiB readback window.\n");
	::printf("  --fault-ahead                        Use a 256 KiB write-fault window "
	         "(experimental, off).\n");
	::printf("  --no-fault-ahead                     Restore the 64 KiB write-fault window.\n");
	::printf(
	    "  --flush-draws <num>                  Submit GPU work every <num> draws; 0 is off.\n");
	::printf(
	    "  --no-trophy-notifications            Disable trophy unlock toasts and their sound.\n");
	::printf("  --trophy-duration <seconds>          Trophy toast duration, 5-10. Default: 6.\n");
	::printf("  --trim-scratch-memory                Release idle or oversized tiling scratch "
	         "buffers.\n");
	::printf("  --no-trim-scratch-memory             Keep scratch allocations (default).\n");
	::printf("  --compact-image-readback             Avoid preserving padding for fully packed "
	         "linear image readbacks.\n");
	::printf(
	    "  --no-compact-image-readback          Use full preserving image readbacks (default).\n");
	::printf("  --perf-overlay                       Show the performance panel (F2 or Cmd+P).\n");
	::printf("  --playgo-hack                        Report 1000 PlayGo chunks without chunk "
	         "metadata.\n");
	::printf("  --skip-notice-screen                 Skip startup logos and notices in supported "
	         "games.\n");
	::printf("  --launch-activity <id>               Launch into this activity (1-32 printable "
	         "ASCII chars).\n");
	::printf("  --no-poll-backoff                    Let failed event-flag polls spin instead of "
	         "yielding.\n");
	::printf("  --keep-gpu-cache-on-reuse            Keep GPU buffer caches when released direct "
	         "memory\n"
	         "                                       is mapped back unchanged. Experimental. "
	         "Default: off.\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::printf("  --redzone                            Protect the guest SysV red zone.\n");
#endif
	::printf("  --keymap <Control=Input>             DualSense mapping; may be repeated.\n");
	::printf("  --audit-faults                       Record low-overhead profiling data for page "
	         "faults,\n"
	         "                                       re-protections, GPU waits and queue "
	         "submissions.\n");
	::printf("  --audit-export <path>                Path to export audit profile (JSON/CSV) at "
	         "shutdown.\n");
	::printf("  --audit-capture-delay <sec>          Wait <sec> seconds before starting audit "
	         "capture.\n");
	::printf("  --audit-capture-duration <sec>       Automatically stop and export capture after "
	         "<sec> seconds.\n");
	::printf("  --audit-manual-trigger               Do not start capture at launch; wait for F9 "
	         "hotkey in target scene.\n");
	::printf("  --gpu-submit-cadence-ms <ms>         Interval for periodic GPU submit cadence in "
	         "ms (default 1.0).\n");
	::printf("  --gpu-submit-cadence-interval-us <u> Interval for periodic GPU submit cadence in "
	         "us (default 1000).\n");
	::printf("  --extended-stream-buffer             Expand stream and download ring buffers "
	         "(256/128 MiB) to\n"
	         "                                       mitigate queue wrap stalls and stuttering "
	         "under cadence.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
}

static bool NextArg(int argc, char* argv[], int& index, std::string& out) {
	if (index + 1 >= argc) {
		return false;
	}

	index++;
	out = argv[index];
	return true;
}

template <typename E>
static bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value, magic_enum::case_insensitive);
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

static bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

static bool ParseUint32(const std::string& value, uint32_t& out) {
	uint32_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseControllerColor(const std::string& value, Config::ControllerColor& out) {
	if (value.size() != 7 || value[0] != '#') {
		return false;
	}
	uint32_t rgb      = 0;
	auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), rgb, 16);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
	       static_cast<uint8_t>(rgb)};
	return true;
}

static bool ParseInt32(const std::string& value, int32_t& out) {
	int32_t number    = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

static bool ParseUint64(const std::string& value, uint64_t& out) {
	uint64_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseDouble(const std::string& value, double& out) {
	char*        end = nullptr;
	const double d   = std::strtod(value.c_str(), &end);
	if (end == value.c_str() || *end != '\0') {
		return false;
	}
	out = d;
	return true;
}

using Config::ConfigOptions;

struct Flag {
	std::string_view name;
	bool ConfigOptions::* field;
	bool                  value = true;
};

static constexpr Flag FLAGS[] = {
    {"--rd", &ConfigOptions::renderdoc_enabled},
    {"--fullscreen", &ConfigOptions::fullscreen_enabled},
    {"--hdr", &ConfigOptions::hdr_enabled},
    {"--hide-cursor", &ConfigOptions::hide_cursor_enabled},
    {"--stretch", &ConfigOptions::stretch_enabled},
    {"--no-splash", &ConfigOptions::splash_enabled, false},
    {"--vr", &ConfigOptions::vr_enabled},
    {"--trinity", &ConfigOptions::pro_enabled},
    {"--amd-cpu", &ConfigOptions::amd_cpu_enabled},
    {"--guest-tsc", &ConfigOptions::guest_tsc_enabled},
    {"--network-connected", &ConfigOptions::network_connected},
    {"--emulate-psn", &ConfigOptions::psn_emulation_enabled},
    {"--vulkan-validation", &ConfigOptions::vulkan_validation_enabled},
    {"--gpu-assisted-validation", &ConfigOptions::gpu_assisted_validation_enabled},
    {"--shader-validation", &ConfigOptions::shader_validation_enabled},
    {"--shader-cache", &ConfigOptions::shader_cache_enabled},
    {"--no-shader-cache", &ConfigOptions::shader_cache_enabled, false},
    {"--async-shader-compile", &ConfigOptions::async_shader_compile_enabled},
    {"--no-async-shader-compile", &ConfigOptions::async_shader_compile_enabled, false},
    {"--tessellation", &ConfigOptions::tessellation_enabled},
    {"--occlusion-queries", &ConfigOptions::occlusion_queries_enabled},
    {"--relaxed-shader-float", &ConfigOptions::relaxed_shader_float},
    {"--native-rt", &ConfigOptions::native_rt},
    {"--lod-stats", &ConfigOptions::lod_stats_enabled},
    {"--audio-compile-hold", &ConfigOptions::audio_compile_hold_enabled},
    {"--gpu-submit-cadence", &ConfigOptions::gpu_submit_cadence_enabled},
    {"--command-buffer-dump", &ConfigOptions::command_buffer_dump_enabled},
    {"--graphics-debug-dump", &ConfigOptions::graphics_debug_dump_enabled},
    {"--profile", &ConfigOptions::profiler_enabled},
    {"--spirv-debug-printf", &ConfigOptions::spirv_debug_printf_enabled},
    {"--readback-linear-images", &ConfigOptions::readback_linear_images},
    {"--readback-tiled-images", &ConfigOptions::readback_tiled_images},
    {"--readback-storage-images", &ConfigOptions::readback_storage_images},
    {"--sync-raw-image-buffers", &ConfigOptions::sync_raw_image_buffers},
    {"--skip-redundant-unmap-drains", &ConfigOptions::skip_redundant_unmap_drains},
    {"--partial-buffer-refresh", &ConfigOptions::partial_buffer_refresh_enabled},
    {"--batch-hot-readbacks", &ConfigOptions::batch_hot_readbacks},
    {"--no-batch-hot-readbacks", &ConfigOptions::batch_hot_readbacks, false},
    {"--readback-neighbours", &ConfigOptions::readback_neighbours},
    {"--no-readback-neighbours", &ConfigOptions::readback_neighbours, false},
    {"--wide-readback-window", &ConfigOptions::wide_readback_window},
    {"--no-wide-readback-window", &ConfigOptions::wide_readback_window, false},
    {"--fault-ahead", &ConfigOptions::fault_ahead},
    {"--no-fault-ahead", &ConfigOptions::fault_ahead, false},
    {"--no-trophy-notifications", &ConfigOptions::trophy_enabled, false},
    {"--perf-overlay", &ConfigOptions::perf_overlay_enabled},
    {"--playgo-hack", &ConfigOptions::playgo_hack_enabled},
    {"--skip-notice-screen", &ConfigOptions::skip_notice_screen},
    {"--no-poll-backoff", &ConfigOptions::poll_backoff_enabled, false},
    {"--keep-gpu-cache-on-reuse", &ConfigOptions::keep_gpu_cache_on_reuse},
    {"--audit-faults", &ConfigOptions::audit_faults_enabled},
    {"--trim-scratch-memory", &ConfigOptions::trim_scratch_memory_enabled},
    {"--no-trim-scratch-memory", &ConfigOptions::trim_scratch_memory_enabled, false},
    {"--compact-image-readback", &ConfigOptions::compact_image_readback_enabled},
    {"--no-compact-image-readback", &ConfigOptions::compact_image_readback_enabled, false},
    {"--read-buffer-reuse", &ConfigOptions::read_buffer_reuse_enabled},
    {"--no-read-buffer-reuse", &ConfigOptions::read_buffer_reuse_enabled, false},
    {"--resource-prep-reuse", &ConfigOptions::resource_prep_reuse_enabled},
    {"--no-resource-prep-reuse", &ConfigOptions::resource_prep_reuse_enabled, false},
    {"--live-diagnostics", &ConfigOptions::live_diagnostics_enabled},
    {"--no-live-diagnostics", &ConfigOptions::live_diagnostics_enabled, false},
    {"--audit-manual-trigger", &ConfigOptions::audit_manual_trigger},
    {"--audit-wait-for-hotkey", &ConfigOptions::audit_manual_trigger},
    {"--extended-stream-buffer", &ConfigOptions::extended_stream_buffer_enabled},
    {"--redundant-bind-elision", &ConfigOptions::redundant_bind_elision_enabled, true},
    {"--no-redundant-bind-elision", &ConfigOptions::redundant_bind_elision_enabled, false},
    {"--pipeline-cache-mru", &ConfigOptions::pipeline_cache_mru_enabled, true},
    {"--no-pipeline-cache-mru", &ConfigOptions::pipeline_cache_mru_enabled, false},
    {"--pipeline-key-cache", &ConfigOptions::pipeline_key_cache_enabled, true},
    {"--no-pipeline-key-cache", &ConfigOptions::pipeline_key_cache_enabled, false},
    {"--draw-state-dirty-check", &ConfigOptions::draw_state_dirty_check_enabled, true},
    {"--no-draw-state-dirty-check", &ConfigOptions::draw_state_dirty_check_enabled, false},
    {"--ensure-pipeline-cache", &ConfigOptions::ensure_pipeline_cache_enabled, true},
    {"--no-ensure-pipeline-cache", &ConfigOptions::ensure_pipeline_cache_enabled, false},
    {"--resolve-texture-cache-512", &ConfigOptions::resolve_texture_cache_512_enabled, true},
    {"--no-resolve-texture-cache-512", &ConfigOptions::resolve_texture_cache_512_enabled, false},
    {"--profile-programs", &ConfigOptions::profile_programs_enabled, true},
    {"--no-profile-programs", &ConfigOptions::profile_programs_enabled, false},
    {"--descriptor-cache", &ConfigOptions::descriptor_cache_enabled, true},
    {"--no-descriptor-cache", &ConfigOptions::descriptor_cache_enabled, false},
    {"--render-target-cache", &ConfigOptions::render_target_cache_enabled, true},
    {"--no-render-target-cache", &ConfigOptions::render_target_cache_enabled, false},
    {"--effect-profile", &ConfigOptions::effect_profile_enabled, true},
    {"--no-effect-profile", &ConfigOptions::effect_profile_enabled, false},
    {"--gpu-timestamps", &ConfigOptions::gpu_timestamps_enabled, true},
    {"--no-gpu-timestamps", &ConfigOptions::gpu_timestamps_enabled, false},
    {"--no-gpu-submit-cadence", &ConfigOptions::gpu_submit_cadence_enabled, false},
    {"--tile-layout-cache", &ConfigOptions::tile_layout_cache_enabled, true},
    {"--no-tile-layout-cache", &ConfigOptions::tile_layout_cache_enabled, false},
    {"--binding-reuse", &ConfigOptions::binding_reuse_enabled, true},
    {"--no-binding-reuse", &ConfigOptions::binding_reuse_enabled, false},
    {"--fast-pending-ops", &ConfigOptions::fast_pending_ops_enabled, true},
    {"--no-fast-pending-ops", &ConfigOptions::fast_pending_ops_enabled, false},
    {"--priority-spin-wait", &ConfigOptions::priority_spin_wait_enabled, true},
    {"--no-priority-spin-wait", &ConfigOptions::priority_spin_wait_enabled, false},
    {"--cp-sequencer", &ConfigOptions::cp_sequencer_enabled, true},
    {"--no-cp-sequencer", &ConfigOptions::cp_sequencer_enabled, false},
    {"--draw-prep", &ConfigOptions::draw_prep_enabled, true},
    {"--no-draw-prep", &ConfigOptions::draw_prep_enabled, false},
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    {"--redzone", &ConfigOptions::red_zone_protection_enabled},
#endif
};

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}
		if (arg == "--no-fps-limit") {
			Libs::Graphics::g_host_fps_limit.store(0, std::memory_order_relaxed);
			continue;
		}

		if (const auto* flag = std::ranges::find(FLAGS, arg, &Flag::name);
		    flag != std::end(FLAGS)) {
			options.config.*flag->field = flag->value;
			continue;
		}

		if (!arg.starts_with("--")) {
			::printf("game input must be provided with --game\n");
			return false;
		}

		if (!NextArg(argc, argv, i, value)) {
			::printf("missing value for %s\n", arg.c_str());
			return false;
		}

		if (arg == "--game") {
			if (!options.app0_dir.empty()) {
				::printf("--game can only be specified once\n");
				return false;
			}

			value           = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (Common::File::IsDirectoryExisting(path)) {
				options.app0_dir = path;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::IsSupportedArchive(path) && Common::File::IsFileExisting(path)) {
				const auto root = Common::MakeArchivePath(path);
				if (!Common::File::IsFileExisting(root / "eboot.bin")) {
					::printf("Archive does not contain eboot.bin: %s\n", value.c_str());
					return false;
				}
				options.app0_dir = root;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::File::IsFileExisting(path)) {
				options.app0_dir = path.parent_path();

				if (options.app0_dir.empty()) {
					options.app0_dir = ".";
				}

				options.elf = std::filesystem::path("/app0") / path.filename();
			} else {
				::printf("--game must point to an existing directory, ELF, or archive: %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--guest-exec") {
			options.elf = Common::PathFromUtf8(Common::FixFilenameSlash(value));
			options.config.guest_arguments.assign(argv + i + 1, argv + argc);
			break;
		} else if (arg == "--game-patch") {
			if (!options.game_patch.empty()) {
				::printf("--game-patch can only be specified once\n");
				return false;
			}
			value           = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (!Common::File::IsFileExisting(path)) {
				::printf("--game-patch must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.game_patch = path;
		} else if (arg == "--addcont-dir") {
			if (!options.addcont_dir.empty()) {
				::printf("--addcont-dir can only be specified once\n");
				return false;
			}
			options.addcont_dir = Common::PathFromUtf8(Common::FixFilenameSlash(value));
		} else if (arg == "--addcont") {
			auto entry = Common::ParseAddContent(value);
			if (!entry) {
				::printf("--addcont needs <16-character A-Z/0-9 label>[=<existing folder>]: %s\n",
				         value.c_str());
				return false;
			}
			options.addcont.push_back(std::move(*entry));
		} else if (arg == "--screen-width") {
			if (!ParseUint32(value, options.config.screen_width) ||
			    options.config.screen_width == 0) {
				::printf("invalid screen width: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--screen-height") {
			if (!ParseUint32(value, options.config.screen_height) ||
			    options.config.screen_height == 0) {
				::printf("invalid screen height: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--user-name") {
			if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
				::printf("invalid user name: must contain 1-%zu bytes\n",
				         Config::MAX_USER_NAME_LENGTH);
				return false;
			}
			options.config.user_name = value;
		} else if (arg == "--launch-activity") {
			if (value.empty() || value.size() > 32 ||
			    !std::ranges::all_of(value, [](char c) { return c >= ' ' && c <= '~'; })) {
				::printf("invalid activity ID: must contain 1-32 printable ASCII characters\n");
				return false;
			}
			options.config.launch_activity = value;
		} else if (arg == "--user-id") {
			if (!ParseUserId(value, options.config.user_id)) {
				::printf("invalid user ID: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--mic") {
			options.config.audio_input_device = value;
		} else if (arg == "--controller-color") {
			Config::ControllerColor color {};
			if (!ParseControllerColor(value, color)) {
				::printf("invalid controller color (expected #RRGGBB): %s\n", value.c_str());
				return false;
			}
			options.config.controller_color = color;
		} else if (arg == "--controller-volume") {
			if (!ParseUint32(value, options.config.controller_speaker_volume) ||
			    options.config.controller_speaker_volume > 100) {
				::printf("invalid controller volume: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--controller-vibration") {
			if (!ParseUint32(value, options.config.controller_vibration_intensity) ||
			    options.config.controller_vibration_intensity > 100) {
				::printf("invalid controller vibration intensity: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--keyboard-player") {
			if (!ParseUint32(value, options.config.keyboard_player) ||
			    options.config.keyboard_player > static_cast<uint32_t>(Config::MAX_PLAYERS)) {
				::printf("invalid keyboard player: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--fps-limit") {
			uint32_t fps = 0;
			if (!ParseUint32(value, fps) || fps > 360) {
				::printf("invalid host FPS limit: %s\n", value.c_str());
				return false;
			}
			Libs::Graphics::g_host_fps_limit.store(fps, std::memory_order_relaxed);
		} else if (arg == "--present-mode") {
			if (!ParseEnum(value, options.config.present_mode)) {
				::printf("invalid present mode: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--pssr-upscaler") {
			if (!ParseEnum(value, options.config.pssr_upscaler)) {
				::printf("invalid PSSR upscaler: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--dlss-folder") {
			options.config.dlss_folder = Common::PathFromUtf8(value);
		} else if (arg == "--dlss-preset") {
			if (value.size() != 1 || value[0] < 'A' || value[0] > 'O') {
				::printf("invalid DLSS preset: %s\n", value.c_str());
				return false;
			}
			options.config.dlss_preset = static_cast<uint32_t>(value[0] - 'A' + 1);
		} else if (arg == "--fsr-folder") {
			options.config.fsr_folder = Common::PathFromUtf8(value);
		} else if (arg == "--gpu") {
			if (!ParseInt32(value, options.config.gpu_index)) {
				::printf("invalid gpu index: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vblank-frequency") {
			if (!ParseUint32(value, options.config.vblank_frequency)) {
				::printf("invalid vblank frequency: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--flush-draws") {
			if (!ParseUint32(value, options.config.flush_draws)) {
				::printf("invalid draw count: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--trophy-duration") {
			if (!ParseUint32(value, options.config.trophy_duration_seconds)) {
				::printf("invalid trophy duration: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-direction") {
			if (!ParseEnum(value, options.config.shader_log_direction)) {
				::printf("invalid shader log direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-folder") {
			options.config.shader_log_folder = Common::PathFromUtf8(value);
		} else if (arg == "--command-buffer-dump-folder") {
			options.config.command_buffer_dump_folder = Common::PathFromUtf8(value);
		} else if (arg == "--printf-direction") {
			if (!ParseEnum(value, options.config.printf_direction)) {
				::printf("invalid printf direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--printf-output-file") {
			options.config.printf_output_file = Common::PathFromUtf8(value);
		} else if (arg == "--audit-export") {
			options.config.audit_export_path = value;
		} else if (arg == "--audit-capture-delay") {
			if (!ParseUint32(value, options.config.audit_capture_delay_sec)) {
				::printf("invalid audit capture delay: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--audit-capture-duration") {
			if (!ParseUint32(value, options.config.audit_capture_duration_sec)) {
				::printf("invalid audit capture duration: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu-submit-cadence-ms") {
			double ms = 0.0;
			if (!ParseDouble(value, ms) || ms <= 0.0) {
				::printf("invalid cadence ms: %s\n", value.c_str());
				return false;
			}
			options.config.gpu_submit_cadence_interval_us = static_cast<uint64_t>(ms * 1000.0);
		} else if (arg == "--gpu-submit-cadence-interval-us") {
			if (!ParseUint64(value, options.config.gpu_submit_cadence_interval_us) ||
			    options.config.gpu_submit_cadence_interval_us == 0) {
				::printf("invalid cadence interval us: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--priority-spin-duration-us") {
			if (!ParseUint32(value, options.config.priority_spin_duration_us)) {
				::printf("invalid priority spin duration us: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--cp-sequencer-spin-us") {
			if (!ParseUint32(value, options.config.cp_sequencer_spin_us)) {
				::printf("invalid cp sequencer spin duration us: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--draw-prep-workers") {
			if (!ParseUint32(value, options.config.draw_prep_workers) ||
			    options.config.draw_prep_workers == 0 || options.config.draw_prep_workers > 32) {
				::printf("invalid workers for --draw-prep-workers: %s (must be 1-32)\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--keymap") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid keymap: %s\n", value.c_str());
				return false;
			}
			options.config.keymap.push_back(value);
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}

static int Main(int argc, char* argv[]) {
	InitializeThreads();

	RunOptions options;
	bool       show_help = false;

	if (argc < 2) {
		PrintUsage();
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help)) {
		PrintUsage();
		return 1;
	}

	if (show_help) {
		PrintUsage();
		return 0;
	}

	const auto fps_limit = Libs::Graphics::g_host_fps_limit.load(std::memory_order_relaxed);
	if (fps_limit != 0) {
		::printf("Host presentation limit: %u FPS (guest clocks unchanged)\n", fps_limit);
	}
	Run(options);

	return 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

int wmain(int argc, wchar_t* argv[]) {
	std::vector<std::string> utf8_args;
	utf8_args.reserve(static_cast<size_t>(argc));

	for (int index = 0; index < argc; index++) {
		const std::wstring_view wide(argv[index]);
		const std::u16string    utf16(wide.begin(), wide.end());

		utf8_args.push_back(Common::Utf16ToUtf8(utf16));
	}

	std::vector<char*> utf8_argv;
	utf8_argv.reserve(utf8_args.size());

	for (auto& argument: utf8_args) {
		utf8_argv.push_back(argument.data());
	}

	return Main(argc, utf8_argv.data());
}

#else

int main(int argc, char* argv[]) {
	return Main(argc, argv);
}

#endif
