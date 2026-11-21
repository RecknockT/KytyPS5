#ifndef KYTY_COMMON_AUDIT_PROFILER_H_
#define KYTY_COMMON_AUDIT_PROFILER_H_

#include "common/common.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Common::AuditProfiler {

/// State of an audit capture session.
enum class CaptureState : uint32_t {
	Idle = 0,         ///< Profiler is disabled or inactive.
	WaitingForHotkey, ///< Waiting for user to press F9 in target scene (warmup phase).
	Scheduled,        ///< Waiting for configured start delay before starting recording.
	Running,          ///< Active measurement session in progress.
	Completed,        ///< Session completed target duration and exported results.
	Interrupted       ///< Session stopped before target duration (e.g. emulator quit or user stop).
};

/// Execution phase classification (for isolating Loading vs Gameplay).
enum class ExecutionPhase : uint32_t {
	Initialization = 0, ///< Emulator boot, module loading, Vulkan initialization.
	Loading,    ///< Asset loading, shader cache compilation, heavy texture/buffer streaming.
	Warmup,     ///< Scene reached, waiting for cache/streaming stabilization.
	Gameplay,   ///< Interactive steady-state gameplay benchmark.
	Transition, ///< Level transition or pause screen.
	Count
};

struct PhaseMetrics {
	uint64_t frame_count    = 0;
	double   total_frame_ms = 0.0;
	uint64_t faults         = 0;
	uint64_t reprotects     = 0;
	uint64_t upload_bytes   = 0;
	uint64_t readback_bytes = 0;
	uint64_t submits        = 0;
	uint64_t gpu_wait_ns    = 0;
};

/// Classification of GPU wait causes on MasterSemaphore / Timeline Semaphores.
enum class GpuWaitReason : uint32_t {
	Other = 0,
	TextureWriteBack,        ///< CPU waiting for GPU texture write-back to host memory.
	BufferReadback,          ///< CPU waiting for GPU buffer download.
	BufferGarbageCollection, ///< CPU waiting for GPU to finish work before freeing unreferenced
	                         ///< buffers.
	BufferSuspendFlush,      ///< Direct memory unmap/suspend flushing GPU-dirty bytes.
	StreamBufferWait,        ///< Allocation waiting for ring-buffer space.
	OcclusionQueries,        ///< CPU waiting for occlusion query results.
	DescriptorHeap,          ///< Descriptor heap recycling wait.
	CommandSchedulerDrain,   ///< Explicit flush/drain of the command scheduler.
	PriorityOperation,       ///< Waiting on priority GPU operations.
	RayTracing,              ///< Acceleration structure builds/updates.
	Swapchain,               ///< Presentation and frame acquire waiting for swapchain slots.
	FaultManager,            ///< Processing GPU hardware fault buffer areas.
	Count
};

/// Classification of vkQueueSubmit triggers.
enum class SubmitReason : uint32_t {
	Other = 0,
	Cadence,         ///< Periodic submit at PM4 packet boundary triggered by submit cadence.
	ExplicitFlush,   ///< Explicit flush from renderer or command processor.
	Synchronization, ///< Submit required prior to waiting on a fence/semaphore.
	Presentation,    ///< Frame presentation submit.
	Drain,           ///< Pipeline drain or unmap drain.
	Count
};

struct HotPageInfo {
	uint64_t page_vaddr      = 0;
	uint64_t fault_count     = 0;
	uint64_t reprotect_count = 0;
};

struct HotRipInfo {
	uint64_t rip         = 0;
	uint64_t fault_count = 0;
};

struct HotThreadInfo {
	uint32_t tid         = 0;
	uint64_t fault_count = 0;
	char     name[32]    = {};
};

struct SlowFrameInfo {
	uint64_t frame_index  = 0;
	double   frame_ms     = 0.0;
	uint64_t timestamp_ms = 0; // Monotonic ms since capture start
};

struct WaitReasonStats {
	uint64_t count    = 0;
	uint64_t total_ns = 0;
	uint64_t max_ns   = 0;
};

struct SubmitReasonStats {
	uint64_t count = 0;
};

struct ProfileSnapshot {
	bool     enabled                        = false;
	bool     cadence_enabled                = false;
	uint64_t cadence_interval_us            = 1000;
	bool     partial_refresh_enabled        = false;
	bool     batch_hot_readbacks_enabled    = false;
	bool     extended_stream_buffer_enabled = false;

	// Capture lifecycle metadata
	std::string capture_id;
	std::string capture_state_str; // "Completed", "Interrupted", "Running", "Continuous",
	                               // "WaitingForHotkey"
	std::string capture_phase_str; // "Gameplay", "Loading", "Warmup", "Mixed"
	std::string trigger_reason;    // "ManualHotkeyF9", "StartupScheduled", etc.
	uint64_t    capture_real_duration_ms    = 0; // Monotonic elapsed time of the capture
	uint32_t    capture_target_duration_sec = 0;
	uint32_t    capture_delay_sec           = 0;
	std::string git_commit;

	// Phase-separated metrics
	PhaseMetrics phase_loading {};
	PhaseMetrics phase_gameplay {};

	// Write-tracking fault metrics (over the captured interval)
	uint64_t total_faults     = 0;
	uint64_t total_reprotects = 0;
	uint64_t total_handler_ns = 0;
	uint64_t max_handler_ns   = 0;
	double   avg_handler_us   = 0.0;

	// Timeline semaphore waits classified by reason (over the captured interval)
	std::array<WaitReasonStats, static_cast<size_t>(GpuWaitReason::Count)> waits {};

