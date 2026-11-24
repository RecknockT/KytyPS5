#include "common/auditProfiler.h"

#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "kytyGitVersion.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace Common::AuditProfiler {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t PAGE_TABLE_CAPACITY   = 65536;
constexpr size_t PAGE_TABLE_MASK       = PAGE_TABLE_CAPACITY - 1;
constexpr size_t RIP_TABLE_CAPACITY    = 4096;
constexpr size_t RIP_TABLE_MASK        = RIP_TABLE_CAPACITY - 1;
constexpr size_t THREAD_TABLE_CAPACITY = 128;
constexpr size_t THREAD_TABLE_MASK     = THREAD_TABLE_CAPACITY - 1;
constexpr size_t MAX_PAGE_PROBE        = 32;
constexpr size_t MAX_PROBE             = 16;
constexpr size_t MAX_CAPTURED_FRAMES   = 65536;

// Independent counters: audit Reset/StartCapture cannot erase a live interval.
std::atomic<bool> g_live_enabled {false};
enum LiveCounter : size_t {
	LiveFrames,
	LiveFrameNs,
	LiveVblanks,
	LiveSubmitted,
	LiveCompleted,
	LiveAudioNs,
	LiveAudioBatches,
	LiveHitch50,
	LiveHitch100,
	LiveHitch250,
	LiveCount
};
std::array<std::atomic<uint64_t>, LiveCount> g_live_counts {};
std::atomic<uint64_t>                        g_live_last_frame_ns {0}, g_live_max_frame_ns {0};
std::array<std::atomic<uint64_t>, static_cast<size_t>(GpuWaitReason::Count)> g_live_wait_ns {};
std::array<std::atomic<uint64_t>, static_cast<size_t>(PipelineStage::Count)> g_live_stage_ns {};

uint64_t LiveNowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
	    .count();
}

// Constructed on first use, after the counters. Joined before their destruction.
std::jthread& LiveWriter() {
	static std::jthread writer;
	return writer;
}

struct alignas(64) PageSlot {
	std::atomic<uint64_t> page {0};
	std::atomic<uint64_t> faults {0};
	std::atomic<uint64_t> reprotects {0};
};

struct alignas(64) RipSlot {
	std::atomic<uint64_t> rip {0};
	std::atomic<uint64_t> count {0};
};

struct alignas(64) ThreadSlot {
	std::atomic<uint32_t> tid {0};
	std::atomic<uint64_t> count {0};
	char                  name[32] = {};
};

std::atomic<bool> g_enabled {false};
std::string       g_export_path = "audit_profile.json";
std::mutex        g_config_mutex;

// Capture session management
std::atomic<CaptureState>   g_capture_state {CaptureState::Idle};
std::atomic<ExecutionPhase> g_current_phase {ExecutionPhase::Initialization};
std::string                 g_trigger_reason;
std::atomic<uint32_t>       g_capture_delay_sec {0};
std::atomic<uint32_t>       g_capture_duration_sec {0};
std::string                 g_capture_id;
Clock::time_point           g_session_start_time;
Clock::time_point           g_capture_start_time;
Clock::time_point           g_capture_end_time;
std::mutex                  g_capture_mutex;
ProfileSnapshot             g_completed_snapshot {};
std::atomic<bool>           g_has_completed_snapshot {false};

// Phase-separated tracking counters
struct PhaseCounters {
	std::atomic<uint64_t> frame_count {0};
	std::atomic<uint64_t> total_frame_ns {0};
	std::atomic<uint64_t> faults {0};
	std::atomic<uint64_t> reprotects {0};
	std::atomic<uint64_t> submits {0};
	std::atomic<uint64_t> gpu_wait_ns {0};
};
PhaseCounters g_phase_loading {};
PhaseCounters g_phase_gameplay {};

// Baseline counters sampled at StartCapture()
std::atomic<uint64_t>                g_baseline_buffer_upload_bytes {0};
std::atomic<uint64_t>                g_baseline_texture_upload_bytes {0};
std::atomic<uint64_t>                g_baseline_buffer_download_bytes {0};
std::atomic<uint64_t>                g_baseline_neighbour_downloads {0};
std::atomic<uint64_t>                g_baseline_neighbour_download_bytes {0};
std::array<std::atomic<uint64_t>, 5> g_baseline_binding_reuses {};
std::atomic<uint64_t> g_baseline_tile_layout_hits {0}, g_baseline_tile_layout_misses {0};
std::atomic<uint64_t> g_baseline_scratch_freed_bytes {0};
std::atomic<uint64_t> g_baseline_scratch_retirements {0};
std::atomic<uint64_t> g_baseline_scratch_allocations {0};
std::atomic<uint64_t> g_baseline_compact_readback_copies {0};
std::atomic<uint64_t> g_baseline_compact_readback_fallbacks {0};
std::atomic<uint64_t> g_baseline_readback_prefill_avoided_bytes {0};
std::atomic<uint64_t> g_baseline_readback_staging_saved_bytes {0};
std::atomic<uint64_t> g_baseline_read_buffer_hits {0}, g_baseline_read_buffer_misses {0};
std::atomic<uint64_t> g_baseline_resource_prep_hits {0}, g_baseline_resource_prep_misses {0};
std::atomic<uint64_t> g_baseline_texture_download_bytes {0};
std::atomic<uint64_t> g_baseline_texture_partial_refreshes {0};
std::atomic<uint64_t> g_baseline_texture_partial_fallbacks {0};
std::atomic<uint64_t> g_baseline_texture_partial_upload_bytes {0};
std::atomic<uint64_t> g_baseline_cadence_held_back {0};
std::atomic<uint64_t> g_baseline_cadence_submits {0};
std::atomic<uint64_t> g_baseline_draws {0};
std::atomic<uint64_t> g_baseline_draw_instances {0};
std::atomic<uint64_t> g_baseline_indirect_draws {0};
std::atomic<uint64_t> g_baseline_dispatches {0};
std::atomic<uint64_t> g_baseline_pipelines_created {0};
std::atomic<uint64_t> g_baseline_shaders_compiled {0};
std::atomic<uint64_t> g_baseline_gpu_thread_busy_ns {0};
std::atomic<uint64_t> g_baseline_pipeline_cache_lookups {0};
std::atomic<uint64_t> g_baseline_pipeline_cache_hits {0};
std::atomic<uint64_t> g_baseline_redundant_pipeline_binds {0};
std::atomic<uint64_t> g_baseline_redundant_vertex_binds {0};
std::atomic<uint64_t> g_baseline_redundant_index_binds {0};

std::atomic<uint64_t> g_priority_wait_spins {0};
std::atomic<uint64_t> g_priority_wait_spin_hits {0};
std::atomic<uint64_t> g_pending_ops_fast_skips {0};
std::atomic<uint64_t> g_baseline_priority_wait_spins {0};
std::atomic<uint64_t> g_baseline_priority_wait_spin_hits {0};
std::atomic<uint64_t> g_baseline_pending_ops_fast_skips {0};

std::atomic<uint64_t> g_cp_sequencer_ops_emitted {0};
std::atomic<uint64_t> g_cp_sequencer_lockstep_waits {0};
std::atomic<uint64_t> g_cp_sequencer_snapshots_taken {0};
std::atomic<uint64_t> g_cp_sequencer_waits_spin {0};
std::atomic<uint64_t> g_baseline_cp_sequencer_ops_emitted {0};
std::atomic<uint64_t> g_baseline_cp_sequencer_lockstep_waits {0};
std::atomic<uint64_t> g_baseline_cp_sequencer_snapshots_taken {0};
std::atomic<uint64_t> g_baseline_cp_sequencer_waits_spin {0};

std::atomic<uint64_t> g_draw_prep_published {0};
std::atomic<uint64_t> g_draw_prep_prepared {0};
std::atomic<uint64_t> g_draw_prep_committed {0};
std::atomic<uint64_t> g_draw_prep_fallback {0};
std::atomic<uint64_t> g_draw_prep_avoided {0};
std::atomic<uint64_t> g_draw_prep_avoided_time_ns {0};
std::atomic<uint64_t> g_baseline_draw_prep_published {0};
std::atomic<uint64_t> g_baseline_draw_prep_prepared {0};
std::atomic<uint64_t> g_baseline_draw_prep_committed {0};
std::atomic<uint64_t> g_baseline_draw_prep_fallback {0};
std::atomic<uint64_t> g_baseline_draw_prep_avoided {0};
std::atomic<uint64_t> g_baseline_draw_prep_avoided_time_ns {0};

// Tripartite workload distribution: queue latency accumulators
std::array<std::atomic<uint64_t>, static_cast<size_t>(QueueLatencyType::Count)>
    g_queue_latency_ns {};
std::array<std::atomic<uint64_t>, static_cast<size_t>(QueueLatencyType::Count)>
    g_queue_latency_count {};

// Global lock-free tables for the active captured window
PageSlot   g_page_table[PAGE_TABLE_CAPACITY];
RipSlot    g_rip_table[RIP_TABLE_CAPACITY];
ThreadSlot g_thread_table[THREAD_TABLE_CAPACITY];

// Fault timing
std::atomic<uint64_t> g_total_faults {0};
std::atomic<uint64_t> g_total_reprotects {0};
std::atomic<uint64_t> g_total_handler_ns {0};
std::atomic<uint64_t> g_max_handler_ns {0};

// Timeline semaphore wait stats
std::atomic<uint64_t> g_wait_counts[static_cast<size_t>(GpuWaitReason::Count)] {};
std::atomic<uint64_t> g_wait_nanos[static_cast<size_t>(GpuWaitReason::Count)] {};
std::atomic<uint64_t> g_wait_max_nanos[static_cast<size_t>(GpuWaitReason::Count)] {};

// Submits by reason
std::atomic<uint64_t> g_submit_counts[static_cast<size_t>(SubmitReason::Count)] {};

// Internal GPU transfers
std::atomic<uint64_t> g_internal_gpu_transfer_bytes {0};

// Frametimes buffer for exact percentiles
alignas(64) double g_captured_frames[MAX_CAPTURED_FRAMES] {};
std::atomic<uint64_t> g_captured_frame_count {0};

// Stuttering and hitch metrics
std::atomic<uint64_t> g_hitches_50ms {0};
std::atomic<uint64_t> g_hitches_100ms {0};
std::atomic<uint64_t> g_hitches_250ms {0};
std::atomic<uint64_t> g_hitches_500ms {0};
std::atomic<uint64_t> g_hitches_1000ms {0};
std::atomic<uint64_t> g_hitch_time_ns {0};

// Timing and presentation metrics
std::atomic<uint64_t> g_vblank_count {0};
std::atomic<uint64_t> g_flip_submitted_count {0};
std::atomic<uint64_t> g_flip_completed_count {0};
std::atomic<uint64_t> g_flip_wait_ns {0};
std::atomic<uint64_t> g_flip_wait_count {0};
std::atomic<uint64_t> g_audio_updates_count {0};
std::atomic<uint64_t> g_audio_total_samples {0};

// Stage 8 additions: Pipeline stage timings
std::atomic<uint64_t> g_stage_counts[static_cast<size_t>(PipelineStage::Count)] {};
std::atomic<uint64_t> g_stage_nanos[static_cast<size_t>(PipelineStage::Count)] {};
std::atomic<uint64_t> g_stage_max_nanos[static_cast<size_t>(PipelineStage::Count)] {};
std::atomic<uint64_t> g_stage_min_nanos[static_cast<size_t>(PipelineStage::Count)] {};

// Stage 8 additions: StreamBuffer audit
std::atomic<uint64_t> g_stream_buffer_allocs {0};
std::atomic<uint64_t> g_stream_buffer_wraps {0};
std::atomic<uint64_t> g_stream_buffer_bytes {0};
std::atomic<uint64_t> g_stream_buffer_high_water {0};
std::atomic<uint64_t> g_stream_buffer_wait_events {0};
std::atomic<uint64_t> g_stream_buffer_wait_total_ns {0};
std::atomic<uint64_t> g_stream_buffer_wait_max_ns {0};
std::atomic<uint64_t> g_stream_buffer_last_wait_tick {0};

// Stage 8 additions: CPU / GPU separation
std::atomic<uint64_t> g_gpu_thread_cpu_start_ns {0};
std::atomic<uint64_t> g_gpu_thread_cpu_end_ns {0};
std::atomic<uint64_t> g_gpu_thread_wall_start_ns {0};
std::atomic<uint64_t> g_gpu_thread_wall_end_ns {0};
std::atomic<uint64_t> g_gpu_execution_total_ns {0};
std::atomic<uint64_t> g_gpu_execution_count {0};

// Stage 8 additions: Trace event circular ring buffer
constexpr size_t      TRACE_EVENT_RING_CAPACITY = 16384;
TraceEvent            g_trace_event_ring[TRACE_EVENT_RING_CAPACITY] {};
std::atomic<uint64_t> g_trace_event_cursor {0};

struct AtomicSlowFrame {
	std::atomic<uint64_t> frame_index {0};
	std::atomic<double>   frame_ms {0.0};
	std::atomic<uint64_t> timestamp_ms {0};
};
constexpr size_t    MAX_SLOW_FRAMES = 32;
AtomicSlowFrame     g_slow_frames[MAX_SLOW_FRAMES] {};
std::atomic<size_t> g_slow_frame_cursor {0};

uint32_t GetCurrentTid() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint32_t>(GetCurrentThreadId());
#elif defined(__APPLE__)
	uint64_t tid = 0;
	pthread_threadid_np(nullptr, &tid);
	return static_cast<uint32_t>(tid);
#elif defined(__linux__)
	return static_cast<uint32_t>(gettid());
#else
	return 0;
#endif
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static HANDLE g_gpu_thread_handle = nullptr;

uint64_t GetThreadCpuTimeNsFromHandle(HANDLE handle) noexcept {
	if (handle == nullptr) {
		return 0;
	}
	FILETIME creation {}, exit {}, kernel {}, user {};
	if (::GetThreadTimes(handle, &creation, &exit, &kernel, &user)) {
		ULARGE_INTEGER k, u;
		k.LowPart  = kernel.dwLowDateTime;
		k.HighPart = kernel.dwHighDateTime;
		u.LowPart  = user.dwLowDateTime;
		u.HighPart = user.dwHighDateTime;
		return (k.QuadPart + u.QuadPart) * 100ULL;
	}
	return 0;
}

uint64_t GetCurrentThreadCpuTimeNs() noexcept {
	return GetThreadCpuTimeNsFromHandle(::GetCurrentThread());
}
#else
uint64_t GetCurrentThreadCpuTimeNs() noexcept {
	return 0;
}
#endif

uint64_t SampleGpuThreadCpuTimeNs() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (g_gpu_thread_handle != nullptr) {
		const auto ns = GetThreadCpuTimeNsFromHandle(g_gpu_thread_handle);
		if (ns > 0) {
			return ns;
		}
	}
#endif
	return GetCurrentThreadCpuTimeNs();
}

struct FrameTimeSeriesRecord {
	uint32_t frame_index            = 0;
	float    frame_ms               = 0.0f;
	uint8_t  phase                  = 0;
	uint32_t draws                  = 0;
	uint32_t dispatches             = 0;
	uint32_t submits                = 0;
	uint32_t pm4_decode_us          = 0;
	uint32_t pm4_dispatch_us        = 0;
	uint32_t render_state_prep_us   = 0;
	uint32_t descriptor_prep_us     = 0;
	uint32_t pipeline_lookup_us     = 0;
	uint32_t pipeline_key_build_us  = 0;
	uint32_t buffer_prep_us         = 0;
	uint32_t command_recording_us   = 0;
	uint32_t barriers_us            = 0;
	uint32_t sync_waits_us          = 0;
	uint32_t write_faults           = 0;
	uint32_t stream_buffer_alloc_kb = 0;
	uint32_t stream_buffer_wait_us  = 0;
};

std::vector<FrameTimeSeriesRecord> g_frame_timeseries;
std::mutex                         g_frame_timeseries_mutex;

struct LastFrameCounters {
	uint64_t draws                 = 0;
	uint64_t dispatches            = 0;
	uint64_t submits               = 0;
	uint64_t write_faults          = 0;
	uint64_t stream_buffer_bytes   = 0;
	uint64_t stream_buffer_wait_ns = 0;
	uint64_t stage_nanos[static_cast<size_t>(PipelineStage::Count)] {};
};
LastFrameCounters g_last_frame_counters {};

void UpdateMax(std::atomic<uint64_t>& target, uint64_t value) noexcept {
	auto current = target.load(std::memory_order_relaxed);
	while (value > current &&
	       !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
	}
}

void UpdateMin(std::atomic<uint64_t>& target, uint64_t value) noexcept {
	auto current = target.load(std::memory_order_relaxed);
	while ((current == 0 || value < current) &&
	       !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
	}
}

} // namespace

bool IsEnabled() noexcept {
	return g_enabled.load(std::memory_order_relaxed);
}

void StopLiveDiagnostics() {
	g_live_enabled.store(false, std::memory_order_relaxed);
	auto& writer = LiveWriter();
	if (writer.joinable()) {
		writer.request_stop();
		writer.join();
	}
}

bool StartLiveDiagnostics(const std::string& path) {
	StopLiveDiagnostics();
	std::ofstream output(path, std::ios::out | std::ios::trunc);
	if (!output) return false;
	for (auto& value: g_live_counts)
		value.store(0, std::memory_order_relaxed);
	for (auto& value: g_live_wait_ns)
		value.store(0, std::memory_order_relaxed);
	for (auto& value: g_live_stage_ns)
		value.store(0, std::memory_order_relaxed);
	g_live_last_frame_ns.store(0, std::memory_order_relaxed);
	g_live_max_frame_ns.store(0, std::memory_order_relaxed);
	output << nlohmann::json(
	              {{"type", "metadata"},
	               {"schema", 1},
	               {"interval_seconds", 1},
	               {"simulation_speed_measured", false},
	               {"notes",
	                "Flip/audio rates are not physics speed. Waits are thread-accumulated; stages "
	                "can overlap. Last incomplete line may be lost on crash."}})
	              .dump()
	       << '\n';
	output.flush();
	if (!output) return false;
	g_live_enabled.store(true, std::memory_order_relaxed);
	LiveWriter() = std::jthread([out = std::move(output)](std::stop_token stop) mutable {
		{
			using Counter = DebugCounters::Counter;
			constexpr std::array                  counters {Counter::Draws,
			                                                Counter::Dispatches,
			                                                Counter::ShadersCompiled,
			                                                Counter::PipelinesCreated,
			                                                Counter::BufferUploadBytes,
			                                                Counter::BufferDownloadBytes,
			                                                Counter::TextureUploadBytes,
			                                                Counter::TextureDownloadBytes,
			                                                Counter::GpuFaults};
			constexpr std::array                  names {"draws",
			                                             "dispatches",
			                                             "shaders_compiled",
			                                             "pipelines_created",
			                                             "buffer_upload_bytes",
			                                             "buffer_download_bytes",
			                                             "texture_upload_bytes",
			                                             "texture_download_bytes",
			                                             "write_faults"};
			std::array<uint64_t, LiveCount>       previous {};
			std::array<uint64_t, counters.size()> previous_debug {};
			for (size_t i = 0; i < counters.size(); ++i)
				previous_debug[i] = DebugCounters::Get(counters[i]);
			const auto started = LiveNowNs();
			auto       last    = started;
			do {
				for (int i = 0; i < 10 && !stop.stop_requested(); ++i)
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				const auto   now     = LiveNowNs();
				const double seconds = static_cast<double>(now - last) / 1e9;
				if (seconds <= 0) continue;
				std::array<uint64_t, LiveCount> current {}, delta {};
				for (size_t i = 0; i < current.size(); ++i) {
					current[i] = g_live_counts[i].load(std::memory_order_relaxed);
					delta[i]   = current[i] - previous[i];
				}
				const auto     last_frame = g_live_last_frame_ns.load(std::memory_order_relaxed);
				nlohmann::json row {
				    {"type", stop.stop_requested() ? "final" : "sample"},
				    {"elapsed_seconds", static_cast<double>(now - started) / 1e9},
				    {"interval_seconds", seconds},
				    {"frame_intervals", delta[LiveFrames]},
				    {"rendered_interval_fps", delta[LiveFrames] / seconds},
				    {"completed_flip_hz", delta[LiveCompleted] / seconds},
				    {"submitted_flip_hz", delta[LiveSubmitted] / seconds},
				    {"vblank_hz", delta[LiveVblanks] / seconds},
				    {"completed_flips_total", current[LiveCompleted]},
				    {"hitches_50ms", delta[LiveHitch50]},
				    {"hitches_100ms", delta[LiveHitch100]},
				    {"hitches_250ms", delta[LiveHitch250]},
				    {"worst_frame_ms",
				     g_live_max_frame_ns.exchange(0, std::memory_order_relaxed) / 1e6},
				    {"audio_media_seconds", delta[LiveAudioNs] / 1e9},
				    {"audio_batches", delta[LiveAudioBatches]},
				    {"debug_counters_enabled", DebugCounters::Enabled()},
				    {"phase", GetExecutionPhaseName(GetExecutionPhase())},
				    {"gpu_submit_cadence", Config::GpuSubmitCadenceEnabled()},
				    {"simulation_speed_measured", false}};
				row["average_frame_ms"] =
				    delta[LiveFrames] ? nlohmann::json(delta[LiveFrameNs] / 1e6 / delta[LiveFrames])
				                      : nlohmann::json(nullptr);
				row["seconds_without_frame"] =
				    static_cast<double>(now - (last_frame ? last_frame : started)) / 1e9;
				row["no_frame_seen"] = last_frame == 0;
				for (size_t i = 0; i < counters.size(); ++i) {
					const auto value      = DebugCounters::Get(counters[i]);
					row["work"][names[i]] = value >= previous_debug[i]
					                            ? nlohmann::json(value - previous_debug[i])
					                            : nlohmann::json(nullptr);
					previous_debug[i]     = value;
				}
				for (size_t i = 0; i < g_live_wait_ns.size(); ++i)
					row["thread_accumulated_wait_ms"]
					   [GetWaitReasonName(static_cast<GpuWaitReason>(i))] =
					       g_live_wait_ns[i].exchange(0, std::memory_order_relaxed) / 1e6;
				for (size_t i = 0; i < g_live_stage_ns.size(); ++i)
					row["host_stage_ms"][GetPipelineStageName(static_cast<PipelineStage>(i))] =
					    g_live_stage_ns[i].exchange(0, std::memory_order_relaxed) / 1e6;
				out << row.dump() << '\n';
				out.flush(); // Complete records remain readable without an ordinary shutdown.
				if (!out) break;
				previous = current;
				last     = now;
			} while (!stop.stop_requested());
		}
		g_live_enabled.store(false, std::memory_order_relaxed);
	});
	return true;
}

void SetEnabled(bool enabled) noexcept {
	g_enabled.store(enabled, std::memory_order_relaxed);
	if (enabled) {
		if (g_session_start_time == Clock::time_point {}) {
			g_session_start_time = Clock::now();
		}
		if (g_capture_state.load(std::memory_order_relaxed) == CaptureState::Idle) {
			g_capture_state.store(CaptureState::Running, std::memory_order_relaxed);
			g_capture_start_time = Clock::now();
		}
	} else {
		g_capture_state.store(CaptureState::Idle, std::memory_order_relaxed);
	}
}

