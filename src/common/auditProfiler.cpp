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
std::atomic<uint64_t> g_baseline_buffer_upload_bytes {0};
std::atomic<uint64_t> g_baseline_texture_upload_bytes {0};
std::atomic<uint64_t> g_baseline_buffer_download_bytes {0};
std::atomic<uint64_t> g_baseline_texture_download_bytes {0};
std::atomic<uint64_t> g_baseline_texture_partial_refreshes {0};
std::atomic<uint64_t> g_baseline_texture_partial_fallbacks {0};
std::atomic<uint64_t> g_baseline_texture_partial_upload_bytes {0};
std::atomic<uint64_t> g_baseline_cadence_held_back {0};
std::atomic<uint64_t> g_baseline_cadence_submits {0};

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

void UpdateMax(std::atomic<uint64_t>& target, uint64_t value) noexcept {
	auto current = target.load(std::memory_order_relaxed);
	while (value > current &&
	       !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
	}
}

} // namespace

bool IsEnabled() noexcept {
	return g_enabled.load(std::memory_order_relaxed);
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
	g_has_completed_snapshot.store(false, std::memory_order_relaxed);
}

void StartCapture(const char* trigger_reason) {
	std::lock_guard lock(g_capture_mutex);
	Reset();

	g_current_phase.store(ExecutionPhase::Gameplay, std::memory_order_relaxed);
	g_trigger_reason = trigger_reason != nullptr ? trigger_reason : "Unknown";

	// Snapshot baseline values of external DebugCounters to compute deltas
	namespace Counters = Common::DebugCounters;
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

	const auto now       = Clock::now();
	g_capture_start_time = now;
	g_capture_end_time   = Clock::time_point {};

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
		default: return "Unknown";
	}
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
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_vblank_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordFlipSubmitted() noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_flip_submitted_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordFlipCompleted() noexcept {
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

void RecordAudioUpdate(uint32_t samples, uint32_t /*freq*/) noexcept {
	if (!IsEnabled() || g_capture_state.load(std::memory_order_relaxed) != CaptureState::Running) {
		return;
	}
	g_audio_updates_count.fetch_add(1, std::memory_order_relaxed);
	g_audio_total_samples.fetch_add(samples, std::memory_order_relaxed);
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
	snapshot.extended_stream_buffer_enabled = Config::ExtendedStreamBufferEnabled();
	snapshot.git_commit                     = KYTY_GIT_VERSION;

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
	    {"config",
	     {
	         {"gpu_submit_cadence", snap.cadence_enabled},
	         {"gpu_submit_cadence_interval_us", snap.cadence_interval_us},
	         {"partial_buffer_refresh", snap.partial_refresh_enabled},
	         {"batch_hot_readbacks", snap.batch_hot_readbacks_enabled},
	         {"extended_stream_buffer", snap.extended_stream_buffer_enabled},
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
	    {"texture_partial_refreshes", snap.texture_partial_refreshes},
	    {"texture_partial_fallbacks", snap.texture_partial_fallbacks},
	    {"texture_partial_upload_mb",
	     static_cast<double>(snap.texture_partial_upload_bytes) / (1024.0 * 1024.0)},
	    {"cadence_submits", snap.cadence_submits},
	    {"cadence_held_back", snap.cadence_held_back},
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
	    {"simulation_speed_ratio", snap.simulation_speed_ratio},
	    {"simulation_speed_percent", snap.simulation_speed_ratio * 100.0},
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
	out << fmt::format("Metadata,CadenceEnabled,{}\n", snap.cadence_enabled ? 1 : 0);
	out << fmt::format("Metadata,CadenceIntervalUs,{}\n", snap.cadence_interval_us);
	out << fmt::format("Metadata,PartialRefreshEnabled,{}\n", snap.partial_refresh_enabled ? 1 : 0);
	out << fmt::format("Metadata,BatchHotReadbacksEnabled,{}\n",
	                   snap.batch_hot_readbacks_enabled ? 1 : 0);
	out << fmt::format("Metadata,ExtendedStreamBufferEnabled,{}\n",
	                   snap.extended_stream_buffer_enabled ? 1 : 0);

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

	out << fmt::format("Optimization,TexturePartialRefreshes,{}\n", snap.texture_partial_refreshes);
	out << fmt::format("Optimization,TexturePartialFallbacks,{}\n", snap.texture_partial_fallbacks);
	out << fmt::format("Optimization,TexturePartialUploadMB,{:.2f}\n",
	                   static_cast<double>(snap.texture_partial_upload_bytes) / (1024.0 * 1024.0));
	out << fmt::format("Optimization,CadenceSubmits,{}\n", snap.cadence_submits);
	out << fmt::format("Optimization,CadenceHeldBack,{}\n", snap.cadence_held_back);

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

bool ExportAll(const std::string& json_path, const std::string& csv_path,
               const ProfileSnapshot* existing_snap) {
	ProfileSnapshot        local_snap;
	const ProfileSnapshot* snap_ptr = existing_snap;
	if (snap_ptr == nullptr) {
		local_snap = TakeSnapshot();
		snap_ptr   = &local_snap;
	}
	const bool json_ok = ExportJson(json_path, snap_ptr);
	const bool csv_ok  = ExportCsv(csv_path, snap_ptr);
	return json_ok && csv_ok;
}

} // namespace Common::AuditProfiler