	// Submissions classified by reason (over the captured interval)
	std::array<SubmitReasonStats, static_cast<size_t>(SubmitReason::Count)> submits {};

	// Memory transfer metrics (in bytes over the captured interval)
	uint64_t cpu_to_gpu_upload_bytes     = 0;
	uint64_t gpu_to_cpu_readback_bytes   = 0;
	uint64_t internal_gpu_transfer_bytes = 0;

	// Optimization instrumentation metrics (over the captured interval)
	uint64_t texture_partial_refreshes    = 0;
	uint64_t texture_partial_fallbacks    = 0;
	uint64_t texture_partial_upload_bytes = 0;
	uint64_t cadence_held_back            = 0;
	uint64_t cadence_submits              = 0;

	// Frametime metrics (in milliseconds, calculated over all frames of the captured window)
	double   p50_frame_ms  = 0.0;
	double   p90_frame_ms  = 0.0;
	double   p95_frame_ms  = 0.0;
	double   p99_frame_ms  = 0.0;
	double   avg_frame_ms  = 0.0;
	double   min_frame_ms  = 0.0;
	double   max_frame_ms  = 0.0;
	double   effective_fps = 0.0; // total_frames / (real_duration_ms / 1000.0)
	double   frametime_fps = 0.0; // 1000.0 / avg_frame_ms
	uint64_t total_frames  = 0;

	// Stuttering and hitch metrics (over the captured interval)
	uint64_t                   hitches_ge_50ms     = 0;
	uint64_t                   hitches_ge_100ms    = 0;
	uint64_t                   hitches_ge_250ms    = 0;
	uint64_t                   hitches_ge_500ms    = 0;
	uint64_t                   hitches_ge_1000ms   = 0;
	double                     hitch_time_total_ms = 0.0;
	std::vector<SlowFrameInfo> slow_frames;

	// Top hotspot lists (over the captured window)
	std::vector<HotPageInfo>   top_pages;
	std::vector<HotRipInfo>    top_rips;
	std::vector<HotThreadInfo> top_threads;
};

[[nodiscard]] bool               IsEnabled() noexcept;
void                             SetEnabled(bool enabled) noexcept;
void                             SetExportPath(std::string_view path);
[[nodiscard]] const std::string& GetExportPath();

/// Initializes capture parameters from config (delay, duration, export path, manual trigger).
void InitCapture(uint32_t delay_sec, uint32_t duration_sec, std::string_view export_path,
                 bool manual_trigger = false);

/// Starts or restarts a capture session. Resets all stats and baseline counters.
void StartCapture(const char* trigger_reason = "Manual");

/// Stops active capture session, records real duration, and optionally triggers export.
void StopCapture(CaptureState state = CaptureState::Completed);

/// Toggles capture state (starts if idle/stopped/waiting; stops and exports if running). Bound to
/// F9.
void ToggleCapture();

[[nodiscard]] CaptureState GetCaptureState() noexcept;
[[nodiscard]] bool         IsCaptureRunning() noexcept;
[[nodiscard]] uint64_t     GetCaptureDurationMs() noexcept;
[[nodiscard]] uint32_t     GetTargetDurationSec() noexcept;

/// Execution phase control and marking
void                         SetExecutionPhase(ExecutionPhase phase) noexcept;
[[nodiscard]] ExecutionPhase GetExecutionPhase() noexcept;
[[nodiscard]] const char*    GetExecutionPhaseName(ExecutionPhase phase) noexcept;
void MarkPhase(ExecutionPhase phase, const char* description = nullptr) noexcept;

/// Records a write fault on [fault_vaddr] caused by instruction at [rip], taking [duration_ns].
/// Zero allocation, lock-free, safe to call from exception handlers.
void RecordFault(uint64_t fault_vaddr, uint64_t rip, uint64_t duration_ns) noexcept;

/// Records a page re-protection event (when pages in [vaddr, vaddr+size) are made read-only).
void RecordReprotect(uint64_t vaddr, uint64_t size) noexcept;

/// Records a timeline semaphore wait event.
void RecordGpuWait(GpuWaitReason reason, uint64_t wait_ns) noexcept;

/// Records a vkQueueSubmit event.
void RecordSubmit(SubmitReason reason) noexcept;

/// Records internal GPU-to-GPU data copies.
void RecordInternalGpuTransfer(uint64_t bytes) noexcept;

/// Records a frame duration for percentile calculation.
void RecordFrame(double frame_ms) noexcept;

/// Captures a point-in-time snapshot of all profiling metrics.
[[nodiscard]] ProfileSnapshot TakeSnapshot();

/// Resets all counters and tables.
void Reset();

/// Exports current profile to a JSON file. If existing_snap is provided, exports that snapshot
/// directly.
bool ExportJson(const std::string& filepath, const ProfileSnapshot* existing_snap = nullptr);

/// Exports current profile to a CSV file. If existing_snap is provided, exports that snapshot
/// directly.
bool ExportCsv(const std::string& filepath, const ProfileSnapshot* existing_snap = nullptr);

/// Takes a single consistent snapshot and exports both JSON and CSV files.
bool ExportAll(const std::string& json_path, const std::string& csv_path,
               const ProfileSnapshot* existing_snap = nullptr);

/// Returns human-readable label for wait reason.
[[nodiscard]] const char* GetWaitReasonName(GpuWaitReason reason) noexcept;

/// Returns human-readable label for submit reason.
[[nodiscard]] const char* GetSubmitReasonName(SubmitReason reason) noexcept;

} // namespace Common::AuditProfiler

#endif // KYTY_COMMON_AUDIT_PROFILER_H_