void SetExportPath(std::string_view path) {
	std::lock_guard lock(g_config_mutex);
	g_export_path = path.empty() ? "audit_profile.json" : std::string(path);
}

const std::string& GetExportPath() {
	std::lock_guard lock(g_config_mutex);
	return g_export_path;
}

CaptureState GetCaptureState() noexcept {
	return g_capture_state.load(std::memory_order_relaxed);
}

bool IsCaptureRunning() noexcept {
	return g_capture_state.load(std::memory_order_relaxed) == CaptureState::Running;
}

uint64_t GetCaptureDurationMs() noexcept {
	const auto state = g_capture_state.load(std::memory_order_relaxed);
	if (state == CaptureState::Running) {
		const auto now = Clock::now();
		return static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(now - g_capture_start_time)
		        .count());
	}
	if (state == CaptureState::Completed || state == CaptureState::Interrupted) {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		                                 g_capture_end_time - g_capture_start_time)
		                                 .count());
	}
	return 0;
}

void Reset() {
	g_total_faults.store(0, std::memory_order_relaxed);
	g_total_reprotects.store(0, std::memory_order_relaxed);
	g_total_handler_ns.store(0, std::memory_order_relaxed);
	g_max_handler_ns.store(0, std::memory_order_relaxed);

	for (auto& slot: g_page_table) {
		slot.page.store(0, std::memory_order_relaxed);
		slot.faults.store(0, std::memory_order_relaxed);
		slot.reprotects.store(0, std::memory_order_relaxed);
	}
	for (auto& slot: g_rip_table) {
		slot.rip.store(0, std::memory_order_relaxed);
		slot.count.store(0, std::memory_order_relaxed);
	}
	for (auto& slot: g_thread_table) {
		slot.tid.store(0, std::memory_order_relaxed);
		slot.count.store(0, std::memory_order_relaxed);
		slot.name[0] = '\0';
	}
	for (auto& count: g_wait_counts) {
		count.store(0, std::memory_order_relaxed);
	}
	for (auto& nanos: g_wait_nanos) {
		nanos.store(0, std::memory_order_relaxed);
	}
	for (auto& max_ns: g_wait_max_nanos) {
		max_ns.store(0, std::memory_order_relaxed);
	}
	for (auto& count: g_submit_counts) {
		count.store(0, std::memory_order_relaxed);
	}
	g_internal_gpu_transfer_bytes.store(0, std::memory_order_relaxed);
	g_captured_frame_count.store(0, std::memory_order_relaxed);
	g_hitches_50ms.store(0, std::memory_order_relaxed);
	g_hitches_100ms.store(0, std::memory_order_relaxed);
	g_hitches_250ms.store(0, std::memory_order_relaxed);
	g_hitches_500ms.store(0, std::memory_order_relaxed);
	g_hitches_1000ms.store(0, std::memory_order_relaxed);
	g_hitch_time_ns.store(0, std::memory_order_relaxed);
	g_slow_frame_cursor.store(0, std::memory_order_relaxed);
	for (auto& sf: g_slow_frames) {
		sf.frame_index.store(0, std::memory_order_relaxed);
		sf.frame_ms.store(0.0, std::memory_order_relaxed);
		sf.timestamp_ms.store(0, std::memory_order_relaxed);
	}

	g_phase_loading.frame_count.store(0, std::memory_order_relaxed);
	g_phase_loading.total_frame_ns.store(0, std::memory_order_relaxed);
	g_phase_loading.faults.store(0, std::memory_order_relaxed);
	g_phase_loading.reprotects.store(0, std::memory_order_relaxed);
	g_phase_loading.submits.store(0, std::memory_order_relaxed);
	g_phase_loading.gpu_wait_ns.store(0, std::memory_order_relaxed);

	g_phase_gameplay.frame_count.store(0, std::memory_order_relaxed);
	g_phase_gameplay.total_frame_ns.store(0, std::memory_order_relaxed);
	g_phase_gameplay.faults.store(0, std::memory_order_relaxed);
	g_phase_gameplay.reprotects.store(0, std::memory_order_relaxed);
	g_phase_gameplay.submits.store(0, std::memory_order_relaxed);
	g_phase_gameplay.gpu_wait_ns.store(0, std::memory_order_relaxed);
	g_vblank_count.store(0, std::memory_order_relaxed);
	g_flip_submitted_count.store(0, std::memory_order_relaxed);
	g_flip_completed_count.store(0, std::memory_order_relaxed);
	g_flip_wait_ns.store(0, std::memory_order_relaxed);
	g_flip_wait_count.store(0, std::memory_order_relaxed);
	g_audio_updates_count.store(0, std::memory_order_relaxed);
	g_audio_total_samples.store(0, std::memory_order_relaxed);

	// Stage 8 additions: Reset pipeline stage timings
	for (auto& count: g_stage_counts)
		count.store(0, std::memory_order_relaxed);
	for (auto& nanos: g_stage_nanos)
		nanos.store(0, std::memory_order_relaxed);
	for (auto& max_ns: g_stage_max_nanos)
		max_ns.store(0, std::memory_order_relaxed);
	for (auto& min_ns: g_stage_min_nanos)
		min_ns.store(0, std::memory_order_relaxed);

	// Stage 8 additions: Reset StreamBuffer audit
	g_stream_buffer_allocs.store(0, std::memory_order_relaxed);
	g_stream_buffer_wraps.store(0, std::memory_order_relaxed);
	g_stream_buffer_bytes.store(0, std::memory_order_relaxed);
	g_stream_buffer_high_water.store(0, std::memory_order_relaxed);
	g_stream_buffer_wait_events.store(0, std::memory_order_relaxed);
	g_stream_buffer_wait_total_ns.store(0, std::memory_order_relaxed);
	g_stream_buffer_wait_max_ns.store(0, std::memory_order_relaxed);
	g_stream_buffer_last_wait_tick.store(0, std::memory_order_relaxed);

	// Stage 8 additions: Reset CPU/GPU separation
	g_gpu_thread_cpu_start_ns.store(0, std::memory_order_relaxed);
	g_gpu_thread_cpu_end_ns.store(0, std::memory_order_relaxed);
	g_gpu_thread_wall_start_ns.store(0, std::memory_order_relaxed);
	g_gpu_thread_wall_end_ns.store(0, std::memory_order_relaxed);
	g_gpu_execution_total_ns.store(0, std::memory_order_relaxed);
	g_gpu_execution_count.store(0, std::memory_order_relaxed);

	// Stage 8 additions: Reset Trace event cursor
	g_trace_event_cursor.store(0, std::memory_order_relaxed);

	// Stage 8.4B/C/E additions: Reset CP Sequencer and DrawPrep telemetry
	g_cp_sequencer_ops_emitted.store(0, std::memory_order_relaxed);
	g_cp_sequencer_lockstep_waits.store(0, std::memory_order_relaxed);
	g_cp_sequencer_snapshots_taken.store(0, std::memory_order_relaxed);
	g_cp_sequencer_waits_spin.store(0, std::memory_order_relaxed);
	g_baseline_cp_sequencer_ops_emitted.store(0, std::memory_order_relaxed);
	g_baseline_cp_sequencer_lockstep_waits.store(0, std::memory_order_relaxed);
	g_baseline_cp_sequencer_snapshots_taken.store(0, std::memory_order_relaxed);
	g_baseline_cp_sequencer_waits_spin.store(0, std::memory_order_relaxed);

	g_draw_prep_published.store(0, std::memory_order_relaxed);
	g_draw_prep_prepared.store(0, std::memory_order_relaxed);
	g_draw_prep_committed.store(0, std::memory_order_relaxed);
	g_draw_prep_fallback.store(0, std::memory_order_relaxed);
	g_draw_prep_avoided.store(0, std::memory_order_relaxed);
	g_draw_prep_avoided_time_ns.store(0, std::memory_order_relaxed);
	g_baseline_draw_prep_published.store(0, std::memory_order_relaxed);
	g_baseline_draw_prep_prepared.store(0, std::memory_order_relaxed);
	g_baseline_draw_prep_committed.store(0, std::memory_order_relaxed);
	g_baseline_draw_prep_fallback.store(0, std::memory_order_relaxed);
	g_baseline_draw_prep_avoided.store(0, std::memory_order_relaxed);
	g_baseline_draw_prep_avoided_time_ns.store(0, std::memory_order_relaxed);

	for (size_t i = 0; i < static_cast<size_t>(QueueLatencyType::Count); ++i) {
		g_queue_latency_ns[i].store(0, std::memory_order_relaxed);
		g_queue_latency_count[i].store(0, std::memory_order_relaxed);
	}

	{
		std::lock_guard<std::mutex> lock(g_frame_timeseries_mutex);
		g_frame_timeseries.clear();
	}
	g_last_frame_counters = {};

	g_has_completed_snapshot.store(false, std::memory_order_relaxed);
}

void StartCapture(const char* trigger_reason) {
	std::lock_guard lock(g_capture_mutex);
	SetEnabled(true);
	Common::DebugCounters::SetEnabled(true);
	Reset();

	g_current_phase.store(ExecutionPhase::Gameplay, std::memory_order_relaxed);
	g_trigger_reason = trigger_reason != nullptr ? trigger_reason : "Unknown";

	// Snapshot baseline values of external DebugCounters to compute deltas
	namespace Counters = Common::DebugCounters;
	g_baseline_scratch_freed_bytes.store(Counters::Get(Counters::Counter::ScratchFreedBytes),
	                                     std::memory_order_relaxed);
	g_baseline_scratch_retirements.store(Counters::Get(Counters::Counter::ScratchRetirements),
	                                     std::memory_order_relaxed);
	g_baseline_scratch_allocations.store(Counters::Get(Counters::Counter::ScratchAllocations),
	                                     std::memory_order_relaxed);
	g_baseline_compact_readback_copies.store(
	    Counters::Get(Counters::Counter::CompactReadbackCopies), std::memory_order_relaxed);
	g_baseline_compact_readback_fallbacks.store(
	    Counters::Get(Counters::Counter::CompactReadbackFallbacks), std::memory_order_relaxed);
	g_baseline_readback_prefill_avoided_bytes.store(
	    Counters::Get(Counters::Counter::ReadbackPrefillAvoidedBytes), std::memory_order_relaxed);
	g_baseline_readback_staging_saved_bytes.store(
	    Counters::Get(Counters::Counter::ReadbackStagingSavedBytes), std::memory_order_relaxed);
	g_baseline_read_buffer_hits.store(Counters::Get(Counters::Counter::ReadBufferHits),
	                                  std::memory_order_relaxed);
	g_baseline_resource_prep_hits.store(Counters::Get(Counters::Counter::ResourcePrepHits),
	                                    std::memory_order_relaxed);
	g_baseline_read_buffer_misses.store(Counters::Get(Counters::Counter::ReadBufferMisses),
	                                    std::memory_order_relaxed);
	g_baseline_resource_prep_misses.store(Counters::Get(Counters::Counter::ResourcePrepMisses),
	                                      std::memory_order_relaxed);
	g_baseline_tile_layout_hits.store(Counters::Get(Counters::Counter::TileLayoutHits),
	                                  std::memory_order_relaxed);
	g_baseline_tile_layout_misses.store(Counters::Get(Counters::Counter::TileLayoutMisses),
	                                    std::memory_order_relaxed);
	g_baseline_binding_reuses[0].store(Counters::Get(Counters::Counter::BindingRangeReuses),
	                                   std::memory_order_relaxed);
	g_baseline_binding_reuses[1].store(Counters::Get(Counters::Counter::BindingLookupReuses),
	                                   std::memory_order_relaxed);
	g_baseline_binding_reuses[2].store(Counters::Get(Counters::Counter::BindingSamplerReuses),
	                                   std::memory_order_relaxed);
	g_baseline_binding_reuses[3].store(Counters::Get(Counters::Counter::BindingUploadReuses),
	                                   std::memory_order_relaxed);
	g_baseline_binding_reuses[4].store(Counters::Get(Counters::Counter::BindingUploadReuseBytes),
	                                   std::memory_order_relaxed);
	g_baseline_neighbour_downloads.store(Counters::Get(Counters::Counter::BufferNeighbourDownloads),
	                                     std::memory_order_relaxed);
	g_baseline_neighbour_download_bytes.store(
	    Counters::Get(Counters::Counter::BufferNeighbourDownloadBytes), std::memory_order_relaxed);
	g_baseline_buffer_upload_bytes.store(Counters::Get(Counters::Counter::BufferUploadBytes),
	                                     std::memory_order_relaxed);
	g_baseline_texture_upload_bytes.store(Counters::Get(Counters::Counter::TextureUploadBytes),
	                                      std::memory_order_relaxed);
	g_baseline_buffer_download_bytes.store(Counters::Get(Counters::Counter::BufferDownloadBytes),
	                                       std::memory_order_relaxed);
	g_baseline_texture_download_bytes.store(Counters::Get(Counters::Counter::TextureDownloadBytes),
	                                        std::memory_order_relaxed);
	g_baseline_texture_partial_refreshes.store(
	    Counters::Get(Counters::Counter::TexturePartialRefreshes), std::memory_order_relaxed);
	g_baseline_texture_partial_fallbacks.store(
	    Counters::Get(Counters::Counter::TexturePartialFallbacks), std::memory_order_relaxed);
	g_baseline_texture_partial_upload_bytes.store(
	    Counters::Get(Counters::Counter::TexturePartialUploadBytes), std::memory_order_relaxed);
	g_baseline_cadence_held_back.store(Counters::Get(Counters::Counter::CadenceHeldBack),
	                                   std::memory_order_relaxed);
	g_baseline_cadence_submits.store(Counters::Get(Counters::Counter::CadenceSubmits),
	                                 std::memory_order_relaxed);
	g_baseline_draws.store(Counters::Get(Counters::Counter::Draws), std::memory_order_relaxed);
	g_baseline_draw_instances.store(Counters::Get(Counters::Counter::DrawInstances),
	                                std::memory_order_relaxed);
	g_baseline_indirect_draws.store(Counters::Get(Counters::Counter::IndirectDraws),
	                                std::memory_order_relaxed);
	g_baseline_dispatches.store(Counters::Get(Counters::Counter::Dispatches),
	                            std::memory_order_relaxed);
	g_baseline_pipelines_created.store(Counters::Get(Counters::Counter::PipelinesCreated),
	                                   std::memory_order_relaxed);
	g_baseline_shaders_compiled.store(Counters::Get(Counters::Counter::ShadersCompiled),
	                                  std::memory_order_relaxed);
	g_baseline_gpu_thread_busy_ns.store(Counters::Get(Counters::Counter::GpuThreadBusyNs),
	                                    std::memory_order_relaxed);
	g_baseline_pipeline_cache_lookups.store(Counters::Get(Counters::Counter::PipelineCacheLookups),
	                                        std::memory_order_relaxed);
	g_baseline_pipeline_cache_hits.store(Counters::Get(Counters::Counter::PipelineCacheHits),
	                                     std::memory_order_relaxed);
	g_baseline_redundant_pipeline_binds.store(
	    Counters::Get(Counters::Counter::RedundantPipelineBinds), std::memory_order_relaxed);
	g_baseline_redundant_vertex_binds.store(Counters::Get(Counters::Counter::RedundantVertexBinds),
	                                        std::memory_order_relaxed);
	g_baseline_redundant_index_binds.store(Counters::Get(Counters::Counter::RedundantIndexBinds),
	                                       std::memory_order_relaxed);
	g_baseline_priority_wait_spins.store(g_priority_wait_spins.load(std::memory_order_relaxed),
	                                     std::memory_order_relaxed);
	g_baseline_priority_wait_spin_hits.store(
	    g_priority_wait_spin_hits.load(std::memory_order_relaxed), std::memory_order_relaxed);
	g_baseline_pending_ops_fast_skips.store(
	    g_pending_ops_fast_skips.load(std::memory_order_relaxed), std::memory_order_relaxed);
	g_baseline_cp_sequencer_ops_emitted.store(
	    g_cp_sequencer_ops_emitted.load(std::memory_order_relaxed), std::memory_order_relaxed);
	g_baseline_cp_sequencer_lockstep_waits.store(
	    g_cp_sequencer_lockstep_waits.load(std::memory_order_relaxed), std::memory_order_relaxed);
	g_baseline_cp_sequencer_snapshots_taken.store(
	    g_cp_sequencer_snapshots_taken.load(std::memory_order_relaxed), std::memory_order_relaxed);
	g_baseline_cp_sequencer_waits_spin.store(
	    g_cp_sequencer_waits_spin.load(std::memory_order_relaxed), std::memory_order_relaxed);
	g_baseline_draw_prep_published.store(g_draw_prep_published.load(std::memory_order_relaxed),
	                                     std::memory_order_relaxed);
	g_baseline_draw_prep_prepared.store(g_draw_prep_prepared.load(std::memory_order_relaxed),
	                                    std::memory_order_relaxed);
	g_baseline_draw_prep_committed.store(g_draw_prep_committed.load(std::memory_order_relaxed),
	                                     std::memory_order_relaxed);
	g_baseline_draw_prep_fallback.store(g_draw_prep_fallback.load(std::memory_order_relaxed),
	                                    std::memory_order_relaxed);
	g_baseline_draw_prep_avoided.store(g_draw_prep_avoided.load(std::memory_order_relaxed),
	                                   std::memory_order_relaxed);
	g_baseline_draw_prep_avoided_time_ns.store(
	    g_draw_prep_avoided_time_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);

	for (size_t i = 0; i < static_cast<size_t>(QueueLatencyType::Count); ++i) {
		g_queue_latency_ns[i].store(0, std::memory_order_relaxed);
		g_queue_latency_count[i].store(0, std::memory_order_relaxed);
	}

	const auto now       = Clock::now();
	g_capture_start_time = now;
	g_capture_end_time   = Clock::time_point {};

	const auto wall_now_ns = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
	const auto cpu_now_ns = SampleGpuThreadCpuTimeNs();
	g_gpu_thread_wall_start_ns.store(wall_now_ns, std::memory_order_relaxed);
	g_gpu_thread_wall_end_ns.store(wall_now_ns, std::memory_order_relaxed);
	g_gpu_thread_cpu_start_ns.store(cpu_now_ns, std::memory_order_relaxed);
	g_gpu_thread_cpu_end_ns.store(cpu_now_ns, std::memory_order_relaxed);

	// Initialize last frame counters for timeseries delta tracking
	g_last_frame_counters.draws      = Counters::Get(Counters::Counter::Draws);
	g_last_frame_counters.dispatches = Counters::Get(Counters::Counter::Dispatches);
	uint64_t init_submits            = 0;
	for (size_t i = 0; i < static_cast<size_t>(SubmitReason::Count); ++i) {
		init_submits += g_submit_counts[i].load(std::memory_order_relaxed);
	}
	g_last_frame_counters.submits      = init_submits;
	g_last_frame_counters.write_faults = g_total_faults.load(std::memory_order_relaxed);
	g_last_frame_counters.stream_buffer_bytes =
	    g_stream_buffer_bytes.load(std::memory_order_relaxed);
	g_last_frame_counters.stream_buffer_wait_ns =
	    g_stream_buffer_wait_total_ns.load(std::memory_order_relaxed);
	for (size_t s = 0; s < static_cast<size_t>(PipelineStage::Count); ++s) {
		g_last_frame_counters.stage_nanos[s] = g_stage_nanos[s].load(std::memory_order_relaxed);
	}

	const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                          std::chrono::system_clock::now().time_since_epoch())
	                          .count();
	g_capture_id        = fmt::format("cap_{:x}", epoch_ms);

	g_capture_state.store(CaptureState::Running, std::memory_order_relaxed);
	Log::WriteToConsoleAndLog(fmt::format(
	    "[AuditProfiler] Capture STARTED (ID: {}, reason: {}, target duration: {} s, phase: "
	    "Gameplay) - Clean baseline established\n",
	    g_capture_id, g_trigger_reason, g_capture_duration_sec.load(std::memory_order_relaxed)));
}

void StopCapture(CaptureState state) {
	std::lock_guard lock(g_capture_mutex);
	const auto      current = g_capture_state.load(std::memory_order_relaxed);
	if (current != CaptureState::Running) {
		return;
	}

	g_capture_end_time = Clock::now();
	g_capture_state.store(state, std::memory_order_relaxed);

	// Freeze snapshot at the exact moment of capture completion
	g_completed_snapshot = TakeSnapshot();
	g_has_completed_snapshot.store(true, std::memory_order_release);

	const auto duration_ms =
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                              g_capture_end_time - g_capture_start_time)
	                              .count());
	const auto frames = g_captured_frame_count.load(std::memory_order_relaxed);

	const char* state_str = (state == CaptureState::Completed) ? "COMPLETED" : "INTERRUPTED";
	Log::WriteToConsoleAndLog(
	    fmt::format("[AuditProfiler] Capture {} (ID: {}, frames: {}, duration: {:.2f} s)\n",
	                state_str, g_capture_id, frames, duration_ms / 1000.0));

	// Automatically export results upon successful completion
	std::string path;
	{
		std::lock_guard cfg_lock(g_config_mutex);
		path = g_export_path;
	}
	if (!path.empty()) {
		std::string csv_path = path;
		if (csv_path.ends_with(".json")) {
			csv_path = csv_path.substr(0, csv_path.size() - 5) + ".csv";
		} else {
			csv_path += ".csv";
		}
		ExportAll(path, csv_path, &g_completed_snapshot);
	}
}

void ToggleCapture() {
	const auto state = g_capture_state.load(std::memory_order_relaxed);
	if (state == CaptureState::Running) {
		StopCapture(CaptureState::Completed);
	} else {
		StartCapture("ManualHotkeyF9");
	}
}

uint32_t GetTargetDurationSec() noexcept {
	return g_capture_duration_sec.load(std::memory_order_relaxed);
}

void SetExecutionPhase(ExecutionPhase phase) noexcept {
	g_current_phase.store(phase, std::memory_order_relaxed);
}

ExecutionPhase GetExecutionPhase() noexcept {
	return g_current_phase.load(std::memory_order_relaxed);
}

const char* GetExecutionPhaseName(ExecutionPhase phase) noexcept {
	switch (phase) {
		case ExecutionPhase::Initialization: return "Initialization";
		case ExecutionPhase::Loading: return "Loading";
		case ExecutionPhase::Warmup: return "Warmup";
		case ExecutionPhase::Gameplay: return "Gameplay";
		case ExecutionPhase::Transition: return "Transition";
		case ExecutionPhase::WorldEntryPrep: return "WorldEntryPrep";
		case ExecutionPhase::DescentStart: return "DescentStart";
		case ExecutionPhase::ActiveDescent: return "ActiveDescent";
		case ExecutionPhase::GroundApproach: return "GroundApproach";
		case ExecutionPhase::Touchdown: return "Touchdown";
		case ExecutionPhase::EarlyGameplay: return "EarlyGameplay";
		default: return "Unknown";
	}
}

void AdvancePhase() noexcept {
	const auto     cur  = g_current_phase.load(std::memory_order_relaxed);
	ExecutionPhase next = ExecutionPhase::WorldEntryPrep;
	switch (cur) {
		case ExecutionPhase::WorldEntryPrep: next = ExecutionPhase::DescentStart; break;
		case ExecutionPhase::DescentStart: next = ExecutionPhase::ActiveDescent; break;
		case ExecutionPhase::ActiveDescent: next = ExecutionPhase::GroundApproach; break;
		case ExecutionPhase::GroundApproach: next = ExecutionPhase::Touchdown; break;
		case ExecutionPhase::Touchdown: next = ExecutionPhase::EarlyGameplay; break;
		case ExecutionPhase::EarlyGameplay: next = ExecutionPhase::Gameplay; break;
		default: next = ExecutionPhase::WorldEntryPrep; break;
	}
	SetExecutionPhase(next);
	Log::WriteToConsoleAndLog(fmt::format("[AuditProfiler] Execution phase advanced to: {} (F10)\n",
	                                      GetExecutionPhaseName(next)));
}

void MarkPhase(ExecutionPhase phase, const char* description) noexcept {
	const auto old = g_current_phase.exchange(phase, std::memory_order_relaxed);
	Log::WriteToConsoleAndLog(fmt::format("[AuditProfiler] Phase transition: {} -> {} ({})\n",
	                                      GetExecutionPhaseName(old), GetExecutionPhaseName(phase),
	                                      description != nullptr ? description : "no details"));
}

void InitCapture(uint32_t delay_sec, uint32_t duration_sec, std::string_view export_path,
                 bool manual_trigger) {
	SetEnabled(true);
	Common::DebugCounters::SetEnabled(true);
	if (!export_path.empty()) {
		SetExportPath(export_path);
	}
	g_capture_delay_sec.store(delay_sec, std::memory_order_relaxed);
	g_capture_duration_sec.store(duration_sec, std::memory_order_relaxed);
	g_session_start_time = Clock::now();

	if (manual_trigger) {
		g_capture_state.store(CaptureState::WaitingForHotkey, std::memory_order_relaxed);
		g_current_phase.store(ExecutionPhase::Warmup, std::memory_order_relaxed);
		Log::WriteToConsoleAndLog(
		    fmt::format("[AuditProfiler] Audit profiler initialized in WARMUP mode. Waiting for F9 "
		                "in target scene (target duration: {} s)\n",
		                duration_sec));
	} else if (delay_sec > 0) {
		g_capture_state.store(CaptureState::Scheduled, std::memory_order_relaxed);
		Log::WriteToConsoleAndLog(fmt::format(
		    "[AuditProfiler] Capture scheduled to start after {} seconds (duration: {} s)\n",
		    delay_sec, duration_sec));
	} else {
		// Continuous capture from startup or immediate start
		StartCapture(duration_sec > 0 ? "StartupScheduled" : "StartupContinuous");
	}
}

void RecordFault(uint64_t fault_vaddr, uint64_t rip, uint64_t duration_ns) noexcept {
	if (!IsEnabled()) {
		return;
	}

	if (g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}

	const auto phase = g_current_phase.load(std::memory_order_relaxed);
	if (phase == ExecutionPhase::Loading) {
		g_phase_loading.faults.fetch_add(1, std::memory_order_relaxed);
	} else if (phase == ExecutionPhase::Gameplay) {
		g_phase_gameplay.faults.fetch_add(1, std::memory_order_relaxed);
	}

	g_total_faults.fetch_add(1, std::memory_order_relaxed);
	g_total_handler_ns.fetch_add(duration_ns, std::memory_order_relaxed);
	UpdateMax(g_max_handler_ns, duration_ns);

	// Page slot lookup (align to 4KB page)
	const uint64_t page      = fault_vaddr & ~uint64_t {4095};
	const size_t   page_hash = static_cast<size_t>((page >> 12) * 0x9e3779b97f4a7c15ull);
	for (size_t i = 0; i < MAX_PAGE_PROBE; ++i) {
		auto& slot = g_page_table[(page_hash + i) & PAGE_TABLE_MASK];
		if (slot.page.load(std::memory_order_relaxed) == page) {
			slot.faults.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		uint64_t expected = 0;
		if (slot.page.load(std::memory_order_relaxed) == 0 &&
		    slot.page.compare_exchange_strong(expected, page, std::memory_order_acq_rel)) {
			slot.faults.fetch_add(1, std::memory_order_relaxed);
			break;
		}
	}

	// RIP slot lookup
	if (rip != 0) {
		const size_t rip_hash = static_cast<size_t>(rip * 0x9e3779b97f4a7c15ull);
		for (size_t i = 0; i < MAX_PROBE; ++i) {
			auto& slot = g_rip_table[(rip_hash + i) & RIP_TABLE_MASK];
			if (slot.rip.load(std::memory_order_relaxed) == rip) {
				slot.count.fetch_add(1, std::memory_order_relaxed);
				break;
			}
			uint64_t expected = 0;
			if (slot.rip.load(std::memory_order_relaxed) == 0 &&
			    slot.rip.compare_exchange_strong(expected, rip, std::memory_order_acq_rel)) {
				slot.count.fetch_add(1, std::memory_order_relaxed);
				break;
			}
		}
	}

	// Thread slot lookup
	const uint32_t tid = GetCurrentTid();
	if (tid != 0) {
		const size_t thread_hash = static_cast<size_t>(tid * 2654435761u);
		for (size_t i = 0; i < MAX_PROBE; ++i) {
			auto& slot = g_thread_table[(thread_hash + i) & THREAD_TABLE_MASK];
			if (slot.tid.load(std::memory_order_relaxed) == tid) {
				slot.count.fetch_add(1, std::memory_order_relaxed);
				break;
			}
			uint32_t expected = 0;
			if (slot.tid.load(std::memory_order_relaxed) == 0 &&
			    slot.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
				slot.count.fetch_add(1, std::memory_order_relaxed);
				std::snprintf(slot.name, sizeof(slot.name), "TID %u", tid);
				break;
			}
		}
	}
}

void RecordReprotect(uint64_t vaddr, uint64_t size) noexcept {
	if (!IsEnabled() || size == 0) {
		return;
	}

	if (g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}

	const uint64_t begin      = vaddr & ~uint64_t {4095};
	const uint64_t end        = (vaddr + size + 4095) & ~uint64_t {4095};
	const uint64_t page_count = (end - begin) / 4096;

	const auto phase = g_current_phase.load(std::memory_order_relaxed);
	if (phase == ExecutionPhase::Loading) {
		g_phase_loading.reprotects.fetch_add(page_count, std::memory_order_relaxed);
	} else if (phase == ExecutionPhase::Gameplay) {
		g_phase_gameplay.reprotects.fetch_add(page_count, std::memory_order_relaxed);
	}

	g_total_reprotects.fetch_add(page_count, std::memory_order_relaxed);

	// Only update pages already tracked in g_page_table (i.e. pages that have faulted).
	for (uint64_t page = begin; page < end; page += 4096) {
		const size_t page_hash = static_cast<size_t>((page >> 12) * 0x9e3779b97f4a7c15ull);
		for (size_t i = 0; i < MAX_PAGE_PROBE; ++i) {
			auto&      slot         = g_page_table[(page_hash + i) & PAGE_TABLE_MASK];
			const auto current_page = slot.page.load(std::memory_order_relaxed);
			if (current_page == page) {
				slot.reprotects.fetch_add(1, std::memory_order_relaxed);
				break;
			}
			if (current_page == 0) {
				break;
			}
		}
	}
}

void RecordGpuWait(GpuWaitReason reason, uint64_t wait_ns) noexcept {
	if (g_live_enabled.load(std::memory_order_relaxed) &&
	    static_cast<size_t>(reason) < g_live_wait_ns.size())
		g_live_wait_ns[static_cast<size_t>(reason)].fetch_add(wait_ns, std::memory_order_relaxed);
	if (!IsEnabled()) {
		return;
	}

	if (g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}

	const auto phase = g_current_phase.load(std::memory_order_relaxed);
	if (phase == ExecutionPhase::Loading) {
		g_phase_loading.gpu_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
	} else if (phase == ExecutionPhase::Gameplay) {
		g_phase_gameplay.gpu_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
	}

	const auto idx = static_cast<size_t>(reason);
	if (idx < static_cast<size_t>(GpuWaitReason::Count)) {
		g_wait_counts[idx].fetch_add(1, std::memory_order_relaxed);
		g_wait_nanos[idx].fetch_add(wait_ns, std::memory_order_relaxed);
		UpdateMax(g_wait_max_nanos[idx], wait_ns);
	}
}

void RecordSubmit(SubmitReason reason) noexcept {
	if (!IsEnabled()) {
		return;
	}

	if (g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}

	const auto phase = g_current_phase.load(std::memory_order_relaxed);
	if (phase == ExecutionPhase::Loading) {
		g_phase_loading.submits.fetch_add(1, std::memory_order_relaxed);
	} else if (phase == ExecutionPhase::Gameplay) {
		g_phase_gameplay.submits.fetch_add(1, std::memory_order_relaxed);
	}

	const auto idx = static_cast<size_t>(reason);
	if (idx < static_cast<size_t>(SubmitReason::Count)) {
		g_submit_counts[idx].fetch_add(1, std::memory_order_relaxed);
	}
}

void RecordInternalGpuTransfer(uint64_t bytes) noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_internal_gpu_transfer_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void RecordFrame(double frame_ms) noexcept {
	if (g_live_enabled.load(std::memory_order_relaxed) && std::isfinite(frame_ms) &&
	    frame_ms > 0.0 && frame_ms < 1e9) {
		const auto ns = static_cast<uint64_t>(frame_ms * 1e6);
		g_live_counts[LiveFrames].fetch_add(1, std::memory_order_relaxed);
		g_live_counts[LiveFrameNs].fetch_add(ns, std::memory_order_relaxed);
		g_live_last_frame_ns.store(LiveNowNs(), std::memory_order_relaxed);
		UpdateMax(g_live_max_frame_ns, ns);
		if (frame_ms >= 50) g_live_counts[LiveHitch50].fetch_add(1, std::memory_order_relaxed);
		if (frame_ms >= 100) g_live_counts[LiveHitch100].fetch_add(1, std::memory_order_relaxed);
		if (frame_ms >= 250) g_live_counts[LiveHitch250].fetch_add(1, std::memory_order_relaxed);
	}
	if (!IsEnabled() || frame_ms <= 0.0) {
		return;
	}

	const auto state = g_capture_state.load(std::memory_order_relaxed);
	const auto now   = Clock::now();

	// Check scheduled delay
	if (state == CaptureState::Scheduled) {
		const auto delay_sec = g_capture_delay_sec.load(std::memory_order_relaxed);
		if (std::chrono::duration_cast<std::chrono::seconds>(now - g_session_start_time).count() >=
		    delay_sec) {
			StartCapture("DelayExpired");
		}
		return;
	}

	// While waiting for hotkey (warmup) or when not running, do not record capture frames or phase
	// metrics
	if (state == CaptureState::WaitingForHotkey || state != CaptureState::Running) {
		return;
	}

	const auto phase = g_current_phase.load(std::memory_order_relaxed);
	if (phase == ExecutionPhase::Loading) {
		g_phase_loading.frame_count.fetch_add(1, std::memory_order_relaxed);
		g_phase_loading.total_frame_ns.fetch_add(static_cast<uint64_t>(frame_ms * 1e6),
		                                         std::memory_order_relaxed);
	} else if (phase == ExecutionPhase::Gameplay) {
		g_phase_gameplay.frame_count.fetch_add(1, std::memory_order_relaxed);
		g_phase_gameplay.total_frame_ns.fetch_add(static_cast<uint64_t>(frame_ms * 1e6),
		                                          std::memory_order_relaxed);
	}

	// Record frame duration into bounded preallocated buffer
	const auto index = g_captured_frame_count.fetch_add(1, std::memory_order_relaxed);
	if (index < MAX_CAPTURED_FRAMES) {
		g_captured_frames[index] = frame_ms;
	}

	if (g_capture_state.load(std::memory_order_relaxed) == CaptureState::Running) {
		FrameTimeSeriesRecord rec {};
		rec.frame_index = static_cast<uint32_t>(index);
		rec.frame_ms    = static_cast<float>(frame_ms);
		rec.phase       = static_cast<uint8_t>(g_current_phase.load(std::memory_order_relaxed));

		namespace Counters            = Common::DebugCounters;
		const uint64_t cur_draws      = Counters::Get(Counters::Counter::Draws);
		const uint64_t cur_dispatches = Counters::Get(Counters::Counter::Dispatches);
		uint64_t       cur_submits    = 0;
		for (size_t i = 0; i < static_cast<size_t>(SubmitReason::Count); ++i) {
			cur_submits += g_submit_counts[i].load(std::memory_order_relaxed);
		}
		const uint64_t cur_faults   = g_total_faults.load(std::memory_order_relaxed);
		const uint64_t cur_sb_bytes = g_stream_buffer_bytes.load(std::memory_order_relaxed);
		const uint64_t cur_sb_wait  = g_stream_buffer_wait_total_ns.load(std::memory_order_relaxed);

		rec.draws = static_cast<uint32_t>(cur_draws >= g_last_frame_counters.draws
		                                      ? (cur_draws - g_last_frame_counters.draws)
		                                      : 0);
		rec.dispatches =
		    static_cast<uint32_t>(cur_dispatches >= g_last_frame_counters.dispatches
		                              ? (cur_dispatches - g_last_frame_counters.dispatches)
		                              : 0);
		rec.submits = static_cast<uint32_t>(cur_submits >= g_last_frame_counters.submits
		                                        ? (cur_submits - g_last_frame_counters.submits)
		                                        : 0);
		rec.write_faults =
		    static_cast<uint32_t>(cur_faults >= g_last_frame_counters.write_faults
		                              ? (cur_faults - g_last_frame_counters.write_faults)
		                              : 0);
		rec.stream_buffer_alloc_kb =
		    static_cast<uint32_t>((cur_sb_bytes >= g_last_frame_counters.stream_buffer_bytes
		                               ? (cur_sb_bytes - g_last_frame_counters.stream_buffer_bytes)
		                               : 0) /
		                          1024);
		rec.stream_buffer_wait_us =
		    static_cast<uint32_t>((cur_sb_wait >= g_last_frame_counters.stream_buffer_wait_ns
		                               ? (cur_sb_wait - g_last_frame_counters.stream_buffer_wait_ns)
		                               : 0) /
		                          1000);

		auto get_stage_us = [&](PipelineStage stage) -> uint32_t {
			const auto     s     = static_cast<size_t>(stage);
			const uint64_t cur   = g_stage_nanos[s].load(std::memory_order_relaxed);
			const uint64_t delta = (cur >= g_last_frame_counters.stage_nanos[s])
			                           ? (cur - g_last_frame_counters.stage_nanos[s])
			                           : 0;
			g_last_frame_counters.stage_nanos[s] = cur;
			return static_cast<uint32_t>(delta / 1000);
		};

		rec.pm4_decode_us         = get_stage_us(PipelineStage::Pm4Decode);
		rec.pm4_dispatch_us       = get_stage_us(PipelineStage::Pm4Dispatch);
		rec.render_state_prep_us  = get_stage_us(PipelineStage::RenderStatePrep);
		rec.descriptor_prep_us    = get_stage_us(PipelineStage::DescriptorPrep);
		rec.pipeline_lookup_us    = get_stage_us(PipelineStage::PipelineLookup);
		rec.pipeline_key_build_us = get_stage_us(PipelineStage::PipelineKeyBuild);
		rec.buffer_prep_us        = get_stage_us(PipelineStage::BufferPrep);
		rec.command_recording_us  = get_stage_us(PipelineStage::CommandRecording);
		rec.barriers_us           = get_stage_us(PipelineStage::BarriersTransitions);
		rec.sync_waits_us         = get_stage_us(PipelineStage::SyncWaits);

		g_last_frame_counters.draws                 = cur_draws;
		g_last_frame_counters.dispatches            = cur_dispatches;
		g_last_frame_counters.submits               = cur_submits;
		g_last_frame_counters.write_faults          = cur_faults;
		g_last_frame_counters.stream_buffer_bytes   = cur_sb_bytes;
		g_last_frame_counters.stream_buffer_wait_ns = cur_sb_wait;

		{
			std::lock_guard<std::mutex> lock(g_frame_timeseries_mutex);
			if (g_frame_timeseries.size() < MAX_CAPTURED_FRAMES) {
				g_frame_timeseries.push_back(rec);
			}
		}
	}

	// Track hitches and stuttering (zero allocations, lock-free atomics)
	if (frame_ms >= 50.0) {
		g_hitches_50ms.fetch_add(1, std::memory_order_relaxed);
		g_hitch_time_ns.fetch_add(static_cast<uint64_t>(frame_ms * 1e6), std::memory_order_relaxed);
		if (frame_ms >= 100.0) {
			g_hitches_100ms.fetch_add(1, std::memory_order_relaxed);
			if (frame_ms >= 250.0) {
				g_hitches_250ms.fetch_add(1, std::memory_order_relaxed);
				if (frame_ms >= 500.0) {
					g_hitches_500ms.fetch_add(1, std::memory_order_relaxed);
					if (frame_ms >= 1000.0) {
						g_hitches_1000ms.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
			const auto slot =
			    g_slow_frame_cursor.fetch_add(1, std::memory_order_relaxed) % MAX_SLOW_FRAMES;
			const auto elapsed_ms = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(now - g_capture_start_time)
			        .count());
			g_slow_frames[slot].frame_index.store(index, std::memory_order_relaxed);
			g_slow_frames[slot].frame_ms.store(frame_ms, std::memory_order_relaxed);
			g_slow_frames[slot].timestamp_ms.store(elapsed_ms, std::memory_order_relaxed);
		}
	}

	// Check if target duration expired
	const auto duration_sec = g_capture_duration_sec.load(std::memory_order_relaxed);
	if (duration_sec > 0 &&
	    std::chrono::duration_cast<std::chrono::seconds>(now - g_capture_start_time).count() >=
	        duration_sec) {
		StopCapture(CaptureState::Completed);
	}
}

void RecordVblank() noexcept {
	if (g_live_enabled.load(std::memory_order_relaxed))
		g_live_counts[LiveVblanks].fetch_add(1, std::memory_order_relaxed);
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_vblank_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordFlipSubmitted() noexcept {
	if (g_live_enabled.load(std::memory_order_relaxed))
		g_live_counts[LiveSubmitted].fetch_add(1, std::memory_order_relaxed);
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_flip_submitted_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordFlipCompleted() noexcept {
	if (g_live_enabled.load(std::memory_order_relaxed))
		g_live_counts[LiveCompleted].fetch_add(1, std::memory_order_relaxed);
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_flip_completed_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordFlipWait(uint64_t wait_ns) noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_flip_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
	g_flip_wait_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordAudioUpdate(uint32_t samples, uint32_t freq) noexcept {
	if (g_live_enabled.load(std::memory_order_relaxed) && freq) {
		g_live_counts[LiveAudioNs].fetch_add(uint64_t {samples} * 1'000'000'000 / freq,
		                                     std::memory_order_relaxed);
		g_live_counts[LiveAudioBatches].fetch_add(1, std::memory_order_relaxed);
	}
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_audio_updates_count.fetch_add(1, std::memory_order_relaxed);
	g_audio_total_samples.fetch_add(samples, std::memory_order_relaxed);
}

void RecordPipelineStage(PipelineStage stage, uint64_t duration_ns) noexcept {
	if (g_live_enabled.load(std::memory_order_relaxed) &&
	    static_cast<size_t>(stage) < g_live_stage_ns.size())
		g_live_stage_ns[static_cast<size_t>(stage)].fetch_add(duration_ns,
		                                                      std::memory_order_relaxed);
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	const auto idx = static_cast<size_t>(stage);
	if (idx < static_cast<size_t>(PipelineStage::Count)) {
		g_stage_counts[idx].fetch_add(1, std::memory_order_relaxed);
		g_stage_nanos[idx].fetch_add(duration_ns, std::memory_order_relaxed);
		UpdateMax(g_stage_max_nanos[idx], duration_ns);
		UpdateMin(g_stage_min_nanos[idx], duration_ns);
	}
}

const char* GetPipelineStageName(PipelineStage stage) noexcept {
	switch (stage) {
		case PipelineStage::Pm4Decode: return "Pm4Decode";
		case PipelineStage::Pm4Dispatch: return "Pm4Dispatch";
		case PipelineStage::RenderStatePrep: return "RenderStatePrep";
		case PipelineStage::PipelineKeyBuild: return "PipelineKeyBuild";
		case PipelineStage::PipelineLookup: return "PipelineLookup";
		case PipelineStage::DescriptorPrep: return "DescriptorPrep";
		case PipelineStage::BarriersTransitions: return "BarriersTransitions";
		case PipelineStage::BufferPrep: return "BufferPrep";
		case PipelineStage::CommandRecording: return "CommandRecording";
		case PipelineStage::CommandEnd: return "CommandEnd";
		case PipelineStage::Submit: return "Submit";
		case PipelineStage::SyncWaits: return "SyncWaits";
		case PipelineStage::Present: return "Present";
		case PipelineStage::DrawPrep: return "DrawPrep";
		case PipelineStage::SequencerWait: return "SequencerWait";
		case PipelineStage::DrawPrepWorker: return "DrawPrepWorker";
		case PipelineStage::DrawPrepCommitWait: return "DrawPrepCommitWait";
		default: return "Unknown";
	}
}

void RecordTraceEvent(TraceEventType type, uint64_t duration_ns, uint64_t tick,
                      uint64_t data) noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	if (duration_ns < 1'000'000 && type != TraceEventType::StreamBufferWait &&
	    type != TraceEventType::BufferReadback && type != TraceEventType::PriorityOperation &&
	    type != TraceEventType::PipelineCompile && type != TraceEventType::WriteFault &&
	    type != TraceEventType::Reprotect) {
		return;
	}
	const auto idx =
	    g_trace_event_cursor.fetch_add(1, std::memory_order_relaxed) % TRACE_EVENT_RING_CAPACITY;
	const auto now          = Clock::now();
	const auto timestamp_us = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::microseconds>(now - g_capture_start_time).count());
	g_trace_event_ring[idx].timestamp_us = timestamp_us;
	g_trace_event_ring[idx].duration_ns  = duration_ns;
	g_trace_event_ring[idx].tick         = tick;
	g_trace_event_ring[idx].data         = data;
	g_trace_event_ring[idx].tid          = GetCurrentTid();
	g_trace_event_ring[idx].type         = type;
}

const char* GetTraceEventTypeName(TraceEventType type) noexcept {
	switch (type) {
		case TraceEventType::None: return "None";
		case TraceEventType::TimelineWait: return "TimelineWait";
		case TraceEventType::VulkanSubmit: return "VulkanSubmit";
		case TraceEventType::StreamBufferWait: return "StreamBufferWait";
		case TraceEventType::BufferReadback: return "BufferReadback";
		case TraceEventType::PriorityOperation: return "PriorityOperation";
		case TraceEventType::SwapchainPresent: return "SwapchainPresent";
		case TraceEventType::WriteFault: return "WriteFault";
		case TraceEventType::Reprotect: return "Reprotect";
		case TraceEventType::PipelineCompile: return "PipelineCompile";
		case TraceEventType::Other:
		default: return "Other";
	}
}

void RecordStreamBufferAlloc(uint64_t bytes, bool wrapped, uint64_t high_water) noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_stream_buffer_allocs.fetch_add(1, std::memory_order_relaxed);
	g_stream_buffer_bytes.fetch_add(bytes, std::memory_order_relaxed);
	if (wrapped) {
		g_stream_buffer_wraps.fetch_add(1, std::memory_order_relaxed);
	}
	UpdateMax(g_stream_buffer_high_water, high_water);
}

void RecordStreamBufferWaitEvent(uint64_t wait_ns, uint64_t target_tick) noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_stream_buffer_wait_events.fetch_add(1, std::memory_order_relaxed);
	g_stream_buffer_wait_total_ns.fetch_add(wait_ns, std::memory_order_relaxed);
	UpdateMax(g_stream_buffer_wait_max_ns, wait_ns);
	g_stream_buffer_last_wait_tick.store(target_tick, std::memory_order_relaxed);
}

void NoteGpuThreadStart() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (g_gpu_thread_handle != nullptr) {
		::CloseHandle(g_gpu_thread_handle);
		g_gpu_thread_handle = nullptr;
	}
	::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
	                  &g_gpu_thread_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
#endif
	const auto wall_ns = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
	        .count());
	const auto cpu_ns = SampleGpuThreadCpuTimeNs();
	g_gpu_thread_wall_start_ns.store(wall_ns, std::memory_order_relaxed);
	g_gpu_thread_cpu_start_ns.store(cpu_ns, std::memory_order_relaxed);
}

void NoteGpuThreadFrame() noexcept {
	const auto wall_ns = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
	        .count());
	const auto cpu_ns = SampleGpuThreadCpuTimeNs();
	g_gpu_thread_wall_end_ns.store(wall_ns, std::memory_order_relaxed);
	g_gpu_thread_cpu_end_ns.store(cpu_ns, std::memory_order_relaxed);
}

void RecordGpuExecutionTime(uint64_t execution_ns) noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_gpu_execution_total_ns.fetch_add(execution_ns, std::memory_order_relaxed);
	g_gpu_execution_count.fetch_add(1, std::memory_order_relaxed);
}

ProfileSnapshot TakeSnapshot() {
	if (g_has_completed_snapshot.load(std::memory_order_acquire)) {
		return g_completed_snapshot;
	}

	ProfileSnapshot snapshot {};
	snapshot.enabled                        = IsEnabled();
	snapshot.cadence_enabled                = Config::GpuSubmitCadenceEnabled();
	snapshot.cadence_interval_us            = Config::GetGpuSubmitCadenceIntervalUs();
	snapshot.partial_refresh_enabled        = Config::PartialBufferRefreshEnabled();
	snapshot.batch_hot_readbacks_enabled    = Config::BatchHotReadbacksEnabled();
	snapshot.readback_neighbours_enabled    = Config::ReadbackNeighboursEnabled();
	snapshot.wide_readback_window_enabled   = Config::WideReadbackWindowEnabled();
	snapshot.fault_ahead_enabled            = Config::FaultAheadEnabled();
	snapshot.tile_layout_cache_enabled      = Config::TileLayoutCacheEnabled();
	snapshot.trim_scratch_memory_enabled    = Config::TrimScratchMemoryEnabled();
	snapshot.compact_image_readback_enabled = Config::CompactImageReadbackEnabled();
	snapshot.read_buffer_reuse_enabled      = Config::ReadBufferReuseEnabled();
	snapshot.resource_prep_reuse_enabled    = Config::ResourcePrepReuseEnabled();
	snapshot.binding_reuse_enabled          = Config::BindingReuseEnabled();
	snapshot.extended_stream_buffer_enabled = Config::ExtendedStreamBufferEnabled();
	snapshot.redundant_bind_elision_enabled = Config::RedundantBindElisionEnabled();
	snapshot.pipeline_cache_mru_enabled     = Config::PipelineCacheMruEnabled();
	snapshot.pipeline_key_cache_enabled     = Config::PipelineKeyCacheEnabled();
	snapshot.draw_state_dirty_check_enabled = Config::DrawStateDirtyCheckEnabled();
	snapshot.ensure_pipeline_cache_enabled  = Config::EnsurePipelineCacheEnabled();
	snapshot.descriptor_cache_enabled       = Config::DescriptorCacheEnabled();
	snapshot.fast_pending_ops_enabled       = Config::FastPendingOpsEnabled();
	snapshot.priority_spin_wait_enabled     = Config::PrioritySpinWaitEnabled();
	snapshot.priority_spin_duration_us      = Config::PrioritySpinDurationUs();
	snapshot.git_commit                     = KYTY_GIT_VERSION;
	snapshot.git_revision                   = KYTY_GIT_REVISION;
	snapshot.git_hash                       = KYTY_GIT_HASH;
	snapshot.build_timestamp                = __DATE__ " " __TIME__;
#if defined(__clang__)
	snapshot.compiler =
	    fmt::format("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_VER)
	snapshot.compiler = fmt::format("MSVC {}", _MSC_VER);
#else
	snapshot.compiler = "Unknown";
#endif
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	wchar_t mod_path[MAX_PATH] = {};
	::GetModuleFileNameW(nullptr, mod_path, MAX_PATH);
	std::wstring ws(mod_path);
	snapshot.binary_path = std::string(ws.begin(), ws.end());
#else
	snapshot.binary_path = "Unknown";
#endif

	const auto state = g_capture_state.load(std::memory_order_relaxed);
	switch (state) {
		case CaptureState::Running: snapshot.capture_state_str = "Running"; break;
		case CaptureState::Completed: snapshot.capture_state_str = "Completed"; break;
		case CaptureState::Interrupted: snapshot.capture_state_str = "Interrupted"; break;
		case CaptureState::Scheduled: snapshot.capture_state_str = "Scheduled"; break;
		case CaptureState::WaitingForHotkey: snapshot.capture_state_str = "WaitingForHotkey"; break;
		case CaptureState::Idle:
		default: snapshot.capture_state_str = "Idle"; break;
	}

	snapshot.capture_phase_str =
	    GetExecutionPhaseName(g_current_phase.load(std::memory_order_relaxed));
	snapshot.trigger_reason              = g_trigger_reason.empty() ? "None" : g_trigger_reason;
	snapshot.capture_id                  = g_capture_id.empty() ? "unnamed" : g_capture_id;
	snapshot.capture_delay_sec           = g_capture_delay_sec.load(std::memory_order_relaxed);
	snapshot.capture_target_duration_sec = g_capture_duration_sec.load(std::memory_order_relaxed);

	if (state == CaptureState::Running) {
		snapshot.capture_real_duration_ms =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		                              Clock::now() - g_capture_start_time)
		                              .count());
	} else if (state == CaptureState::Completed || state == CaptureState::Interrupted) {
		snapshot.capture_real_duration_ms =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		                              g_capture_end_time - g_capture_start_time)
		                              .count());
	} else {
		snapshot.capture_real_duration_ms = 0;
	}

	snapshot.total_faults     = g_total_faults.load(std::memory_order_relaxed);
	snapshot.total_reprotects = g_total_reprotects.load(std::memory_order_relaxed);
	snapshot.total_handler_ns = g_total_handler_ns.load(std::memory_order_relaxed);
	snapshot.max_handler_ns   = g_max_handler_ns.load(std::memory_order_relaxed);
	if (snapshot.total_faults > 0) {
		snapshot.avg_handler_us =
		    static_cast<double>(snapshot.total_handler_ns) / (snapshot.total_faults * 1000.0);
	}

	for (size_t i = 0; i < static_cast<size_t>(GpuWaitReason::Count); ++i) {
		snapshot.waits[i].count    = g_wait_counts[i].load(std::memory_order_relaxed);
		snapshot.waits[i].total_ns = g_wait_nanos[i].load(std::memory_order_relaxed);
		snapshot.waits[i].max_ns   = g_wait_max_nanos[i].load(std::memory_order_relaxed);
	}

	for (size_t i = 0; i < static_cast<size_t>(SubmitReason::Count); ++i) {
		snapshot.submits[i].count = g_submit_counts[i].load(std::memory_order_relaxed);
	}

	// Memory transfer metrics: calculate deltas against baseline
	namespace Counters      = Common::DebugCounters;
	const auto cur_buf_up   = Counters::Get(Counters::Counter::BufferUploadBytes);
	const auto base_buf_up  = g_baseline_buffer_upload_bytes.load(std::memory_order_relaxed);
	const auto delta_buf_up = (cur_buf_up >= base_buf_up) ? (cur_buf_up - base_buf_up) : cur_buf_up;

	const auto cur_tex_up   = Counters::Get(Counters::Counter::TextureUploadBytes);
	const auto base_tex_up  = g_baseline_texture_upload_bytes.load(std::memory_order_relaxed);
	const auto delta_tex_up = (cur_tex_up >= base_tex_up) ? (cur_tex_up - base_tex_up) : cur_tex_up;

	snapshot.cpu_to_gpu_upload_bytes = delta_buf_up + delta_tex_up;

	const auto cur_buf_down  = Counters::Get(Counters::Counter::BufferDownloadBytes);
	const auto base_buf_down = g_baseline_buffer_download_bytes.load(std::memory_order_relaxed);
	const auto delta_buf_down =
	    (cur_buf_down >= base_buf_down) ? (cur_buf_down - base_buf_down) : cur_buf_down;

	const auto cur_tex_down  = Counters::Get(Counters::Counter::TextureDownloadBytes);
	const auto base_tex_down = g_baseline_texture_download_bytes.load(std::memory_order_relaxed);
	const auto delta_tex_down =
	    (cur_tex_down >= base_tex_down) ? (cur_tex_down - base_tex_down) : cur_tex_down;

	snapshot.gpu_to_cpu_readback_bytes = delta_buf_down + delta_tex_down;
	snapshot.neighbour_readback_count =
	    Counters::Get(Counters::Counter::BufferNeighbourDownloads) -
	    g_baseline_neighbour_downloads.load(std::memory_order_relaxed);
	snapshot.scratch_freed_bytes = Counters::Get(Counters::Counter::ScratchFreedBytes) -
	                               g_baseline_scratch_freed_bytes.load(std::memory_order_relaxed);
	snapshot.scratch_retirements = Counters::Get(Counters::Counter::ScratchRetirements) -
	                               g_baseline_scratch_retirements.load(std::memory_order_relaxed);
	snapshot.scratch_allocations = Counters::Get(Counters::Counter::ScratchAllocations) -
	                               g_baseline_scratch_allocations.load(std::memory_order_relaxed);
	snapshot.compact_readback_copies =
	    Counters::Get(Counters::Counter::CompactReadbackCopies) -
	    g_baseline_compact_readback_copies.load(std::memory_order_relaxed);
	snapshot.compact_readback_fallbacks =
	    Counters::Get(Counters::Counter::CompactReadbackFallbacks) -
	    g_baseline_compact_readback_fallbacks.load(std::memory_order_relaxed);
	snapshot.readback_prefill_avoided_bytes =
	    Counters::Get(Counters::Counter::ReadbackPrefillAvoidedBytes) -
	    g_baseline_readback_prefill_avoided_bytes.load(std::memory_order_relaxed);
	snapshot.readback_staging_saved_bytes =
	    Counters::Get(Counters::Counter::ReadbackStagingSavedBytes) -
	    g_baseline_readback_staging_saved_bytes.load(std::memory_order_relaxed);
	snapshot.read_buffer_hits     = Counters::Get(Counters::Counter::ReadBufferHits) -
	                                g_baseline_read_buffer_hits.load(std::memory_order_relaxed);
	snapshot.resource_prep_hits   = Counters::Get(Counters::Counter::ResourcePrepHits) -
	                                g_baseline_resource_prep_hits.load(std::memory_order_relaxed);
	snapshot.read_buffer_misses   = Counters::Get(Counters::Counter::ReadBufferMisses) -
	                                g_baseline_read_buffer_misses.load(std::memory_order_relaxed);
	snapshot.resource_prep_misses = Counters::Get(Counters::Counter::ResourcePrepMisses) -
	                                g_baseline_resource_prep_misses.load(std::memory_order_relaxed);
	snapshot.tile_layout_hits     = Counters::Get(Counters::Counter::TileLayoutHits) -
	                                g_baseline_tile_layout_hits.load(std::memory_order_relaxed);
	snapshot.tile_layout_misses   = Counters::Get(Counters::Counter::TileLayoutMisses) -
	                                g_baseline_tile_layout_misses.load(std::memory_order_relaxed);
	snapshot.binding_range_reuses = Counters::Get(Counters::Counter::BindingRangeReuses) -
	                                g_baseline_binding_reuses[0].load(std::memory_order_relaxed);
	snapshot.binding_lookup_reuses  = Counters::Get(Counters::Counter::BindingLookupReuses) -
	                                  g_baseline_binding_reuses[1].load(std::memory_order_relaxed);
	snapshot.binding_sampler_reuses = Counters::Get(Counters::Counter::BindingSamplerReuses) -
	                                  g_baseline_binding_reuses[2].load(std::memory_order_relaxed);
	snapshot.binding_upload_reuses  = Counters::Get(Counters::Counter::BindingUploadReuses) -
	                                  g_baseline_binding_reuses[3].load(std::memory_order_relaxed);
	snapshot.binding_upload_reuse_bytes =
	    Counters::Get(Counters::Counter::BindingUploadReuseBytes) -
	    g_baseline_binding_reuses[4].load(std::memory_order_relaxed);
	snapshot.neighbour_readback_bytes =
	    Counters::Get(Counters::Counter::BufferNeighbourDownloadBytes) -
	    g_baseline_neighbour_download_bytes.load(std::memory_order_relaxed);
	snapshot.internal_gpu_transfer_bytes =
	    g_internal_gpu_transfer_bytes.load(std::memory_order_relaxed);

	const auto cur_pref  = Counters::Get(Counters::Counter::TexturePartialRefreshes);
	const auto base_pref = g_baseline_texture_partial_refreshes.load(std::memory_order_relaxed);
	snapshot.texture_partial_refreshes =
	    (cur_pref >= base_pref) ? (cur_pref - base_pref) : cur_pref;

	const auto cur_pfall  = Counters::Get(Counters::Counter::TexturePartialFallbacks);
	const auto base_pfall = g_baseline_texture_partial_fallbacks.load(std::memory_order_relaxed);
	snapshot.texture_partial_fallbacks =
	    (cur_pfall >= base_pfall) ? (cur_pfall - base_pfall) : cur_pfall;

	const auto cur_pup  = Counters::Get(Counters::Counter::TexturePartialUploadBytes);
	const auto base_pup = g_baseline_texture_partial_upload_bytes.load(std::memory_order_relaxed);
	snapshot.texture_partial_upload_bytes = (cur_pup >= base_pup) ? (cur_pup - base_pup) : cur_pup;

	const auto cur_cheld       = Counters::Get(Counters::Counter::CadenceHeldBack);
	const auto base_cheld      = g_baseline_cadence_held_back.load(std::memory_order_relaxed);
	snapshot.cadence_held_back = (cur_cheld >= base_cheld) ? (cur_cheld - base_cheld) : cur_cheld;

	const auto cur_csub      = Counters::Get(Counters::Counter::CadenceSubmits);
	const auto base_csub     = g_baseline_cadence_submits.load(std::memory_order_relaxed);
	snapshot.cadence_submits = (cur_csub >= base_csub) ? (cur_csub - base_csub) : cur_csub;

	const auto cur_spins         = g_priority_wait_spins.load(std::memory_order_relaxed);
	const auto base_spins        = g_baseline_priority_wait_spins.load(std::memory_order_relaxed);
	snapshot.priority_wait_spins = (cur_spins >= base_spins) ? (cur_spins - base_spins) : cur_spins;

	const auto cur_spin_hits  = g_priority_wait_spin_hits.load(std::memory_order_relaxed);
	const auto base_spin_hits = g_baseline_priority_wait_spin_hits.load(std::memory_order_relaxed);
	snapshot.priority_wait_spin_hits =
	    (cur_spin_hits >= base_spin_hits) ? (cur_spin_hits - base_spin_hits) : cur_spin_hits;

	const auto cur_skips  = g_pending_ops_fast_skips.load(std::memory_order_relaxed);
	const auto base_skips = g_baseline_pending_ops_fast_skips.load(std::memory_order_relaxed);
	snapshot.pending_ops_fast_skips =
	    (cur_skips >= base_skips) ? (cur_skips - base_skips) : cur_skips;

	snapshot.cp_sequencer_enabled = Config::CpSequencerEnabled();

	const auto cur_cp_ops  = g_cp_sequencer_ops_emitted.load(std::memory_order_relaxed);
	const auto base_cp_ops = g_baseline_cp_sequencer_ops_emitted.load(std::memory_order_relaxed);
	snapshot.cp_sequencer_ops_emitted =
	    (cur_cp_ops >= base_cp_ops) ? (cur_cp_ops - base_cp_ops) : cur_cp_ops;

	const auto cur_cp_waits = g_cp_sequencer_lockstep_waits.load(std::memory_order_relaxed);
	const auto base_cp_waits =
	    g_baseline_cp_sequencer_lockstep_waits.load(std::memory_order_relaxed);
	snapshot.cp_sequencer_lockstep_waits =
	    (cur_cp_waits >= base_cp_waits) ? (cur_cp_waits - base_cp_waits) : cur_cp_waits;

	const auto cur_cp_snaps = g_cp_sequencer_snapshots_taken.load(std::memory_order_relaxed);
	const auto base_cp_snaps =
	    g_baseline_cp_sequencer_snapshots_taken.load(std::memory_order_relaxed);
	snapshot.cp_sequencer_snapshots_taken =
	    (cur_cp_snaps >= base_cp_snaps) ? (cur_cp_snaps - base_cp_snaps) : cur_cp_snaps;

	const auto cur_cp_spins  = g_cp_sequencer_waits_spin.load(std::memory_order_relaxed);
	const auto base_cp_spins = g_baseline_cp_sequencer_waits_spin.load(std::memory_order_relaxed);
	snapshot.cp_sequencer_waits_spin =
	    (cur_cp_spins >= base_cp_spins) ? (cur_cp_spins - base_cp_spins) : cur_cp_spins;

	snapshot.draw_prep_enabled = Config::DrawPrepEnabled();

	const auto cur_dp_pub  = g_draw_prep_published.load(std::memory_order_relaxed);
	const auto base_dp_pub = g_baseline_draw_prep_published.load(std::memory_order_relaxed);
	snapshot.draw_prep_published =
	    (cur_dp_pub >= base_dp_pub) ? (cur_dp_pub - base_dp_pub) : cur_dp_pub;

	const auto cur_dp_prep  = g_draw_prep_prepared.load(std::memory_order_relaxed);
	const auto base_dp_prep = g_baseline_draw_prep_prepared.load(std::memory_order_relaxed);
	snapshot.draw_prep_prepared =
	    (cur_dp_prep >= base_dp_prep) ? (cur_dp_prep - base_dp_prep) : cur_dp_prep;

	const auto cur_dp_comm  = g_draw_prep_committed.load(std::memory_order_relaxed);
	const auto base_dp_comm = g_baseline_draw_prep_committed.load(std::memory_order_relaxed);
	snapshot.draw_prep_committed =
	    (cur_dp_comm >= base_dp_comm) ? (cur_dp_comm - base_dp_comm) : cur_dp_comm;

	const auto cur_dp_fall  = g_draw_prep_fallback.load(std::memory_order_relaxed);
	const auto base_dp_fall = g_baseline_draw_prep_fallback.load(std::memory_order_relaxed);
	snapshot.draw_prep_fallback =
	    (cur_dp_fall >= base_dp_fall) ? (cur_dp_fall - base_dp_fall) : cur_dp_fall;

	const auto cur_dp_avd  = g_draw_prep_avoided.load(std::memory_order_relaxed);
	const auto base_dp_avd = g_baseline_draw_prep_avoided.load(std::memory_order_relaxed);
	snapshot.draw_prep_avoided =
	    (cur_dp_avd >= base_dp_avd) ? (cur_dp_avd - base_dp_avd) : cur_dp_avd;

	const auto cur_dp_avd_ns = g_draw_prep_avoided_time_ns.load(std::memory_order_relaxed);
	const auto base_dp_avd_ns =
	    g_baseline_draw_prep_avoided_time_ns.load(std::memory_order_relaxed);
	snapshot.draw_prep_avoided_time_ns =
	    (cur_dp_avd_ns >= base_dp_avd_ns) ? (cur_dp_avd_ns - base_dp_avd_ns) : cur_dp_avd_ns;

	if (snapshot.draw_prep_committed > 0) {
		const auto total_req = snapshot.draw_prep_committed;
		const auto avoided   = std::min(snapshot.draw_prep_avoided, total_req);
		snapshot.draw_prep_duplication_pct =
		    (1.0 - (static_cast<double>(avoided) / static_cast<double>(total_req))) * 100.0;
	} else {
		snapshot.draw_prep_duplication_pct = 0.0;
	}

	// Frametimes calculation across the captured window
	const auto total_recorded_frames = g_captured_frame_count.load(std::memory_order_relaxed);
	snapshot.total_frames            = total_recorded_frames;
	const size_t valid_count         = std::min<size_t>(total_recorded_frames, MAX_CAPTURED_FRAMES);

	if (valid_count > 0) {
		std::vector<double> samples;
		samples.reserve(valid_count);
		double sum_ms = 0.0;
		double min_ms = 1e9;
		double max_ms = 0.0;

		for (size_t i = 0; i < valid_count; ++i) {
			const double ms = g_captured_frames[i];
			if (ms > 0.0) {
				samples.push_back(ms);
				sum_ms += ms;
				min_ms = std::min(min_ms, ms);
				max_ms = std::max(max_ms, ms);
			}
		}

		if (!samples.empty()) {
			std::sort(samples.begin(), samples.end());
			snapshot.avg_frame_ms = sum_ms / static_cast<double>(samples.size());
			snapshot.min_frame_ms = min_ms;
			snapshot.max_frame_ms = max_ms;
			snapshot.p50_frame_ms = samples[samples.size() * 50 / 100];
			snapshot.p90_frame_ms = samples[samples.size() * 90 / 100];
			snapshot.p95_frame_ms = samples[samples.size() * 95 / 100];
			snapshot.p99_frame_ms = samples[samples.size() * 99 / 100];

			if (snapshot.capture_real_duration_ms > 0) {
				snapshot.effective_fps = static_cast<double>(total_recorded_frames) /
				                         (snapshot.capture_real_duration_ms / 1000.0);
			}
			if (snapshot.avg_frame_ms > 0.0) {
				snapshot.frametime_fps = 1000.0 / snapshot.avg_frame_ms;
			}
		}
	}

	// Extract stuttering and hitch metrics
	snapshot.hitches_ge_50ms   = g_hitches_50ms.load(std::memory_order_relaxed);
	snapshot.hitches_ge_100ms  = g_hitches_100ms.load(std::memory_order_relaxed);
	snapshot.hitches_ge_250ms  = g_hitches_250ms.load(std::memory_order_relaxed);
	snapshot.hitches_ge_500ms  = g_hitches_500ms.load(std::memory_order_relaxed);
	snapshot.hitches_ge_1000ms = g_hitches_1000ms.load(std::memory_order_relaxed);
	snapshot.hitch_time_total_ms =
	    static_cast<double>(g_hitch_time_ns.load(std::memory_order_relaxed)) / 1e6;

	const auto slow_count =
	    std::min<size_t>(g_slow_frame_cursor.load(std::memory_order_relaxed), MAX_SLOW_FRAMES);
	for (size_t i = 0; i < slow_count; ++i) {
		const double ms = g_slow_frames[i].frame_ms.load(std::memory_order_relaxed);
		if (ms > 0.0) {
			snapshot.slow_frames.push_back(
			    {g_slow_frames[i].frame_index.load(std::memory_order_relaxed), ms,
			     g_slow_frames[i].timestamp_ms.load(std::memory_order_relaxed)});
		}
	}
	std::sort(
	    snapshot.slow_frames.begin(), snapshot.slow_frames.end(),
	    [](const SlowFrameInfo& a, const SlowFrameInfo& b) { return a.frame_ms > b.frame_ms; });

	// Extract top pages
	for (size_t i = 0; i < PAGE_TABLE_CAPACITY; ++i) {
		const uint64_t page       = g_page_table[i].page.load(std::memory_order_relaxed);
		const uint64_t faults     = g_page_table[i].faults.load(std::memory_order_relaxed);
		const uint64_t reprotects = g_page_table[i].reprotects.load(std::memory_order_relaxed);
		if (page != 0 && faults > 0) {
			snapshot.top_pages.push_back({page, faults, reprotects});
		}
	}
	std::sort(snapshot.top_pages.begin(), snapshot.top_pages.end(),
	          [](const HotPageInfo& a, const HotPageInfo& b) {
		          if (a.fault_count != b.fault_count) {
			          return a.fault_count > b.fault_count;
		          }
		          return a.reprotect_count > b.reprotect_count;
	          });
	if (snapshot.top_pages.size() > 50) {
		snapshot.top_pages.resize(50);
	}

	// Extract top RIPs
	for (size_t i = 0; i < RIP_TABLE_CAPACITY; ++i) {
		const uint64_t rip   = g_rip_table[i].rip.load(std::memory_order_relaxed);
		const uint64_t count = g_rip_table[i].count.load(std::memory_order_relaxed);
		if (rip != 0 && count > 0) {
			snapshot.top_rips.push_back({rip, count});
		}
	}
	std::sort(
	    snapshot.top_rips.begin(), snapshot.top_rips.end(),
	    [](const HotRipInfo& a, const HotRipInfo& b) { return a.fault_count > b.fault_count; });
	if (snapshot.top_rips.size() > 50) {
		snapshot.top_rips.resize(50);
	}

	// Extract top threads
	for (size_t i = 0; i < THREAD_TABLE_CAPACITY; ++i) {
		const uint32_t tid   = g_thread_table[i].tid.load(std::memory_order_relaxed);
		const uint64_t count = g_thread_table[i].count.load(std::memory_order_relaxed);
		if (tid != 0 && count > 0) {
			HotThreadInfo info {tid, count};
			std::memcpy(info.name, g_thread_table[i].name, sizeof(info.name));
			snapshot.top_threads.push_back(info);
		}
	}
	std::sort(snapshot.top_threads.begin(), snapshot.top_threads.end(),
	          [](const HotThreadInfo& a, const HotThreadInfo& b) {
		          return a.fault_count > b.fault_count;
	          });

	// Populate phase-separated metrics
	snapshot.phase_loading.frame_count =
	    g_phase_loading.frame_count.load(std::memory_order_relaxed);
	snapshot.phase_loading.total_frame_ms =
	    static_cast<double>(g_phase_loading.total_frame_ns.load(std::memory_order_relaxed)) /
	    1000000.0;
	snapshot.phase_loading.faults     = g_phase_loading.faults.load(std::memory_order_relaxed);
	snapshot.phase_loading.reprotects = g_phase_loading.reprotects.load(std::memory_order_relaxed);
	snapshot.phase_loading.submits    = g_phase_loading.submits.load(std::memory_order_relaxed);
	snapshot.phase_loading.gpu_wait_ns =
	    g_phase_loading.gpu_wait_ns.load(std::memory_order_relaxed);

	snapshot.phase_gameplay.frame_count =
	    g_phase_gameplay.frame_count.load(std::memory_order_relaxed);
	snapshot.phase_gameplay.total_frame_ms =
	    static_cast<double>(g_phase_gameplay.total_frame_ns.load(std::memory_order_relaxed)) /
	    1000000.0;
	snapshot.phase_gameplay.faults = g_phase_gameplay.faults.load(std::memory_order_relaxed);
	snapshot.phase_gameplay.reprotects =
	    g_phase_gameplay.reprotects.load(std::memory_order_relaxed);
	snapshot.phase_gameplay.submits = g_phase_gameplay.submits.load(std::memory_order_relaxed);
	snapshot.phase_gameplay.gpu_wait_ns =
	    g_phase_gameplay.gpu_wait_ns.load(std::memory_order_relaxed);

	// Timing and simulation metrics
	const double snap_duration_sec =
	    snapshot.capture_real_duration_ms > 0 ? (snapshot.capture_real_duration_ms / 1000.0) : 0.0;

	snapshot.total_vblanks = g_vblank_count.load(std::memory_order_relaxed);
	snapshot.vblank_rate_hz =
	    snap_duration_sec > 0.0 ? (static_cast<double>(snapshot.total_vblanks) / snap_duration_sec)
	                            : 0.0;
	snapshot.total_flips_submitted = g_flip_submitted_count.load(std::memory_order_relaxed);
	snapshot.total_flips_completed = g_flip_completed_count.load(std::memory_order_relaxed);
	snapshot.flip_rate_hz =
	    snap_duration_sec > 0.0
	        ? (static_cast<double>(snapshot.total_flips_completed) / snap_duration_sec)
	        : 0.0;
	snapshot.total_flip_wait_ns = g_flip_wait_ns.load(std::memory_order_relaxed);
	snapshot.total_flip_wait_ms = static_cast<double>(snapshot.total_flip_wait_ns) / 1000000.0;
	const auto wait_count       = g_flip_wait_count.load(std::memory_order_relaxed);
	snapshot.avg_flip_wait_ms =
	    wait_count > 0 ? (snapshot.total_flip_wait_ms / static_cast<double>(wait_count)) : 0.0;
	snapshot.audio_updates_count = g_audio_updates_count.load(std::memory_order_relaxed);
	snapshot.audio_total_samples = g_audio_total_samples.load(std::memory_order_relaxed);
	snapshot.audio_effective_hz =
	    snap_duration_sec > 0.0
	        ? (static_cast<double>(snapshot.audio_total_samples) / snap_duration_sec)
	        : 0.0;
	snapshot.simulation_speed_ratio =
	    snap_duration_sec > 0.0
	        ? ((static_cast<double>(snapshot.total_flips_completed) * (1.0 / 60.0)) /
	           snap_duration_sec)
	        : 0.0;

	// Graphics workload metrics
	const auto cur_draws  = Counters::Get(Counters::Counter::Draws);
	const auto base_draws = g_baseline_draws.load(std::memory_order_relaxed);
	snapshot.total_draws  = (cur_draws >= base_draws) ? (cur_draws - base_draws) : cur_draws;
	const double snap_frames =
	    snapshot.total_frames > 0 ? static_cast<double>(snapshot.total_frames) : 1.0;
	snapshot.draws_per_frame = static_cast<double>(snapshot.total_draws) / snap_frames;
	snapshot.draws_per_second =
	    snap_duration_sec > 0.0 ? (static_cast<double>(snapshot.total_draws) / snap_duration_sec)
	                            : 0.0;

	const auto cur_inst           = Counters::Get(Counters::Counter::DrawInstances);
	const auto base_inst          = g_baseline_draw_instances.load(std::memory_order_relaxed);
	snapshot.total_draw_instances = (cur_inst >= base_inst) ? (cur_inst - base_inst) : cur_inst;
	snapshot.instances_per_frame = static_cast<double>(snapshot.total_draw_instances) / snap_frames;

	const auto cur_ind            = Counters::Get(Counters::Counter::IndirectDraws);
	const auto base_ind           = g_baseline_indirect_draws.load(std::memory_order_relaxed);
	snapshot.total_indirect_draws = (cur_ind >= base_ind) ? (cur_ind - base_ind) : cur_ind;

	const auto cur_disp           = Counters::Get(Counters::Counter::Dispatches);
	const auto base_disp          = g_baseline_dispatches.load(std::memory_order_relaxed);
	snapshot.total_dispatches     = (cur_disp >= base_disp) ? (cur_disp - base_disp) : cur_disp;
	snapshot.dispatches_per_frame = static_cast<double>(snapshot.total_dispatches) / snap_frames;
	snapshot.dispatches_per_second =
	    snap_duration_sec > 0.0
	        ? (static_cast<double>(snapshot.total_dispatches) / snap_duration_sec)
	        : 0.0;

	const auto cur_pipe        = Counters::Get(Counters::Counter::PipelinesCreated);
	const auto base_pipe       = g_baseline_pipelines_created.load(std::memory_order_relaxed);
	snapshot.pipelines_created = (cur_pipe >= base_pipe) ? (cur_pipe - base_pipe) : cur_pipe;

	const auto cur_shad       = Counters::Get(Counters::Counter::ShadersCompiled);
	const auto base_shad      = g_baseline_shaders_compiled.load(std::memory_order_relaxed);
	snapshot.shaders_compiled = (cur_shad >= base_shad) ? (cur_shad - base_shad) : cur_shad;

	const auto cur_busy         = Counters::Get(Counters::Counter::GpuThreadBusyNs);
	const auto base_busy        = g_baseline_gpu_thread_busy_ns.load(std::memory_order_relaxed);
	snapshot.gpu_thread_busy_ns = (cur_busy >= base_busy) ? (cur_busy - base_busy) : cur_busy;
	snapshot.gpu_thread_busy_ms = static_cast<double>(snapshot.gpu_thread_busy_ns) / 1000000.0;
	snapshot.gpu_thread_busy_ms_per_frame = snapshot.gpu_thread_busy_ms / snap_frames;

	const auto cur_lookups  = Counters::Get(Counters::Counter::PipelineCacheLookups);
	const auto base_lookups = g_baseline_pipeline_cache_lookups.load(std::memory_order_relaxed);
	snapshot.pipeline_cache_lookups =
	    (cur_lookups >= base_lookups) ? (cur_lookups - base_lookups) : cur_lookups;

	const auto cur_hits          = Counters::Get(Counters::Counter::PipelineCacheHits);
	const auto base_hits         = g_baseline_pipeline_cache_hits.load(std::memory_order_relaxed);
	snapshot.pipeline_cache_hits = (cur_hits >= base_hits) ? (cur_hits - base_hits) : cur_hits;
	snapshot.pipeline_cache_hit_pct =
	    snapshot.pipeline_cache_lookups > 0
	        ? (100.0 * static_cast<double>(snapshot.pipeline_cache_hits) /
	           static_cast<double>(snapshot.pipeline_cache_lookups))
	        : 0.0;

	const auto cur_red_pipe  = Counters::Get(Counters::Counter::RedundantPipelineBinds);
	const auto base_red_pipe = g_baseline_redundant_pipeline_binds.load(std::memory_order_relaxed);
	snapshot.redundant_pipeline_binds =
	    (cur_red_pipe >= base_red_pipe) ? (cur_red_pipe - base_red_pipe) : cur_red_pipe;

	const auto cur_red_vert  = Counters::Get(Counters::Counter::RedundantVertexBinds);
	const auto base_red_vert = g_baseline_redundant_vertex_binds.load(std::memory_order_relaxed);
	snapshot.redundant_vertex_binds =
	    (cur_red_vert >= base_red_vert) ? (cur_red_vert - base_red_vert) : cur_red_vert;

	const auto cur_red_idx  = Counters::Get(Counters::Counter::RedundantIndexBinds);
	const auto base_red_idx = g_baseline_redundant_index_binds.load(std::memory_order_relaxed);
	snapshot.redundant_index_binds =
	    (cur_red_idx >= base_red_idx) ? (cur_red_idx - base_red_idx) : cur_red_idx;

	// Stage 8 additions: Pipeline stages breakdown
	double total_pipeline_ms = 0.0;
	for (size_t i = 0; i < static_cast<size_t>(PipelineStage::Count); ++i) {
		const auto   count    = g_stage_counts[i].load(std::memory_order_relaxed);
		const auto   total_ns = g_stage_nanos[i].load(std::memory_order_relaxed);
		const auto   max_ns   = g_stage_max_nanos[i].load(std::memory_order_relaxed);
		const auto   min_ns   = g_stage_min_nanos[i].load(std::memory_order_relaxed);
		const double ms       = static_cast<double>(total_ns) / 1e6;

		snapshot.pipeline_stages[i].count    = count;
		snapshot.pipeline_stages[i].total_ns = total_ns;
		snapshot.pipeline_stages[i].max_ns   = max_ns;
		snapshot.pipeline_stages[i].min_ns   = (count > 0) ? min_ns : 0;
		snapshot.pipeline_stages[i].total_ms = ms;
		snapshot.pipeline_stages[i].avg_us =
		    count > 0 ? (static_cast<double>(total_ns) / (count * 1000.0)) : 0.0;
		snapshot.pipeline_stages[i].max_ms       = static_cast<double>(max_ns) / 1e6;
		snapshot.pipeline_stages[i].ms_per_frame = snap_frames > 0.0 ? (ms / snap_frames) : 0.0;

		total_pipeline_ms += ms;
	}
	for (size_t i = 0; i < static_cast<size_t>(PipelineStage::Count); ++i) {
		snapshot.pipeline_stages[i].pct_thread_time =
		    total_pipeline_ms > 0.0
		        ? (100.0 * snapshot.pipeline_stages[i].total_ms / total_pipeline_ms)
		        : 0.0;
	}

	// Stage 8 additions: CPU / GPU execution separation
	const auto wall_start = g_gpu_thread_wall_start_ns.load(std::memory_order_relaxed);
	const auto wall_end   = g_gpu_thread_wall_end_ns.load(std::memory_order_relaxed);
	const auto cpu_start  = g_gpu_thread_cpu_start_ns.load(std::memory_order_relaxed);
	const auto cpu_end    = g_gpu_thread_cpu_end_ns.load(std::memory_order_relaxed);

	if (wall_end > wall_start) {
		snapshot.gpu_thread_wall_clock_ms = static_cast<double>(wall_end - wall_start) / 1e6;
	} else if (snapshot.capture_real_duration_ms > 0) {
		snapshot.gpu_thread_wall_clock_ms = static_cast<double>(snapshot.capture_real_duration_ms);
	} else {
		const auto cap_now =
		    (state == CaptureState::Completed || state == CaptureState::Interrupted)
		        ? g_capture_end_time
		        : Clock::now();
		if (g_capture_start_time.time_since_epoch().count() > 0 &&
		    cap_now >= g_capture_start_time) {
			const auto nanos =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(cap_now - g_capture_start_time)
			        .count();
			snapshot.gpu_thread_wall_clock_ms = std::max(0.001, static_cast<double>(nanos) / 1e6);
		}
	}

	const auto current_gpu_cpu = SampleGpuThreadCpuTimeNs();
	const auto final_cpu_end   = (current_gpu_cpu > cpu_start) ? current_gpu_cpu : cpu_end;
	if (final_cpu_end > cpu_start) {
		snapshot.gpu_thread_cpu_time_ms = static_cast<double>(final_cpu_end - cpu_start) / 1e6;
	} else {
		snapshot.gpu_thread_cpu_time_ms = snapshot.gpu_thread_busy_ms;
	}

	if (snapshot.gpu_thread_wall_clock_ms > 0.0) {
		snapshot.gpu_thread_cpu_utilization_pct = std::min(
		    100.0, 100.0 * (snapshot.gpu_thread_cpu_time_ms / snapshot.gpu_thread_wall_clock_ms));
	}

	uint64_t total_wait_ns = 0;
	for (size_t i = 0; i < static_cast<size_t>(GpuWaitReason::Count); ++i) {
		total_wait_ns += g_wait_nanos[i].load(std::memory_order_relaxed);
	}
	snapshot.gpu_thread_wait_time_ms = static_cast<double>(total_wait_ns) / 1e6;

	snapshot.gpu_execution_time_ms =
	    static_cast<double>(g_gpu_execution_total_ns.load(std::memory_order_relaxed)) / 1e6;
	snapshot.gpu_queries_recorded = g_gpu_execution_count.load(std::memory_order_relaxed);

	// Stage 8.1: PM4 Dispatch inclusive vs. exclusive breakdown
	snapshot.pm4_dispatch_inclusive_ms =
	    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Dispatch)].total_ms;
	const double inner_stages_ms =
	    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::RenderStatePrep)].total_ms +
	    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::DescriptorPrep)].total_ms +
	    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::BarriersTransitions)].total_ms +
	    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::BufferPrep)].total_ms +
	    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::CommandRecording)].total_ms;
	snapshot.pm4_dispatch_exclusive_ms =
	    (snapshot.pm4_dispatch_inclusive_ms > inner_stages_ms)
	        ? (snapshot.pm4_dispatch_inclusive_ms - inner_stages_ms)
	        : 0.0;
	if (snapshot.total_frames > 0) {
		snapshot.pm4_dispatch_exclusive_ms_per_frame =
		    snapshot.pm4_dispatch_exclusive_ms / static_cast<double>(snapshot.total_frames);
	}

	// Tripartite workload distribution: Queue vs CPU Active vs GPU Hardware Wait
	{
		auto&      td        = snapshot.time_distribution;
		const auto q_sub_idx = static_cast<size_t>(QueueLatencyType::Submission);
		const auto q_int_idx = static_cast<size_t>(QueueLatencyType::Intake);
		const auto q_op_idx  = static_cast<size_t>(QueueLatencyType::OpStream);
		const auto q_bp_idx  = static_cast<size_t>(QueueLatencyType::Backpressure);

		td.queue_submission_count =
		    g_queue_latency_count[q_sub_idx].load(std::memory_order_relaxed);
		td.queue_submission_ms =
		    static_cast<double>(g_queue_latency_ns[q_sub_idx].load(std::memory_order_relaxed)) /
		    1e6;
		td.queue_intake_count = g_queue_latency_count[q_int_idx].load(std::memory_order_relaxed);
		td.queue_intake_ms =
		    static_cast<double>(g_queue_latency_ns[q_int_idx].load(std::memory_order_relaxed)) /
		    1e6;
		td.queue_opstream_count = g_queue_latency_count[q_op_idx].load(std::memory_order_relaxed);
		td.queue_opstream_ms =
		    static_cast<double>(g_queue_latency_ns[q_op_idx].load(std::memory_order_relaxed)) / 1e6;
		td.queue_backpressure_count =
		    g_queue_latency_count[q_bp_idx].load(std::memory_order_relaxed);
		td.queue_backpressure_ms =
		    static_cast<double>(g_queue_latency_ns[q_bp_idx].load(std::memory_order_relaxed)) /
		        1e6 +
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::SequencerWait)].total_ms +
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::DrawPrepCommitWait)]
		        .total_ms;

		td.total_queue_ms = td.queue_submission_ms + td.queue_intake_ms + td.queue_opstream_ms +
		                    td.queue_backpressure_ms;

		td.cpu_pm4_decode_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Decode)].total_ms;
		td.cpu_pm4_dispatch_ms = snapshot.pm4_dispatch_exclusive_ms;
		td.cpu_render_state_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::RenderStatePrep)].total_ms;
		td.cpu_pipeline_keys_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::PipelineKeyBuild)]
		        .total_ms +
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::PipelineLookup)].total_ms;
		td.cpu_descriptors_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::DescriptorPrep)].total_ms;
		td.cpu_buffers_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::BufferPrep)].total_ms;
		td.cpu_command_record_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::CommandRecording)]
		        .total_ms +
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::CommandEnd)].total_ms;
		td.cpu_submit_overhead_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::Submit)].total_ms;
		td.cpu_draw_prep_worker_ms =
		    snapshot.pipeline_stages[static_cast<size_t>(PipelineStage::DrawPrepWorker)].total_ms;

		td.total_cpu_active_ms =
		    td.cpu_pm4_decode_ms + td.cpu_pm4_dispatch_ms + td.cpu_render_state_ms +
		    td.cpu_pipeline_keys_ms + td.cpu_descriptors_ms + td.cpu_buffers_ms +
		    td.cpu_command_record_ms + td.cpu_submit_overhead_ms + td.cpu_draw_prep_worker_ms;

		td.gpu_timeline_wait_ms =
		    static_cast<double>(
		        g_wait_nanos[static_cast<size_t>(GpuWaitReason::PriorityOperation)].load(
		            std::memory_order_relaxed) +
		        g_wait_nanos[static_cast<size_t>(GpuWaitReason::CommandSchedulerDrain)].load(
		            std::memory_order_relaxed)) /
		    1e6;
		td.gpu_streambuffer_wait_ms =
		    static_cast<double>(
		        g_wait_nanos[static_cast<size_t>(GpuWaitReason::StreamBufferWait)].load(
		            std::memory_order_relaxed)) /
		    1e6;
		td.gpu_swapchain_wait_ms =
		    static_cast<double>(g_wait_nanos[static_cast<size_t>(GpuWaitReason::Swapchain)].load(
		        std::memory_order_relaxed)) /
		    1e6;
		td.gpu_readback_wait_ms =
		    static_cast<double>(
		        g_wait_nanos[static_cast<size_t>(GpuWaitReason::BufferReadback)].load(
		            std::memory_order_relaxed) +
		        g_wait_nanos[static_cast<size_t>(GpuWaitReason::TextureWriteBack)].load(
		            std::memory_order_relaxed)) /
		    1e6;
		td.total_gpu_wait_ms       = snapshot.gpu_thread_wait_time_ms;
		const double accounted_gpu = td.gpu_timeline_wait_ms + td.gpu_streambuffer_wait_ms +
		                             td.gpu_swapchain_wait_ms + td.gpu_readback_wait_ms;
		td.gpu_other_wait_ms =
		    (td.total_gpu_wait_ms > accounted_gpu) ? (td.total_gpu_wait_ms - accounted_gpu) : 0.0;

		td.total_workload_ms = td.total_queue_ms + td.total_cpu_active_ms + td.total_gpu_wait_ms;
		if (td.total_workload_ms > 0.0) {
			td.queue_pct      = 100.0 * (td.total_queue_ms / td.total_workload_ms);
			td.cpu_active_pct = 100.0 * (td.total_cpu_active_ms / td.total_workload_ms);
			td.gpu_wait_pct   = 100.0 * (td.total_gpu_wait_ms / td.total_workload_ms);

			if (td.gpu_wait_pct >= 50.0 || (td.total_gpu_wait_ms > td.total_cpu_active_ms &&
			                                td.total_gpu_wait_ms > td.total_queue_ms)) {
				td.bottleneck = WorkloadTimeDistribution::Bottleneck::GpuBound;
			} else if (td.cpu_active_pct >= 50.0 ||
			           (td.total_cpu_active_ms > td.total_gpu_wait_ms &&
			            td.total_cpu_active_ms > td.total_queue_ms)) {
				td.bottleneck = WorkloadTimeDistribution::Bottleneck::CpuBound;
			} else if (td.queue_pct >= 35.0 || (td.total_queue_ms > td.total_cpu_active_ms &&
			                                    td.total_queue_ms > td.total_gpu_wait_ms)) {
				td.bottleneck = WorkloadTimeDistribution::Bottleneck::QueueSyncBound;
			}
		}
	}

	// Stage 8 additions: StreamBuffer audit
	snapshot.stream_buffer_stats.total_allocs =
	    g_stream_buffer_allocs.load(std::memory_order_relaxed);
	snapshot.stream_buffer_stats.total_wraps =
	    g_stream_buffer_wraps.load(std::memory_order_relaxed);
	snapshot.stream_buffer_stats.total_bytes =
	    g_stream_buffer_bytes.load(std::memory_order_relaxed);
	snapshot.stream_buffer_stats.high_water_bytes =
	    g_stream_buffer_high_water.load(std::memory_order_relaxed);
	snapshot.stream_buffer_stats.wait_events =
	    g_stream_buffer_wait_events.load(std::memory_order_relaxed);
	snapshot.stream_buffer_stats.wait_total_ns =
	    g_stream_buffer_wait_total_ns.load(std::memory_order_relaxed);
	snapshot.stream_buffer_stats.wait_max_ns =
	    g_stream_buffer_wait_max_ns.load(std::memory_order_relaxed);
	snapshot.stream_buffer_stats.last_wait_tick =
	    g_stream_buffer_last_wait_tick.load(std::memory_order_relaxed);

	snapshot.stream_buffer_stats.total_mb =
	    static_cast<double>(snapshot.stream_buffer_stats.total_bytes) / (1024.0 * 1024.0);
	snapshot.stream_buffer_stats.mb_per_frame =
	    snap_frames > 0.0 ? (snapshot.stream_buffer_stats.total_mb / snap_frames) : 0.0;
	snapshot.stream_buffer_stats.high_water_mb =
	    static_cast<double>(snapshot.stream_buffer_stats.high_water_bytes) / (1024.0 * 1024.0);
	snapshot.stream_buffer_stats.wait_total_ms =
	    static_cast<double>(snapshot.stream_buffer_stats.wait_total_ns) / 1e6;
	snapshot.stream_buffer_stats.wait_avg_ms =
	    snapshot.stream_buffer_stats.wait_events > 0
	        ? (snapshot.stream_buffer_stats.wait_total_ms /
	           static_cast<double>(snapshot.stream_buffer_stats.wait_events))
	        : 0.0;
	snapshot.stream_buffer_stats.wait_max_ms =
	    static_cast<double>(snapshot.stream_buffer_stats.wait_max_ns) / 1e6;

	// Stage 8 additions: Detailed hitch correlation with temporal filtering
	const uint64_t total_trace_events = g_trace_event_cursor.load(std::memory_order_relaxed);
	const size_t trace_ring_count = std::min<size_t>(total_trace_events, TRACE_EVENT_RING_CAPACITY);

	for (const auto& sf: snapshot.slow_frames) {
		DetailedHitchInfo hitch {};
		hitch.frame_index  = sf.frame_index;
		hitch.frame_ms     = sf.frame_ms;
		hitch.timestamp_ms = sf.timestamp_ms;

		const double sf_end_ms   = static_cast<double>(sf.timestamp_ms);
		const double sf_start_ms = (sf_end_ms > sf.frame_ms) ? (sf_end_ms - sf.frame_ms) : 0.0;

		uint64_t best_duration_ns = 0;
		size_t   best_event_idx   = SIZE_MAX;

		for (size_t e = 0; e < trace_ring_count; ++e) {
			const auto& evt = g_trace_event_ring[e];
			if (evt.type == TraceEventType::None) continue;
			const double evt_end_ms   = static_cast<double>(evt.timestamp_us) / 1000.0;
			const double evt_dur_ms   = static_cast<double>(evt.duration_ns) / 1e6;
			const double evt_start_ms = (evt_end_ms > evt_dur_ms) ? (evt_end_ms - evt_dur_ms) : 0.0;

			// Check temporal overlap with slow frame window (+- 15 ms margin)
			if (evt_end_ms >= (sf_start_ms - 15.0) && evt_start_ms <= (sf_end_ms + 15.0)) {
				if (evt.duration_ns > best_duration_ns) {
					best_duration_ns = evt.duration_ns;
					best_event_idx   = e;
				}
			}
		}

		if (best_event_idx != SIZE_MAX) {
			const auto& best_evt      = g_trace_event_ring[best_event_idx];
			hitch.dominant_blocker    = GetTraceEventTypeName(best_evt.type);
			hitch.blocker_duration_ms = static_cast<double>(best_evt.duration_ns) / 1e6;
			hitch.blocker_thread_id   = best_evt.tid;
			hitch.blocker_tick        = best_evt.tick;
			hitch.blocker_address     = best_evt.data;

			const double ratio =
			    (sf.frame_ms > 0.0) ? (hitch.blocker_duration_ms / sf.frame_ms) : 0.0;
			if (ratio >= 0.70) {
				hitch.correlation_confidence = "High";
			} else if (ratio >= 0.30) {
				hitch.correlation_confidence = "Medium";
			} else {
				hitch.correlation_confidence = "Low";
			}
		} else {
			hitch.dominant_blocker       = "ComputeOrRasterBound";
			hitch.blocker_duration_ms    = sf.frame_ms;
			hitch.blocker_thread_id      = 0;
			hitch.blocker_tick           = 0;
			hitch.blocker_address        = 0;
			hitch.correlation_confidence = "Low";
		}

		snapshot.detailed_hitches.push_back(hitch);
	}

	return snapshot;
}

const char* GetWaitReasonName(GpuWaitReason reason) noexcept {
	switch (reason) {
		case GpuWaitReason::TextureWriteBack: return "TextureWriteBack";
		case GpuWaitReason::BufferReadback: return "BufferReadback";
		case GpuWaitReason::BufferGarbageCollection: return "BufferGarbageCollection";
		case GpuWaitReason::BufferSuspendFlush: return "BufferSuspendFlush";
		case GpuWaitReason::StreamBufferWait: return "StreamBufferWait";
		case GpuWaitReason::OcclusionQueries: return "OcclusionQueries";
		case GpuWaitReason::DescriptorHeap: return "DescriptorHeap";
		case GpuWaitReason::CommandSchedulerDrain: return "CommandSchedulerDrain";
		case GpuWaitReason::PriorityOperation: return "PriorityOperation";
		case GpuWaitReason::RayTracing: return "RayTracing";
		case GpuWaitReason::Swapchain: return "Swapchain";
		case GpuWaitReason::FaultManager: return "FaultManager";
		case GpuWaitReason::Other:
		default: return "Other";
	}
}

const char* GetSubmitReasonName(SubmitReason reason) noexcept {
	switch (reason) {
		case SubmitReason::Cadence: return "Cadence";
		case SubmitReason::ExplicitFlush: return "ExplicitFlush";
		case SubmitReason::Synchronization: return "Synchronization";
		case SubmitReason::Presentation: return "Presentation";
		case SubmitReason::Drain: return "Drain";
		case SubmitReason::Other:
		default: return "Other";
	}
}

bool ExportJson(const std::string& filepath, const ProfileSnapshot* existing_snap) {
	const auto     snap = existing_snap != nullptr ? *existing_snap : TakeSnapshot();
	nlohmann::json j;

	j["metadata"] = {
	    {"capture_id", snap.capture_id},
	    {"capture_state", snap.capture_state_str},
	    {"capture_phase", snap.capture_phase_str},
	    {"trigger_reason", snap.trigger_reason},
	    {"real_duration_ms", snap.capture_real_duration_ms},
	    {"real_duration_seconds", snap.capture_real_duration_ms / 1000.0},
	    {"target_duration_seconds", snap.capture_target_duration_sec},
	    {"delay_seconds", snap.capture_delay_sec},
	    {"git_commit", snap.git_commit},
	    {"git_revision", snap.git_revision},
	    {"git_hash", snap.git_hash},
	    {"build_timestamp", snap.build_timestamp},
	    {"compiler", snap.compiler},
	    {"binary_path", snap.binary_path},
	    {"config",
	     {
	         {"gpu_submit_cadence", snap.cadence_enabled},
	         {"gpu_submit_cadence_interval_us", snap.cadence_interval_us},
	         {"partial_buffer_refresh", snap.partial_refresh_enabled},
	         {"batch_hot_readbacks", snap.batch_hot_readbacks_enabled},
	         {"readback_neighbours", snap.readback_neighbours_enabled},
	         {"wide_readback_window", snap.wide_readback_window_enabled},
	         {"fault_ahead", snap.fault_ahead_enabled},
	         {"binding_reuse", snap.binding_reuse_enabled},
	         {"trim_scratch_memory", snap.trim_scratch_memory_enabled},
	         {"compact_image_readback", snap.compact_image_readback_enabled},
	         {"read_buffer_reuse", snap.read_buffer_reuse_enabled},
	         {"resource_prep_reuse", snap.resource_prep_reuse_enabled},
	         {"tile_layout_cache", snap.tile_layout_cache_enabled},
	         {"extended_stream_buffer", snap.extended_stream_buffer_enabled},
	         {"redundant_bind_elision", snap.redundant_bind_elision_enabled},
	         {"pipeline_cache_mru", snap.pipeline_cache_mru_enabled},
	         {"pipeline_key_cache", snap.pipeline_key_cache_enabled},
	         {"draw_state_dirty_check", snap.draw_state_dirty_check_enabled},
	         {"ensure_pipeline_cache", snap.ensure_pipeline_cache_enabled},
	         {"descriptor_cache", snap.descriptor_cache_enabled},
	         {"fast_pending_ops", snap.fast_pending_ops_enabled},
	         {"priority_spin_wait", snap.priority_spin_wait_enabled},
	         {"priority_spin_duration_us", snap.priority_spin_duration_us},
	     }},
	};

	const double duration_sec =
	    snap.capture_real_duration_ms > 0 ? (snap.capture_real_duration_ms / 1000.0) : 1.0;
	const double frame_count = snap.total_frames > 0 ? static_cast<double>(snap.total_frames) : 1.0;

	j["frametime_summary"] = {
	    {"total_frames", snap.total_frames},   {"effective_fps", snap.effective_fps},
	    {"frametime_fps", snap.frametime_fps}, {"avg_frame_ms", snap.avg_frame_ms},
	    {"min_frame_ms", snap.min_frame_ms},   {"max_frame_ms", snap.max_frame_ms},
	    {"p50_frame_ms", snap.p50_frame_ms},   {"p90_frame_ms", snap.p90_frame_ms},
	    {"p95_frame_ms", snap.p95_frame_ms},   {"p99_frame_ms", snap.p99_frame_ms},
	};

	nlohmann::json slow_frames_json = nlohmann::json::array();
	for (const auto& sf: snap.slow_frames) {
		slow_frames_json.push_back({
		    {"frame_index", sf.frame_index},
		    {"frame_ms", sf.frame_ms},
		    {"timestamp_ms", sf.timestamp_ms},
		});
	}

	nlohmann::json detailed_hitches_json = nlohmann::json::array();
	for (const auto& h: snap.detailed_hitches) {
		detailed_hitches_json.push_back({
		    {"frame_index", h.frame_index},
		    {"frame_ms", h.frame_ms},
		    {"timestamp_ms", h.timestamp_ms},
		    {"dominant_blocker", h.dominant_blocker},
		    {"blocker_duration_ms", h.blocker_duration_ms},
		    {"blocker_thread_id", h.blocker_thread_id},
		    {"blocker_tick", h.blocker_tick},
		    {"blocker_address", fmt::format("0x{:016x}", h.blocker_address)},
		    {"correlation_confidence", h.correlation_confidence},
		});
	}

	j["stuttering_analysis"] = {
	    {"hitches_ge_50ms", snap.hitches_ge_50ms},
	    {"hitches_ge_100ms", snap.hitches_ge_100ms},
	    {"hitches_ge_250ms", snap.hitches_ge_250ms},
	    {"hitches_ge_500ms", snap.hitches_ge_500ms},
	    {"hitches_ge_1000ms", snap.hitches_ge_1000ms},
	    {"hitch_time_total_ms", snap.hitch_time_total_ms},
	    {"hitch_time_pct_of_session",
	     duration_sec > 0 ? (snap.hitch_time_total_ms / (duration_sec * 1000.0) * 100.0) : 0.0},
	    {"hitch_frame_pct", snap.total_frames > 0 ? (static_cast<double>(snap.hitches_ge_50ms) /
	                                                 snap.total_frames * 100.0)
	                                              : 0.0},
	    {"slowest_frames", slow_frames_json},
	    {"detailed_hitches", detailed_hitches_json},
	};

	j["write_tracking"] = {
	    {"total_faults", snap.total_faults},
	    {"faults_per_frame", static_cast<double>(snap.total_faults) / frame_count},
	    {"faults_per_second", static_cast<double>(snap.total_faults) / duration_sec},
	    {"total_reprotects", snap.total_reprotects},
	    {"reprotects_per_frame", static_cast<double>(snap.total_reprotects) / frame_count},
	    {"thread_accumulated_handler_ms", static_cast<double>(snap.total_handler_ns) / 1000000.0},
	    {"thread_accumulated_handler_ms_per_frame",
	     (static_cast<double>(snap.total_handler_ns) / 1000000.0) / frame_count},
	    {"avg_handler_us", snap.avg_handler_us},
	    {"max_handler_us", static_cast<double>(snap.max_handler_ns) / 1000.0},
	};

	j["memory_transfers"] = {
	    {"neighbour_readback_count", snap.neighbour_readback_count},
	    {"neighbour_readback_bytes", snap.neighbour_readback_bytes},
	    {"cpu_to_gpu_upload_mb",
	     static_cast<double>(snap.cpu_to_gpu_upload_bytes) / (1024.0 * 1024.0)},
	    {"cpu_to_gpu_upload_mb_per_frame",
	     (static_cast<double>(snap.cpu_to_gpu_upload_bytes) / (1024.0 * 1024.0)) / frame_count},
	    {"cpu_to_gpu_upload_mb_per_second",
	     (static_cast<double>(snap.cpu_to_gpu_upload_bytes) / (1024.0 * 1024.0)) / duration_sec},
	    {"gpu_to_cpu_readback_mb",
	     static_cast<double>(snap.gpu_to_cpu_readback_bytes) / (1024.0 * 1024.0)},
	    {"gpu_to_cpu_readback_mb_per_frame",
	     (static_cast<double>(snap.gpu_to_cpu_readback_bytes) / (1024.0 * 1024.0)) / frame_count},
	    {"gpu_to_cpu_readback_mb_per_second",
	     (static_cast<double>(snap.gpu_to_cpu_readback_bytes) / (1024.0 * 1024.0)) / duration_sec},
	    {"internal_gpu_copy_mb",
	     static_cast<double>(snap.internal_gpu_transfer_bytes) / (1024.0 * 1024.0)},
	};

	j["optimizations"] = {
	    {"binding_range_reuses", snap.binding_range_reuses},
	    {"binding_lookup_reuses", snap.binding_lookup_reuses},
	    {"binding_sampler_reuses", snap.binding_sampler_reuses},
	    {"scratch_freed_bytes", snap.scratch_freed_bytes},
	    {"scratch_retirements", snap.scratch_retirements},
	    {"scratch_allocations", snap.scratch_allocations},
	    {"compact_readback_copies", snap.compact_readback_copies},
	    {"compact_readback_fallbacks", snap.compact_readback_fallbacks},
	    {"readback_prefill_avoided_bytes", snap.readback_prefill_avoided_bytes},
	    {"readback_staging_saved_bytes", snap.readback_staging_saved_bytes},
	    {"read_buffer_hits", snap.read_buffer_hits},
	    {"read_buffer_misses", snap.read_buffer_misses},
	    {"resource_prep_hits", snap.resource_prep_hits},
	    {"resource_prep_misses", snap.resource_prep_misses},
	    {"tile_layout_hits", snap.tile_layout_hits},
	    {"tile_layout_misses", snap.tile_layout_misses},
	    {"binding_upload_reuses", snap.binding_upload_reuses},
	    {"binding_upload_reuse_bytes", snap.binding_upload_reuse_bytes},
	    {"texture_partial_refreshes", snap.texture_partial_refreshes},
	    {"texture_partial_fallbacks", snap.texture_partial_fallbacks},
	    {"texture_partial_upload_mb",
	     static_cast<double>(snap.texture_partial_upload_bytes) / (1024.0 * 1024.0)},
	    {"cadence_submits", snap.cadence_submits},
	    {"cadence_held_back", snap.cadence_held_back},
	    {"priority_wait_spins", snap.priority_wait_spins},
	    {"priority_wait_spin_hits", snap.priority_wait_spin_hits},
	    {"pending_ops_fast_skips", snap.pending_ops_fast_skips},
	    {"cp_sequencer_enabled", snap.cp_sequencer_enabled},
	    {"cp_sequencer_ops_emitted", snap.cp_sequencer_ops_emitted},
	    {"cp_sequencer_lockstep_waits", snap.cp_sequencer_lockstep_waits},
	    {"cp_sequencer_snapshots_taken", snap.cp_sequencer_snapshots_taken},
	    {"cp_sequencer_waits_spin", snap.cp_sequencer_waits_spin},
	    {"draw_prep_enabled", snap.draw_prep_enabled},
	    {"draw_prep_published", snap.draw_prep_published},
	    {"draw_prep_prepared", snap.draw_prep_prepared},
	    {"draw_prep_committed", snap.draw_prep_committed},
	    {"draw_prep_fallback", snap.draw_prep_fallback},
	    {"draw_prep_avoided", snap.draw_prep_avoided},
	    {"draw_prep_avoided_time_ms", static_cast<double>(snap.draw_prep_avoided_time_ns) / 1e6},
	    {"draw_prep_duplication_pct", snap.draw_prep_duplication_pct},
	};

	nlohmann::json waits_json = nlohmann::json::object();
	for (size_t i = 0; i < static_cast<size_t>(GpuWaitReason::Count); ++i) {
		const auto   reason   = static_cast<GpuWaitReason>(i);
		const auto&  s        = snap.waits[i];
		const double total_ms = static_cast<double>(s.total_ns) / 1000000.0;
		const double avg_us =
		    s.count > 0 ? (static_cast<double>(s.total_ns) / (s.count * 1000.0)) : 0.0;
		waits_json[GetWaitReasonName(reason)] = {
		    {"count", s.count},
		    {"count_per_frame", static_cast<double>(s.count) / frame_count},
		    {"thread_accumulated_ms", total_ms},
		    {"thread_accumulated_ms_per_frame", total_ms / frame_count},
		    {"avg_latency_us", avg_us},
		    {"max_ms", static_cast<double>(s.max_ns) / 1000000.0},
		};
	}
	j["gpu_timeline_waits"] = waits_json;

	uint64_t       total_submits = 0;
	nlohmann::json submits_json  = nlohmann::json::object();
	for (size_t i = 0; i < static_cast<size_t>(SubmitReason::Count); ++i) {
		const auto reason = static_cast<SubmitReason>(i);
		const auto count  = snap.submits[i].count;
		total_submits += count;
		submits_json[GetSubmitReasonName(reason)] = {
		    {"count", count},
		    {"per_frame", static_cast<double>(count) / frame_count},
		    {"per_second", static_cast<double>(count) / duration_sec},
		};
	}
	j["vulkan_submits"] = {
	    {"total_submits", total_submits},
	    {"submits_per_frame", static_cast<double>(total_submits) / frame_count},
	    {"submits_per_second", static_cast<double>(total_submits) / duration_sec},
	    {"by_reason", submits_json},
	};

	nlohmann::json pages_json = nlohmann::json::array();
	for (const auto& p: snap.top_pages) {
		pages_json.push_back({
		    {"page_vaddr", fmt::format("0x{:016x}", p.page_vaddr)},
		    {"faults", p.fault_count},
		    {"reprotects", p.reprotect_count},
		});
	}
	j["top_hot_pages"] = pages_json;

	nlohmann::json rips_json = nlohmann::json::array();
	for (const auto& r: snap.top_rips) {
		rips_json.push_back({
		    {"rip", fmt::format("0x{:016x}", r.rip)},
		    {"faults", r.fault_count},
		});
	}
	j["top_rips"] = rips_json;

	nlohmann::json threads_json = nlohmann::json::array();
	for (const auto& t: snap.top_threads) {
		threads_json.push_back({
		    {"tid", t.tid},
		    {"name", t.name},
		    {"faults", t.fault_count},
		});
	}
	j["top_threads"] = threads_json;

	j["phase_segmentation"] = {
	    {"loading",
	     {
	         {"frame_count", snap.phase_loading.frame_count},
	         {"total_frame_ms", snap.phase_loading.total_frame_ms},
	         {"faults", snap.phase_loading.faults},
	         {"reprotects", snap.phase_loading.reprotects},
	         {"submits", snap.phase_loading.submits},
	         {"gpu_wait_ms", static_cast<double>(snap.phase_loading.gpu_wait_ns) / 1000000.0},
	     }},
	    {"gameplay",
	     {
	         {"frame_count", snap.phase_gameplay.frame_count},
	         {"total_frame_ms", snap.phase_gameplay.total_frame_ms},
	         {"faults", snap.phase_gameplay.faults},
	         {"reprotects", snap.phase_gameplay.reprotects},
	         {"submits", snap.phase_gameplay.submits},
	         {"gpu_wait_ms", static_cast<double>(snap.phase_gameplay.gpu_wait_ns) / 1000000.0},
	     }},
	};

	j["timing_and_simulation"] = {
	    {"total_vblanks", snap.total_vblanks},
	    {"vblank_rate_hz", snap.vblank_rate_hz},
	    {"total_flips_submitted", snap.total_flips_submitted},
	    {"total_flips_completed", snap.total_flips_completed},
	    {"flip_rate_hz", snap.flip_rate_hz},
	    {"total_flip_wait_ms", snap.total_flip_wait_ms},
	    {"avg_flip_wait_ms", snap.avg_flip_wait_ms},
	    {"audio_updates_count", snap.audio_updates_count},
	    {"audio_total_samples", snap.audio_total_samples},
	    {"audio_effective_hz", snap.audio_effective_hz},
	    {"estimated_simulation_speed_ratio_model_60hz", snap.simulation_speed_ratio},
	    {"estimated_simulation_speed_percent_model_60hz", snap.simulation_speed_ratio * 100.0},
	    {"simulation_speed_model_note",
	     "Theoretical model estimate assuming 1/60s fixed simulation timestep per completed flip. "
	     "Not a directly measured internal guest tick counter."},
	    {"simulation_speed_ratio", snap.simulation_speed_ratio},
	    {"simulation_speed_percent", snap.simulation_speed_ratio * 100.0},
	};

	j["graphics_workload"] = {
	    {"total_draws", snap.total_draws},
	    {"draws_per_frame", snap.draws_per_frame},
	    {"draws_per_second", snap.draws_per_second},
	    {"total_draw_instances", snap.total_draw_instances},
	    {"instances_per_frame", snap.instances_per_frame},
	    {"total_indirect_draws", snap.total_indirect_draws},
	    {"total_dispatches", snap.total_dispatches},
	    {"dispatches_per_frame", snap.dispatches_per_frame},
	    {"dispatches_per_second", snap.dispatches_per_second},
	    {"pipelines_created", snap.pipelines_created},
	    {"shaders_compiled", snap.shaders_compiled},
	    {"gpu_thread_busy_ms", snap.gpu_thread_busy_ms},
	    {"gpu_thread_busy_ms_per_frame", snap.gpu_thread_busy_ms_per_frame},
	    {"pipeline_cache_lookups", snap.pipeline_cache_lookups},
	    {"pipeline_cache_hits", snap.pipeline_cache_hits},
	    {"pipeline_cache_hit_pct", snap.pipeline_cache_hit_pct},
	    {"redundant_pipeline_binds", snap.redundant_pipeline_binds},
	    {"redundant_vertex_binds", snap.redundant_vertex_binds},
	    {"redundant_index_binds", snap.redundant_index_binds},
	};

	nlohmann::json stages_json = nlohmann::json::object();
	for (size_t i = 0; i < static_cast<size_t>(PipelineStage::Count); ++i) {
		const auto  stage                        = static_cast<PipelineStage>(i);
		const auto& s                            = snap.pipeline_stages[i];
		stages_json[GetPipelineStageName(stage)] = {
		    {"count", s.count},
		    {"total_ms", s.total_ms},
		    {"avg_us", s.avg_us},
		    {"max_ms", s.max_ms},
		    {"ms_per_frame", s.ms_per_frame},
		    {"pct_thread_time", s.pct_thread_time},
		};
	}
	j["pipeline_stage_timings"] = stages_json;

	j["cpu_gpu_separation"] = {
	    {"gpu_thread_wall_clock_ms", snap.gpu_thread_wall_clock_ms},
	    {"gpu_thread_cpu_time_ms", snap.gpu_thread_cpu_time_ms},
	    {"gpu_thread_cpu_utilization_pct", snap.gpu_thread_cpu_utilization_pct},
	    {"gpu_thread_wait_time_ms", snap.gpu_thread_wait_time_ms},
	    {"gpu_timestamps_supported", snap.gpu_timestamps_supported},
	    {"gpu_execution_time_ms", snap.gpu_execution_time_ms},
	    {"gpu_execution_avg_per_frame_ms", snap.gpu_execution_avg_per_frame_ms},
	    {"gpu_queries_recorded", snap.gpu_queries_recorded},
	    {"pm4_dispatch_inclusive_ms", snap.pm4_dispatch_inclusive_ms},
	    {"pm4_dispatch_exclusive_ms", snap.pm4_dispatch_exclusive_ms},
	    {"pm4_dispatch_exclusive_ms_per_frame", snap.pm4_dispatch_exclusive_ms_per_frame},
	};

	j["stream_buffer_audit"] = {
	    {"total_allocations", snap.stream_buffer_stats.total_allocs},
	    {"total_wraparounds", snap.stream_buffer_stats.total_wraps},
	    {"total_allocated_mb", snap.stream_buffer_stats.total_mb},
	    {"mb_per_frame", snap.stream_buffer_stats.mb_per_frame},
	    {"high_water_mark_mb", snap.stream_buffer_stats.high_water_mb},
	    {"wait_events", snap.stream_buffer_stats.wait_events},
	    {"wait_total_ms", snap.stream_buffer_stats.wait_total_ms},
	    {"wait_avg_ms", snap.stream_buffer_stats.wait_avg_ms},
	    {"wait_max_ms", snap.stream_buffer_stats.wait_max_ms},
	    {"last_wait_tick", snap.stream_buffer_stats.last_wait_tick},
	};

	const auto& td         = snap.time_distribution;
	j["time_distribution"] = {
	    {"total_workload_ms", td.total_workload_ms},
	    {"bottleneck", td.BottleneckString()},
	    {"queue_ms", td.total_queue_ms},
	    {"queue_pct", td.queue_pct},
	    {"queue_details",
	     {
	         {"submission_queue_count", td.queue_submission_count},
	         {"submission_queue_ms", td.queue_submission_ms},
	         {"intake_queue_count", td.queue_intake_count},
	         {"intake_queue_ms", td.queue_intake_ms},
	         {"opstream_queue_count", td.queue_opstream_count},
	         {"opstream_queue_ms", td.queue_opstream_ms},
	         {"backpressure_count", td.queue_backpressure_count},
	         {"backpressure_ms", td.queue_backpressure_ms},
	     }},
	    {"cpu_active_ms", td.total_cpu_active_ms},
	    {"cpu_active_pct", td.cpu_active_pct},
	    {"cpu_details",
	     {
	         {"pm4_decode_ms", td.cpu_pm4_decode_ms},
	         {"pm4_dispatch_ms", td.cpu_pm4_dispatch_ms},
	         {"render_state_ms", td.cpu_render_state_ms},
	         {"pipeline_keys_ms", td.cpu_pipeline_keys_ms},
	         {"descriptors_ms", td.cpu_descriptors_ms},
	         {"buffers_ms", td.cpu_buffers_ms},
	         {"command_recording_ms", td.cpu_command_record_ms},
	         {"submit_overhead_ms", td.cpu_submit_overhead_ms},
	         {"draw_prep_workers_ms", td.cpu_draw_prep_worker_ms},
	     }},
	    {"gpu_wait_ms", td.total_gpu_wait_ms},
	    {"gpu_wait_pct", td.gpu_wait_pct},
	    {"gpu_wait_details",
	     {
	         {"timeline_wait_ms", td.gpu_timeline_wait_ms},
	         {"streambuffer_wait_ms", td.gpu_streambuffer_wait_ms},
	         {"swapchain_wait_ms", td.gpu_swapchain_wait_ms},
	         {"readback_wait_ms", td.gpu_readback_wait_ms},
	         {"other_wait_ms", td.gpu_other_wait_ms},
	     }},
	};

	// Legacy backwards-compatible aliases for existing tests and scripts
	j["configuration"]              = j["metadata"]["config"];
	j["frametime_metrics_ms"]       = j["frametime_summary"];
	j["write_tracking_metrics"]     = j["write_tracking"];
	j["gpu_waits_breakdown"]        = j["gpu_timeline_waits"];
	j["vk_queue_submits_breakdown"] = j["vulkan_submits"]["by_reason"];
	j["top_faulting_rips"]          = j["top_rips"];
	j["optimization_metrics"]       = j["optimizations"];

	std::ofstream file(filepath);
	if (!file.is_open()) {
		return false;
	}
	file << j.dump(2);
	return true;
}

bool ExportCsv(const std::string& filepath, const ProfileSnapshot* existing_snap) {
	const auto    snap = existing_snap != nullptr ? *existing_snap : TakeSnapshot();
	std::ofstream out(filepath);
	if (!out.is_open()) {
		return false;
	}

	const double duration_sec =
	    snap.capture_real_duration_ms > 0 ? (snap.capture_real_duration_ms / 1000.0) : 1.0;
	const double frame_count = snap.total_frames > 0 ? static_cast<double>(snap.total_frames) : 1.0;

	out << "# KytyPS5 Audit Profile Export (Interval Consistent Snapshot)\n";
	out << "# Note: ThreadAccumulated times sum concurrent thread execution and cannot be added "
	       "directly to wall-clock frametime.\n";
	out << "Category,Metric,Value\n";
	out << fmt::format("Metadata,CaptureId,{}\n", snap.capture_id);
	out << fmt::format("Metadata,CaptureState,{}\n", snap.capture_state_str);
	out << fmt::format("Metadata,CapturePhase,{}\n", snap.capture_phase_str);
	out << fmt::format("Metadata,TriggerReason,{}\n", snap.trigger_reason);
	out << fmt::format("Metadata,RealDurationSeconds,{:.3f}\n", duration_sec);
	out << fmt::format("Metadata,TargetDurationSeconds,{}\n", snap.capture_target_duration_sec);
	out << fmt::format("Metadata,DelaySeconds,{}\n", snap.capture_delay_sec);
	out << fmt::format("Metadata,GitCommit,{}\n", snap.git_commit);
	out << fmt::format("Metadata,GitRevision,{}\n", snap.git_revision);
	out << fmt::format("Metadata,GitHash,{}\n", snap.git_hash);
	out << fmt::format("Metadata,BuildTimestamp,{}\n", snap.build_timestamp);
	out << fmt::format("Metadata,Compiler,{}\n", snap.compiler);
	out << fmt::format("Metadata,BinaryPath,{}\n", snap.binary_path);
	out << fmt::format("Metadata,CadenceEnabled,{}\n", snap.cadence_enabled ? 1 : 0);
	out << fmt::format("Metadata,CadenceIntervalUs,{}\n", snap.cadence_interval_us);
	out << fmt::format("Metadata,PartialRefreshEnabled,{}\n", snap.partial_refresh_enabled ? 1 : 0);
	out << fmt::format("Metadata,BatchHotReadbacksEnabled,{}\n",
	                   snap.batch_hot_readbacks_enabled ? 1 : 0);
	out << fmt::format("Metadata,ReadbackNeighboursEnabled,{}\n",
	                   snap.readback_neighbours_enabled ? 1 : 0);
	out << fmt::format("Metadata,WideReadbackWindowEnabled,{}\n",
	                   snap.wide_readback_window_enabled ? 1 : 0);
	out << fmt::format("Metadata,FaultAheadEnabled,{}\n", snap.fault_ahead_enabled ? 1 : 0);
	out << fmt::format("Metadata,BindingReuseEnabled,{}\n", snap.binding_reuse_enabled ? 1 : 0);
	out << fmt::format("Metadata,TrimScratchMemoryEnabled,{}\n",
	                   snap.trim_scratch_memory_enabled ? 1 : 0);
	out << fmt::format("Metadata,CompactImageReadbackEnabled,{}\n",
	                   snap.compact_image_readback_enabled ? 1 : 0);
	out << fmt::format("Metadata,ReadBufferReuseEnabled,{}\n",
	                   snap.read_buffer_reuse_enabled ? 1 : 0);
	out << fmt::format("Metadata,ResourcePrepReuseEnabled,{}\n",
	                   snap.resource_prep_reuse_enabled ? 1 : 0);
	out << fmt::format("Optimization,ScratchFreedBytes,{}\n", snap.scratch_freed_bytes);
	out << fmt::format("Optimization,ScratchRetirements,{}\n", snap.scratch_retirements);
	out << fmt::format("Optimization,ScratchAllocations,{}\n", snap.scratch_allocations);
	out << fmt::format("Optimization,CompactReadbackCopies,{}\n", snap.compact_readback_copies);
	out << fmt::format("Optimization,CompactReadbackFallbacks,{}\n",
	                   snap.compact_readback_fallbacks);
	out << fmt::format("Optimization,ReadbackPrefillAvoidedBytes,{}\n",
	                   snap.readback_prefill_avoided_bytes);
	out << fmt::format("Optimization,ReadbackStagingSavedBytes,{}\n",
	                   snap.readback_staging_saved_bytes);
	out << fmt::format("Optimization,ReadBufferHits,{}\nOptimization,ReadBufferMisses,{}\n",
	                   snap.read_buffer_hits, snap.read_buffer_misses);
	out << fmt::format("Optimization,ResourcePrepHits,{}\nOptimization,ResourcePrepMisses,{}\n",
	                   snap.resource_prep_hits, snap.resource_prep_misses);
	out << fmt::format("Metadata,TileLayoutCacheEnabled,{}\n",
	                   snap.tile_layout_cache_enabled ? 1 : 0);
	out << fmt::format("Metadata,ExtendedStreamBufferEnabled,{}\n",
	                   snap.extended_stream_buffer_enabled ? 1 : 0);
	out << fmt::format("Metadata,PipelineKeyCacheEnabled,{}\n",
	                   snap.pipeline_key_cache_enabled ? 1 : 0);
	out << fmt::format("Metadata,DrawStateDirtyCheckEnabled,{}\n",
	                   snap.draw_state_dirty_check_enabled ? 1 : 0);
	out << fmt::format("Metadata,EnsurePipelineCacheEnabled,{}\n",
	                   snap.ensure_pipeline_cache_enabled ? 1 : 0);
	out << fmt::format("Metadata,DescriptorCacheEnabled,{}\n",
	                   snap.descriptor_cache_enabled ? 1 : 0);
	out << fmt::format("Metadata,FastPendingOpsEnabled,{}\n",
	                   snap.fast_pending_ops_enabled ? 1 : 0);
	out << fmt::format("Metadata,PrioritySpinWaitEnabled,{}\n",
	                   snap.priority_spin_wait_enabled ? 1 : 0);
	out << fmt::format("Metadata,PrioritySpinDurationUs,{}\n", snap.priority_spin_duration_us);

	out << fmt::format("Frametime,TotalFrames,{}\n", snap.total_frames);
	out << fmt::format("Frametime,EffectiveFps,{:.2f}\n", snap.effective_fps);
	out << fmt::format("Frametime,FrametimeFps,{:.2f}\n", snap.frametime_fps);
	out << fmt::format("Frametime,AvgFrameMs,{:.2f}\n", snap.avg_frame_ms);
	out << fmt::format("Frametime,MinFrameMs,{:.2f}\n", snap.min_frame_ms);
	out << fmt::format("Frametime,MaxFrameMs,{:.2f}\n", snap.max_frame_ms);
	out << fmt::format("Frametime,P50FrameMs,{:.2f}\n", snap.p50_frame_ms);
	out << fmt::format("Frametime,P90FrameMs,{:.2f}\n", snap.p90_frame_ms);
	out << fmt::format("Frametime,P95FrameMs,{:.2f}\n", snap.p95_frame_ms);
	out << fmt::format("Frametime,P99FrameMs,{:.2f}\n", snap.p99_frame_ms);

	out << fmt::format("Stuttering,HitchesGe50ms,{}\n", snap.hitches_ge_50ms);
	out << fmt::format("Stuttering,HitchesGe100ms,{}\n", snap.hitches_ge_100ms);
	out << fmt::format("Stuttering,HitchesGe250ms,{}\n", snap.hitches_ge_250ms);
	out << fmt::format("Stuttering,HitchesGe500ms,{}\n", snap.hitches_ge_500ms);
	out << fmt::format("Stuttering,HitchesGe1000ms,{}\n", snap.hitches_ge_1000ms);
	out << fmt::format("Stuttering,HitchTimeTotalMs,{:.2f}\n", snap.hitch_time_total_ms);

	out << fmt::format("Timing,TotalVblanks,{}\n", snap.total_vblanks);
	out << fmt::format("Timing,VblankRateHz,{:.2f}\n", snap.vblank_rate_hz);
	out << fmt::format("Timing,TotalFlipsSubmitted,{}\n", snap.total_flips_submitted);
	out << fmt::format("Timing,TotalFlipsCompleted,{}\n", snap.total_flips_completed);
	out << fmt::format("Timing,FlipRateHz,{:.2f}\n", snap.flip_rate_hz);
	out << fmt::format("Timing,TotalFlipWaitMs,{:.2f}\n", snap.total_flip_wait_ms);
	out << fmt::format("Timing,AvgFlipWaitMs,{:.2f}\n", snap.avg_flip_wait_ms);
	out << fmt::format("Timing,AudioUpdatesCount,{}\n", snap.audio_updates_count);
	out << fmt::format("Timing,AudioTotalSamples,{}\n", snap.audio_total_samples);
	out << fmt::format("Timing,AudioEffectiveHz,{:.2f}\n", snap.audio_effective_hz);
	out << fmt::format("Timing,SimulationSpeedRatio,{:.4f}\n", snap.simulation_speed_ratio);
	out << fmt::format("Timing,SimulationSpeedPercent,{:.2f}%\n",
	                   snap.simulation_speed_ratio * 100.0);
	out << fmt::format("Timing,EstimatedSimulationSpeedRatioModel60Hz,{:.4f}\n",
	                   snap.simulation_speed_ratio);
	out << fmt::format("Timing,EstimatedSimulationSpeedPercentModel60Hz,{:.2f}%\n",
	                   snap.simulation_speed_ratio * 100.0);
	out << "Timing,SimulationSpeedModelType,TheoreticalModelAssuming60HzFixedTimestep\n";

	out << fmt::format("GraphicsWorkload,TotalDraws,{}\n", snap.total_draws);
	out << fmt::format("GraphicsWorkload,DrawsPerFrame,{:.2f}\n", snap.draws_per_frame);
	out << fmt::format("GraphicsWorkload,DrawsPerSecond,{:.2f}\n", snap.draws_per_second);
	out << fmt::format("GraphicsWorkload,TotalDrawInstances,{}\n", snap.total_draw_instances);
	out << fmt::format("GraphicsWorkload,InstancesPerFrame,{:.2f}\n", snap.instances_per_frame);
	out << fmt::format("GraphicsWorkload,TotalIndirectDraws,{}\n", snap.total_indirect_draws);
	out << fmt::format("GraphicsWorkload,TotalDispatches,{}\n", snap.total_dispatches);
	out << fmt::format("GraphicsWorkload,DispatchesPerFrame,{:.2f}\n", snap.dispatches_per_frame);
	out << fmt::format("GraphicsWorkload,DispatchesPerSecond,{:.2f}\n", snap.dispatches_per_second);
	out << fmt::format("GraphicsWorkload,PipelinesCreated,{}\n", snap.pipelines_created);
	out << fmt::format("GraphicsWorkload,ShadersCompiled,{}\n", snap.shaders_compiled);
	out << fmt::format("GraphicsWorkload,GpuThreadBusyMs,{:.2f}\n", snap.gpu_thread_busy_ms);
	out << fmt::format("GraphicsWorkload,GpuThreadBusyMsPerFrame,{:.2f}\n",
	                   snap.gpu_thread_busy_ms_per_frame);
	out << fmt::format("GraphicsWorkload,PipelineCacheLookups,{}\n", snap.pipeline_cache_lookups);
	out << fmt::format("GraphicsWorkload,PipelineCacheHits,{}\n", snap.pipeline_cache_hits);
	out << fmt::format("GraphicsWorkload,PipelineCacheHitPct,{:.1f}\n",
	                   snap.pipeline_cache_hit_pct);
	out << fmt::format("GraphicsWorkload,RedundantPipelineBinds,{}\n",
	                   snap.redundant_pipeline_binds);
	out << fmt::format("GraphicsWorkload,RedundantVertexBinds,{}\n", snap.redundant_vertex_binds);
	out << fmt::format("GraphicsWorkload,RedundantIndexBinds,{}\n", snap.redundant_index_binds);

	out << fmt::format("WriteTracking,TotalFaults,{}\n", snap.total_faults);
	out << fmt::format("WriteTracking,FaultsPerFrame,{:.1f}\n",
	                   static_cast<double>(snap.total_faults) / frame_count);
	out << fmt::format("WriteTracking,FaultsPerSecond,{:.0f}\n",
	                   static_cast<double>(snap.total_faults) / duration_sec);
	out << fmt::format("WriteTracking,TotalReprotects,{}\n", snap.total_reprotects);
	out << fmt::format("WriteTracking,ReprotectsPerFrame,{:.1f}\n",
	                   static_cast<double>(snap.total_reprotects) / frame_count);
	out << fmt::format("WriteTracking,ThreadAccumulatedHandlerMs,{:.2f}\n",
	                   static_cast<double>(snap.total_handler_ns) / 1000000.0);
	out << fmt::format("WriteTracking,ThreadAccumulatedHandlerMsPerFrame,{:.2f}\n",
	                   (static_cast<double>(snap.total_handler_ns) / 1000000.0) / frame_count);
	out << fmt::format("WriteTracking,AvgHandlerUs,{:.2f}\n", snap.avg_handler_us);
	out << fmt::format("WriteTracking,MaxHandlerUs,{:.2f}\n",
	                   static_cast<double>(snap.max_handler_ns) / 1000.0);

	for (size_t i = 0; i < static_cast<size_t>(GpuWaitReason::Count); ++i) {
		const auto   reason = static_cast<GpuWaitReason>(i);
		const auto&  s      = snap.waits[i];
		const double ms     = static_cast<double>(s.total_ns) / 1000000.0;
		const double avg_us =
		    s.count > 0 ? (static_cast<double>(s.total_ns) / (s.count * 1000.0)) : 0.0;
		out << fmt::format("GpuWaitCount,{},{}\n", GetWaitReasonName(reason), s.count);
		out << fmt::format("GpuWaitPerFrame,{},{:.2f}\n", GetWaitReasonName(reason),
		                   static_cast<double>(s.count) / frame_count);
		out << fmt::format("GpuWaitThreadMs,{},{:.3f}\n", GetWaitReasonName(reason), ms);
		out << fmt::format("GpuWaitThreadMsPerFrame,{},{:.3f}\n", GetWaitReasonName(reason),
		                   ms / frame_count);
		out << fmt::format("GpuWaitAvgLatencyUs,{},{:.1f}\n", GetWaitReasonName(reason), avg_us);
	}

	uint64_t total_submits = 0;
	for (size_t i = 0; i < static_cast<size_t>(SubmitReason::Count); ++i) {
		const auto reason = static_cast<SubmitReason>(i);
		const auto count  = snap.submits[i].count;
		total_submits += count;
		out << fmt::format("SubmitCount,{},{}\n", GetSubmitReasonName(reason), count);
		out << fmt::format("SubmitPerFrame,{},{:.2f}\n", GetSubmitReasonName(reason),
		                   static_cast<double>(count) / frame_count);
	}
	out << fmt::format("SubmitCount,Total,{}\n", total_submits);
	out << fmt::format("SubmitPerFrame,Total,{:.2f}\n",
	                   static_cast<double>(total_submits) / frame_count);

	out << fmt::format("Transfers,CpuToGpuUploadMB,{:.2f}\n",
	                   static_cast<double>(snap.cpu_to_gpu_upload_bytes) / (1024.0 * 1024.0));
	out << fmt::format("Transfers,CpuToGpuUploadMBPerFrame,{:.2f}\n",
	                   (static_cast<double>(snap.cpu_to_gpu_upload_bytes) / (1024.0 * 1024.0)) /
	                       frame_count);
	out << fmt::format("Transfers,GpuToCpuReadbackMB,{:.2f}\n",
	                   static_cast<double>(snap.gpu_to_cpu_readback_bytes) / (1024.0 * 1024.0));
	out << fmt::format("Transfers,GpuToCpuReadbackMBPerFrame,{:.2f}\n",
	                   (static_cast<double>(snap.gpu_to_cpu_readback_bytes) / (1024.0 * 1024.0)) /
	                       frame_count);
	out << fmt::format("Transfers,InternalGpuCopyMB,{:.2f}\n",
	                   static_cast<double>(snap.internal_gpu_transfer_bytes) / (1024.0 * 1024.0));
	out << fmt::format("Transfers,NeighbourReadbackCount,{}\n", snap.neighbour_readback_count);
	out << fmt::format("Transfers,NeighbourReadbackBytes,{}\n", snap.neighbour_readback_bytes);
	out << fmt::format("Optimization,BindingRangeReuses,{}\n", snap.binding_range_reuses);
	out << fmt::format("Optimization,BindingLookupReuses,{}\n", snap.binding_lookup_reuses);
	out << fmt::format("Optimization,BindingSamplerReuses,{}\n", snap.binding_sampler_reuses);
	out << fmt::format("Optimization,TileLayoutHits,{}\n", snap.tile_layout_hits);
	out << fmt::format("Optimization,TileLayoutMisses,{}\n", snap.tile_layout_misses);
	out << fmt::format("Optimization,BindingUploadReuses,{}\n", snap.binding_upload_reuses);
	out << fmt::format("Optimization,BindingUploadReuseBytes,{}\n",
	                   snap.binding_upload_reuse_bytes);

	out << fmt::format("Optimization,TexturePartialRefreshes,{}\n", snap.texture_partial_refreshes);
	out << fmt::format("Optimization,TexturePartialFallbacks,{}\n", snap.texture_partial_fallbacks);
	out << fmt::format("Optimization,TexturePartialUploadMB,{:.2f}\n",
	                   static_cast<double>(snap.texture_partial_upload_bytes) / (1024.0 * 1024.0));
	out << fmt::format("Optimization,CadenceSubmits,{}\n", snap.cadence_submits);
	out << fmt::format("Optimization,CadenceHeldBack,{}\n", snap.cadence_held_back);
	out << fmt::format("Optimization,PriorityWaitSpins,{}\n", snap.priority_wait_spins);
	out << fmt::format("Optimization,PriorityWaitSpinHits,{}\n", snap.priority_wait_spin_hits);
	out << fmt::format("Optimization,PendingOpsFastSkips,{}\n", snap.pending_ops_fast_skips);
	out << fmt::format("Optimization,CpSequencerEnabled,{}\n", snap.cp_sequencer_enabled ? 1 : 0);
	out << fmt::format("Optimization,CpSequencerOpsEmitted,{}\n", snap.cp_sequencer_ops_emitted);
	out << fmt::format("Optimization,CpSequencerLockstepWaits,{}\n",
	                   snap.cp_sequencer_lockstep_waits);
	out << fmt::format("Optimization,CpSequencerSnapshotsTaken,{}\n",
	                   snap.cp_sequencer_snapshots_taken);
	out << fmt::format("Optimization,CpSequencerWaitsSpin,{}\n", snap.cp_sequencer_waits_spin);
	out << fmt::format("Optimization,DrawPrepEnabled,{}\n", snap.draw_prep_enabled ? 1 : 0);
	out << fmt::format("Optimization,DrawPrepPublished,{}\n", snap.draw_prep_published);
	out << fmt::format("Optimization,DrawPrepPrepared,{}\n", snap.draw_prep_prepared);
	out << fmt::format("Optimization,DrawPrepCommitted,{}\n", snap.draw_prep_committed);
	out << fmt::format("Optimization,DrawPrepFallback,{}\n", snap.draw_prep_fallback);
	out << fmt::format("Optimization,DrawPrepAvoided,{}\n", snap.draw_prep_avoided);
	out << fmt::format("Optimization,DrawPrepAvoidedTimeMs,{:.3f}\n",
	                   static_cast<double>(snap.draw_prep_avoided_time_ns) / 1e6);
	out << fmt::format("Optimization,DrawPrepDuplicationPct,{:.1f}%\n",
	                   snap.draw_prep_duplication_pct);

	out << fmt::format("PhaseLoading,FrameCount,{}\n", snap.phase_loading.frame_count);
	out << fmt::format("PhaseLoading,TotalFrameMs,{:.2f}\n", snap.phase_loading.total_frame_ms);
	out << fmt::format("PhaseLoading,Faults,{}\n", snap.phase_loading.faults);
	out << fmt::format("PhaseLoading,Reprotects,{}\n", snap.phase_loading.reprotects);
	out << fmt::format("PhaseLoading,Submits,{}\n", snap.phase_loading.submits);
	out << fmt::format("PhaseLoading,GpuWaitMs,{:.2f}\n",
	                   static_cast<double>(snap.phase_loading.gpu_wait_ns) / 1000000.0);

	out << fmt::format("PhaseGameplay,FrameCount,{}\n", snap.phase_gameplay.frame_count);
	out << fmt::format("PhaseGameplay,TotalFrameMs,{:.2f}\n", snap.phase_gameplay.total_frame_ms);
	out << fmt::format("PhaseGameplay,Faults,{}\n", snap.phase_gameplay.faults);
	out << fmt::format("PhaseGameplay,Reprotects,{}\n", snap.phase_gameplay.reprotects);
	out << fmt::format("PhaseGameplay,Submits,{}\n", snap.phase_gameplay.submits);
	out << fmt::format("PhaseGameplay,GpuWaitMs,{:.2f}\n",
	                   static_cast<double>(snap.phase_gameplay.gpu_wait_ns) / 1000000.0);

	for (size_t i = 0; i < static_cast<size_t>(PipelineStage::Count); ++i) {
		const auto  stage = static_cast<PipelineStage>(i);
		const auto& s     = snap.pipeline_stages[i];
		out << fmt::format("PipelineStageCount,{},{}\n", GetPipelineStageName(stage), s.count);
		out << fmt::format("PipelineStageTotalMs,{},{:.3f}\n", GetPipelineStageName(stage),
		                   s.total_ms);
		out << fmt::format("PipelineStageAvgUs,{},{:.2f}\n", GetPipelineStageName(stage), s.avg_us);
		out << fmt::format("PipelineStageMaxMs,{},{:.3f}\n", GetPipelineStageName(stage), s.max_ms);
		out << fmt::format("PipelineStageMsPerFrame,{},{:.3f}\n", GetPipelineStageName(stage),
		                   s.ms_per_frame);
		out << fmt::format("PipelineStagePctThreadTime,{},{:.2f}%\n", GetPipelineStageName(stage),
		                   s.pct_thread_time);
	}

	out << fmt::format("CpuGpuSeparation,GpuThreadWallClockMs,{:.2f}\n",
	                   snap.gpu_thread_wall_clock_ms);
	out << fmt::format("CpuGpuSeparation,GpuThreadCpuTimeMs,{:.2f}\n", snap.gpu_thread_cpu_time_ms);

	const auto& td = snap.time_distribution;
	out << fmt::format("TimeDistribution,TotalWorkloadMs,{:.3f}\n", td.total_workload_ms);
	out << fmt::format("TimeDistribution,Bottleneck,\"{}\"\n", td.BottleneckString());
	out << fmt::format("TimeDistribution,TotalQueueMs,{:.3f}\n", td.total_queue_ms);
	out << fmt::format("TimeDistribution,QueuePct,{:.2f}%\n", td.queue_pct);
	out << fmt::format("TimeDistribution,SubmissionQueueMs,{:.3f}\n", td.queue_submission_ms);
	out << fmt::format("TimeDistribution,IntakeQueueMs,{:.3f}\n", td.queue_intake_ms);
	out << fmt::format("TimeDistribution,OpStreamQueueMs,{:.3f}\n", td.queue_opstream_ms);
	out << fmt::format("TimeDistribution,BackpressureMs,{:.3f}\n", td.queue_backpressure_ms);
	out << fmt::format("TimeDistribution,TotalCpuActiveMs,{:.3f}\n", td.total_cpu_active_ms);
	out << fmt::format("TimeDistribution,CpuActivePct,{:.2f}%\n", td.cpu_active_pct);
	out << fmt::format("TimeDistribution,TotalGpuWaitMs,{:.3f}\n", td.total_gpu_wait_ms);
	out << fmt::format("TimeDistribution,GpuWaitPct,{:.2f}%\n", td.gpu_wait_pct);
	out << fmt::format("CpuGpuSeparation,GpuThreadCpuUtilizationPct,{:.2f}%\n",
	                   snap.gpu_thread_cpu_utilization_pct);
	out << fmt::format("CpuGpuSeparation,GpuThreadWaitTimeMs,{:.2f}\n",
	                   snap.gpu_thread_wait_time_ms);
	out << fmt::format("CpuGpuSeparation,GpuExecutionTimeMs,{:.2f}\n", snap.gpu_execution_time_ms);
	out << fmt::format("CpuGpuSeparation,GpuQueriesRecorded,{}\n", snap.gpu_queries_recorded);
	out << fmt::format("CpuGpuSeparation,Pm4DispatchInclusiveMs,{:.2f}\n",
	                   snap.pm4_dispatch_inclusive_ms);
	out << fmt::format("CpuGpuSeparation,Pm4DispatchExclusiveMs,{:.2f}\n",
	                   snap.pm4_dispatch_exclusive_ms);
	out << fmt::format("CpuGpuSeparation,Pm4DispatchExclusiveMsPerFrame,{:.2f}\n",
	                   snap.pm4_dispatch_exclusive_ms_per_frame);

	out << fmt::format("StreamBuffer,TotalAllocations,{}\n", snap.stream_buffer_stats.total_allocs);
	out << fmt::format("StreamBuffer,TotalWraparounds,{}\n", snap.stream_buffer_stats.total_wraps);
	out << fmt::format("StreamBuffer,TotalAllocatedMB,{:.2f}\n", snap.stream_buffer_stats.total_mb);
	out << fmt::format("StreamBuffer,MBPerFrame,{:.2f}\n", snap.stream_buffer_stats.mb_per_frame);
	out << fmt::format("StreamBuffer,HighWaterMarkMB,{:.2f}\n",
	                   snap.stream_buffer_stats.high_water_mb);
	out << fmt::format("StreamBuffer,WaitEvents,{}\n", snap.stream_buffer_stats.wait_events);
	out << fmt::format("StreamBuffer,WaitTotalMs,{:.3f}\n", snap.stream_buffer_stats.wait_total_ms);
	out << fmt::format("StreamBuffer,WaitAvgMs,{:.3f}\n", snap.stream_buffer_stats.wait_avg_ms);
	out << fmt::format("StreamBuffer,WaitMaxMs,{:.3f}\n", snap.stream_buffer_stats.wait_max_ms);
	out << fmt::format("StreamBuffer,LastWaitTick,{}\n", snap.stream_buffer_stats.last_wait_tick);

	out << "\nDetailedHitches,FrameIndex,FrameMs,TimestampMs,DominantBlocker,BlockerDurationMs,"
	       "BlockerThreadId,BlockerTick,BlockerAddress,CorrelationConfidence\n";
	for (const auto& h: snap.detailed_hitches) {
		out << fmt::format("Hitch,{},{:.2f},{},{},{:.2f},{},{},0x{:016x},{}\n", h.frame_index,
		                   h.frame_ms, h.timestamp_ms, h.dominant_blocker, h.blocker_duration_ms,
		                   h.blocker_thread_id, h.blocker_tick, h.blocker_address,
		                   h.correlation_confidence);
	}

	out << "\nTopPages,PageVaddr,FaultCount,ReprotectCount\n";
	for (const auto& p: snap.top_pages) {
		out << fmt::format("Page,0x{:016x},{},{}\n", p.page_vaddr, p.fault_count,
		                   p.reprotect_count);
	}

	out << "\nTopRips,InstructionPointer,FaultCount\n";
	for (const auto& r: snap.top_rips) {
		out << fmt::format("RIP,0x{:016x},{}\n", r.rip, r.fault_count);
	}

	out << "\nTopThreads,ThreadId,ThreadName,FaultCount\n";
	for (const auto& t: snap.top_threads) {
		out << fmt::format("Thread,{},\"{}\",{}\n", t.tid, t.name, t.fault_count);
	}

	out << "\nSlowFrames,FrameIndex,FrameMs,TimestampMs\n";
	for (const auto& sf: snap.slow_frames) {
		out << fmt::format("SlowFrame,{},{:.2f},{}\n", sf.frame_index, sf.frame_ms,
		                   sf.timestamp_ms);
	}

	return true;
}

static bool ExportFramesCsv(const std::string& filepath) {
	std::lock_guard<std::mutex> lock(g_frame_timeseries_mutex);
	if (g_frame_timeseries.empty()) {
		return false;
	}
	std::ofstream out(filepath, std::ios::out | std::ios::trunc);
	if (!out.is_open()) {
		Log::WriteToConsoleAndLog(
		    fmt::format("[AuditProfiler] Error: Could not open frames CSV file: {}\n", filepath));
		return false;
	}
	out << "frame_index,frame_ms,fps,phase,draws,dispatches,submits,pm4_decode_us,pm4_dispatch_us,"
	       "render_state_prep_us,descriptor_prep_us,pipeline_lookup_us,pipeline_key_build_us,"
	       "buffer_prep_us,command_recording_us,barriers_us,sync_waits_us,write_faults,stream_"
	       "buffer_alloc_kb,stream_buffer_wait_us\n";
	for (const auto& r: g_frame_timeseries) {
		const double fps = r.frame_ms > 0.0f ? (1000.0 / r.frame_ms) : 0.0;
		out << fmt::format(
		    "{},{:.3f},{:.2f},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", r.frame_index,
		    r.frame_ms, fps, GetExecutionPhaseName(static_cast<ExecutionPhase>(r.phase)), r.draws,
		    r.dispatches, r.submits, r.pm4_decode_us, r.pm4_dispatch_us, r.render_state_prep_us,
		    r.descriptor_prep_us, r.pipeline_lookup_us, r.pipeline_key_build_us, r.buffer_prep_us,
		    r.command_recording_us, r.barriers_us, r.sync_waits_us, r.write_faults,
		    r.stream_buffer_alloc_kb, r.stream_buffer_wait_us);
	}
	out.close();
	Log::WriteToConsoleAndLog(
	    fmt::format("[AuditProfiler] Exported {} per-frame time series records to {}\n",
	                g_frame_timeseries.size(), filepath));
	return true;
}

bool ExportAll(const std::string& json_path, const std::string& csv_path,
               const ProfileSnapshot* existing_snap) {
	ProfileSnapshot        local_snap;
	const ProfileSnapshot* snap_ptr = existing_snap;
	if (snap_ptr == nullptr) {
		local_snap = TakeSnapshot();
		snap_ptr   = &local_snap;
	}
	const bool  json_ok = ExportJson(json_path, snap_ptr);
	const bool  csv_ok  = ExportCsv(csv_path, snap_ptr);
	const auto& td      = snap_ptr->time_distribution;
	if (td.total_workload_ms > 0.0) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "\n[AuditProfiler] === DISTRIBUCION TRIPARTITA (COLA / CPU / GPU WAIT) ===\n"
		    "  1. Tiempo en Cola / Backpressure:  {:8.2f} ms ({:5.1f}%)\n"
		    "     - Submission Queue:             {:8.2f} ms\n"
		    "     - OpStream / Intake Queue:      {:8.2f} ms\n"
		    "     - Backpressure / Sync Wait:     {:8.2f} ms\n"
		    "  2. Ejecucion Activa en CPU:        {:8.2f} ms ({:5.1f}%)\n"
		    "  3. Espera de GPU (Hardware Sync):  {:8.2f} ms ({:5.1f}%)\n"
		    "     - Timeline / Priority Waits:    {:8.2f} ms\n"
		    "     - StreamBuffer Ring Waits:      {:8.2f} ms\n"
		    "     - Swapchain / Present Waits:    {:8.2f} ms\n"
		    "     - Readback / WriteBack Waits:   {:8.2f} ms\n"
		    "  Clasificacion de Cuello de Botella: {}\n"
		    "==========================================================================\n",
		    td.total_queue_ms, td.queue_pct, td.queue_submission_ms,
		    td.queue_intake_ms + td.queue_opstream_ms, td.queue_backpressure_ms,
		    td.total_cpu_active_ms, td.cpu_active_pct, td.total_gpu_wait_ms, td.gpu_wait_pct,
		    td.gpu_timeline_wait_ms, td.gpu_streambuffer_wait_ms, td.gpu_swapchain_wait_ms,
		    td.gpu_readback_wait_ms, td.BottleneckString()));
	}

	std::string frames_csv_path;
	const auto  dot_pos = json_path.rfind('.');
	if (dot_pos != std::string::npos) {
		frames_csv_path = json_path.substr(0, dot_pos) + "_frames.csv";
	} else {
		frames_csv_path = json_path + "_frames.csv";
	}
	ExportFramesCsv(frames_csv_path);
	extern void PrintProgramSubStatsImpl();
	PrintProgramSubStatsImpl();

	return json_ok && csv_ok;
}

namespace {
struct SubStageStat {
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> ns {0};
};
std::array<SubStageStat, static_cast<size_t>(ProgramSubStage::Count)> g_prog_sub_stats {};
std::atomic<uint64_t>                                                 g_resolve_tex_hits {0};
std::atomic<uint64_t>                                                 g_resolve_tex_cold_misses {0};
std::atomic<uint64_t> g_resolve_tex_conflict_misses {0};
std::atomic<uint64_t> g_shader_map_tls_hits {0};
std::atomic<uint64_t> g_shader_map_tls_misses {0};

const char* SubStageName(ProgramSubStage s) noexcept {
	switch (s) {
		case ProgramSubStage::GetGraphicsPrograms: return "GetGraphicsPrograms";
		case ProgramSubStage::PumpAsync: return "PumpAsync";
		case ProgramSubStage::PrepareProgramVS: return "PrepareProgramVS";
		case ProgramSubStage::PrepareProgramPS: return "PrepareProgramPS";
		case ProgramSubStage::ShaderGetMappedData: return "ShaderGetMappedData";
		case ProgramSubStage::ShaderStaticVertexInfo: return "ShaderStaticVertexInfo";
		case ProgramSubStage::ShaderAttribSemantics: return "ShaderAttribSemantics";
		case ProgramSubStage::ShaderDetectBuffers: return "ShaderDetectBuffers";
		case ProgramSubStage::ShaderStaticInputPS: return "ShaderStaticInputPS";
		case ProgramSubStage::GetShaderParams: return "GetShaderParams";
		case ProgramSubStage::GetProgramVS: return "GetProgramVS";
		case ProgramSubStage::GetProgramPS: return "GetProgramPS";
		case ProgramSubStage::BuildStageStaticKey: return "BuildStageStaticKey";
		case ProgramSubStage::ProgramCacheFind: return "ProgramCacheFind";
		case ProgramSubStage::FindModule: return "FindModule";
		case ProgramSubStage::MaterializeResources: return "MaterializeResources";
		case ProgramSubStage::WalkSrt: return "WalkSrt";
		case ProgramSubStage::BuildResourceSpec: return "BuildResourceSpec";
		case ProgramSubStage::PixelRawParamMask: return "PixelRawParamMask";
		case ProgramSubStage::NoteTargetDraw: return "NoteTargetDraw";
		default: return "Unknown";
	}
}
} // namespace

void RecordProgramSubStage(ProgramSubStage stage, uint64_t ns) noexcept {
	auto& st = g_prog_sub_stats[static_cast<size_t>(stage)];
	st.calls.fetch_add(1, std::memory_order_relaxed);
	st.ns.fetch_add(ns, std::memory_order_relaxed);
}

void RecordResolveTexHit() noexcept {
	g_resolve_tex_hits.fetch_add(1, std::memory_order_relaxed);
}

void RecordResolveTexMiss(bool conflict) noexcept {
	if (conflict) {
		g_resolve_tex_conflict_misses.fetch_add(1, std::memory_order_relaxed);
	} else {
		g_resolve_tex_cold_misses.fetch_add(1, std::memory_order_relaxed);
	}
}

void RecordShaderMapTlsHit(bool hit) noexcept {
	if (hit) {
		g_shader_map_tls_hits.fetch_add(1, std::memory_order_relaxed);
	} else {
		g_shader_map_tls_misses.fetch_add(1, std::memory_order_relaxed);
	}
}

void PrintProgramSubStatsImpl() {
	const uint64_t hits      = g_resolve_tex_hits.exchange(0, std::memory_order_relaxed);
	const uint64_t cold      = g_resolve_tex_cold_misses.exchange(0, std::memory_order_relaxed);
	const uint64_t conflicts = g_resolve_tex_conflict_misses.exchange(0, std::memory_order_relaxed);
	const uint64_t total_tex = hits + cold + conflicts;
	if (total_tex > 0) {
		Log::WriteToConsoleAndLog(
		    fmt::format("[ResolveTexCache] hits={} cold_misses={} conflict_misses={} total={} "
		                "hit_rate={:.2f}%\n",
		                hits, cold, conflicts, total_tex,
		                100.0 * static_cast<double>(hits) / static_cast<double>(total_tex)));
	}
	const uint64_t sm_hits   = g_shader_map_tls_hits.exchange(0, std::memory_order_relaxed);
	const uint64_t sm_misses = g_shader_map_tls_misses.exchange(0, std::memory_order_relaxed);
	if (sm_hits + sm_misses > 0) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[ShaderMapTLS] hits={} misses={} hit_rate={:.2f}%\n", sm_hits, sm_misses,
		    100.0 * static_cast<double>(sm_hits) / static_cast<double>(sm_hits + sm_misses)));
	}
	const uint64_t gp_calls = g_prog_sub_stats[0].calls.load(std::memory_order_relaxed);
	if (gp_calls > 0) {
		Log::WriteToConsoleAndLog("[ProgramProfile] === GetGraphicsPrograms Breakdown ===\n");
		for (size_t i = 0; i < static_cast<size_t>(ProgramSubStage::Count); ++i) {
			const uint64_t c  = g_prog_sub_stats[i].calls.exchange(0, std::memory_order_relaxed);
			const uint64_t ns = g_prog_sub_stats[i].ns.exchange(0, std::memory_order_relaxed);
			if (c > 0) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "  {:24s}: calls={:10d} total_ms={:9.2f} ns/call={:8.2f} ns/draw={:8.2f}\n",
				    SubStageName(static_cast<ProgramSubStage>(i)), c, static_cast<double>(ns) / 1e6,
				    static_cast<double>(ns) / static_cast<double>(c),
				    static_cast<double>(ns) / static_cast<double>(gp_calls)));
			}
		}
	}
}

void CountPriorityWaitSpin() noexcept {
	g_priority_wait_spins.fetch_add(1, std::memory_order_relaxed);
}

void CountPriorityWaitSpinHit() noexcept {
	g_priority_wait_spin_hits.fetch_add(1, std::memory_order_relaxed);
}

void CountPendingOpsFastSkip() noexcept {
	g_pending_ops_fast_skips.fetch_add(1, std::memory_order_relaxed);
}

void RecordCpSequencerOpEmitted() noexcept {
	g_cp_sequencer_ops_emitted.fetch_add(1, std::memory_order_relaxed);
}

void RecordCpSequencerLockstepWait() noexcept {
	g_cp_sequencer_lockstep_waits.fetch_add(1, std::memory_order_relaxed);
}

void RecordCpSequencerSnapshot() noexcept {
	g_cp_sequencer_snapshots_taken.fetch_add(1, std::memory_order_relaxed);
}

void RecordCpSequencerWaitSpin() noexcept {
	g_cp_sequencer_waits_spin.fetch_add(1, std::memory_order_relaxed);
}

void RecordDrawPrepPublished() noexcept {
	g_draw_prep_published.fetch_add(1, std::memory_order_relaxed);
}

void RecordDrawPrepPrepared() noexcept {
	g_draw_prep_prepared.fetch_add(1, std::memory_order_relaxed);
}

void RecordDrawPrepCommitted() noexcept {
	g_draw_prep_committed.fetch_add(1, std::memory_order_relaxed);
}

void RecordDrawPrepFallback() noexcept {
	g_draw_prep_fallback.fetch_add(1, std::memory_order_relaxed);
}

void RecordDrawPrepAvoided(uint64_t ns) noexcept {
	g_draw_prep_avoided.fetch_add(1, std::memory_order_relaxed);
	if (ns > 0) {
		g_draw_prep_avoided_time_ns.fetch_add(ns, std::memory_order_relaxed);
	}
}

const char* GetQueueLatencyTypeName(QueueLatencyType type) noexcept {
	switch (type) {
		case QueueLatencyType::Submission: return "SubmissionQueue";
		case QueueLatencyType::Intake: return "SequencerIntake";
		case QueueLatencyType::OpStream: return "OpStream";
		case QueueLatencyType::Backpressure: return "Backpressure";
		default: return "Unknown";
	}
}

void RecordQueueLatency(QueueLatencyType type, uint64_t latency_ns) noexcept {
	if (!IsEnabled()) {
		return;
	}
	if (g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	const auto idx = static_cast<size_t>(type);
	if (idx >= static_cast<size_t>(QueueLatencyType::Count)) {
		return;
	}
	g_queue_latency_ns[idx].fetch_add(latency_ns, std::memory_order_relaxed);
	g_queue_latency_count[idx].fetch_add(1, std::memory_order_relaxed);
}

} // namespace Common::AuditProfiler
