#ifndef KYTY_COMMON_AUDIT_PROFILER_H_
#define KYTY_COMMON_AUDIT_PROFILER_H_

#include "common/common.h"
#include "common/pm4OpcodeProfiler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Common::AuditProfiler {

using Clock = std::chrono::steady_clock;

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
	Loading,        ///< Asset loading, shader cache compilation, heavy texture/buffer streaming.
	Warmup,         ///< Scene reached, waiting for cache/streaming stabilization.
	Gameplay,       ///< Interactive steady-state gameplay benchmark.
	Transition,     ///< Level transition or pause screen.
	WorldEntryPrep, ///< Stage 8.1: Preparation for entering a world.
	DescentStart,   ///< Stage 8.1: Spaceship starts descent toward world.
	ActiveDescent,  ///< Stage 8.1: Spaceship descending through atmosphere.
	GroundApproach, ///< Stage 8.1: Spaceship approaching landing site.
	Touchdown,      ///< Stage 8.1: Spaceship landing/touchdown.
	EarlyGameplay,  ///< Stage 8.1: First seconds of player control after touchdown.
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

/// Classification of callers enqueueing priority GPU operations (Stage 8.2).
enum class PriorityOpCaller : uint32_t {
	Unknown = 0,
	ReadHotPublish,   ///< BufferCache::PublishReadHot backing updates.
	Interrupt,        ///< CommandProcessor::ReleaseMemory interrupt events.
	Flip,             ///< Presentation flip completion (CompleteFlip / CompleteCpuFlip).
	AsyncDownload,    ///< BufferCache::DownloadBufferMemory<true> asynchronous completions.
	TextureWriteBack, ///< TextureCache::WriteBackImages storage image write-backs.
	SuspendPoint,     ///< CommandProcessor::SuspendPoint sync release.
	Count
};

/// Classification of callers triggering buffer readbacks from GPU memory (Stage 8.2).
enum class BufferReadbackCaller : uint32_t {
	Unknown = 0,
	PageFault,        ///< Guest thread reading from GPU-modified buffer (HandleFault).
	ColorMetadata,    ///< TextureCache::FindView checking color metadata clear codes.
	IndirectDispatch, ///< RenderCompute reading indirect dispatch arguments.
	ImageHeapRefresh, ///< ImageHeap::Refresh reading descriptors/heap entries.
	ShaderRead,       ///< PipelineCache reading shader guest memory for recompiler.
	Other,
	Count
};

struct CallerWaitStats {
	uint64_t count        = 0;
	uint64_t total_ns     = 0;
	uint64_t max_ns       = 0;
	uint64_t total_bytes  = 0;
	double   total_ms     = 0.0;
	double   avg_us       = 0.0;
	double   ms_per_frame = 0.0;
};

struct SignificantWaitEvent {
	uint64_t      timestamp_us = 0;
	uint64_t      duration_ns  = 0;
	GpuWaitReason reason       = GpuWaitReason::Other;
	uint32_t      caller_id    = 0;
	std::string   caller_name;
	uint64_t      tick_requested = 0;
	uint64_t      tick_completed = 0;
	uint64_t      address        = 0;
	uint64_t      size_bytes     = 0;
	uint32_t      tid            = 0;
};

/// Classification of rendering pipeline stages for fine-grained timing.
enum class PipelineStage : uint32_t {
	Pm4Decode = 0,       ///< PM4 decode loop in CommandProcessor::Process
	Pm4Dispatch,         ///< PM4 packet handler dispatch
	RenderStatePrep,     ///< PrepareDrawRenderState (targets resolution, sample checks)
	PipelineKeyBuild,    ///< MakeGraphicsPipelineKey construction
	PipelineLookup,      ///< PipelineCache MRU and hash table lookup
	DescriptorPrep,      ///< PrepareBindings & CommitBindings
	BarriersTransitions, ///< Resource transitions and pipeline barriers
	BufferPrep,          ///< Vertex & index buffer preparation and commit
	CommandRecording,    ///< EmitDrawPrimitives (vkCmdDraw/vkCmdDrawIndexed)
	CommandEnd,          ///< vkEndCommandBuffer
	Submit,              ///< vkQueueSubmit in CommandScheduler::Submit
	SyncWaits,           ///< Timeline semaphore / MasterSemaphore wait
	Present,             ///< Swapchain presentation (vkQueuePresentKHR)
	DrawPrep,            ///< Draw-prep parallel / inline snapshot and state preparation
	SequencerWait,       ///< CP sequencer wait on resolver progress / answer
	DrawPrepWorker,      ///< Background worker thread preparing draw state
	DrawPrepCommitWait,  ///< GPU thread waiting for draw prep worker completion
	Count
};

/// Temporal trace event types for hitch correlation.
enum class TraceEventType : uint16_t {
	None = 0,
	TimelineWait,
	VulkanSubmit,
	StreamBufferWait,
	BufferReadback,
	PriorityOperation,
	SwapchainPresent,
	WriteFault,
	Reprotect,
	PipelineCompile,
	Other,
	Count
};

struct TraceEvent {
	uint64_t       timestamp_us = 0;
	uint64_t       duration_ns  = 0;
	uint64_t       tick         = 0;
	uint64_t       data         = 0;
	uint32_t       tid          = 0;
	TraceEventType type         = TraceEventType::None;
};

struct PipelineStageMetrics {
	uint64_t count           = 0;
	uint64_t total_ns        = 0;
	uint64_t max_ns          = 0;
	uint64_t min_ns          = 0;
	double   total_ms        = 0.0;
	double   avg_us          = 0.0;
	double   max_ms          = 0.0;
	double   ms_per_frame    = 0.0;
	double   pct_thread_time = 0.0;
};

/// Classification of internal queue & buffering latency sources.
enum class QueueLatencyType : uint32_t {
	Submission = 0, ///< GuestGpu::Enqueue -> GuestGpu::ThreadRun
	Intake,         ///< GuestGpu::Enqueue -> Sequencer::Run
	OpStream,       ///< OpStream::Emit -> ResolveSubmission (OpStream::Peek)
	Backpressure,   ///< Producer wait / blocking on ring slot / progress
	Count
};

/// Tripartite workload time distribution: Queue vs CPU Active vs GPU Hardware Wait.
struct WorkloadTimeDistribution {
	// 1. Queue & Buffering Latency (OrganizaciÃ³n del trabajo)
	uint64_t queue_submission_count   = 0;
	double   queue_submission_ms      = 0.0;
	uint64_t queue_intake_count       = 0;
	double   queue_intake_ms          = 0.0;
	uint64_t queue_opstream_count     = 0;
	double   queue_opstream_ms        = 0.0;
	uint64_t queue_backpressure_count = 0;
	double   queue_backpressure_ms    = 0.0;
	double   total_queue_ms           = 0.0;
	double   queue_pct                = 0.0;

	// 2. CPU Active Execution (TraducciÃ³n y Driver)
	double cpu_pm4_decode_ms       = 0.0;
	double cpu_pm4_dispatch_ms     = 0.0;
	double cpu_render_state_ms     = 0.0;
	double cpu_pipeline_keys_ms    = 0.0;
	double cpu_descriptors_ms      = 0.0;
	double cpu_buffers_ms          = 0.0;
	double cpu_command_record_ms   = 0.0;
	double cpu_submit_overhead_ms  = 0.0;
	double cpu_draw_prep_worker_ms = 0.0;
	double total_cpu_active_ms     = 0.0;
	double cpu_active_pct          = 0.0;

	// 3. GPU Hardware Wait (SincronizaciÃ³n y Espera de GPU)
	double gpu_timeline_wait_ms     = 0.0;
	double gpu_streambuffer_wait_ms = 0.0;
	double gpu_swapchain_wait_ms    = 0.0;
	double gpu_readback_wait_ms     = 0.0;
	double gpu_other_wait_ms        = 0.0;
	double total_gpu_wait_ms        = 0.0;
	double gpu_wait_pct             = 0.0;

	// Combined measured workload
	double total_workload_ms = 0.0;

	enum class Bottleneck : uint32_t {
		Undetermined = 0,
		GpuBound,
		CpuBound,
		QueueSyncBound
	} bottleneck = Bottleneck::Undetermined;

	[[nodiscard]] const char* BottleneckString() const noexcept {
		switch (bottleneck) {
			case Bottleneck::GpuBound: return "GPU_BOUND (Hardware/Vulkan Sync)";
			case Bottleneck::CpuBound: return "CPU_BOUND (Translation/Driver Overhead)";
			case Bottleneck::QueueSyncBound:
				return "QUEUE_SYNC_BOUND (Buffering/Backpressure Stalls)";
			default: return "UNDETERMINED";
		}
	}
};

struct DetailedHitchInfo {
	uint64_t    frame_index  = 0;
	double      frame_ms     = 0.0;
	uint64_t    timestamp_ms = 0;
	std::string dominant_blocker;
	double      blocker_duration_ms = 0.0;
	uint32_t    blocker_thread_id   = 0;
	uint64_t    blocker_tick        = 0;
	uint64_t    blocker_address     = 0;
	std::string correlation_confidence;
};

struct StreamBufferSnapshotStats {
	uint32_t normal_size_mb   = 64;
	uint32_t actual_size_mb   = 64;
	uint32_t download_size_mb = 64;
	uint64_t total_allocs     = 0;
	uint64_t total_wraps      = 0;
	uint64_t total_bytes      = 0;
	uint64_t high_water_bytes = 0;
	uint64_t wait_events      = 0;
	uint64_t wait_total_ns    = 0;
	uint64_t wait_max_ns      = 0;
	uint64_t last_wait_tick   = 0;
	double   total_mb         = 0.0;
	double   mb_per_frame     = 0.0;
	double   high_water_mb    = 0.0;
	double   wait_total_ms    = 0.0;
	double   wait_avg_ms      = 0.0;
	double   wait_max_ms      = 0.0;
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
	bool     readback_neighbours_enabled    = false;
	bool     wide_readback_window_enabled   = false;
	bool     fault_ahead_enabled            = false;
	bool     tile_layout_cache_enabled      = false;
	bool     binding_reuse_enabled          = false;
	bool     trim_scratch_memory_enabled    = false;
	bool     compact_image_readback_enabled = false;
	bool     read_buffer_reuse_enabled      = false;
	bool     resource_prep_reuse_enabled    = false;
	bool     extended_stream_buffer_enabled = false;
	bool     redundant_bind_elision_enabled = true;
	bool     pipeline_cache_mru_enabled     = true;
	bool     pipeline_key_cache_enabled     = true;
	bool     draw_state_dirty_check_enabled = false;
	bool     ensure_pipeline_cache_enabled  = true;
	bool     descriptor_cache_enabled       = true;
	bool     program_cache_mru_enabled      = true;
	bool     coalesce_read_faults_enabled   = true;
	bool     gpu_timestamps_enabled         = true;
	bool     diagnostic_skip_water_enabled  = false;
	bool     fast_pending_ops_enabled       = true;
	bool     priority_spin_wait_enabled     = true;
	uint32_t priority_spin_duration_us      = 100;

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
	std::string git_revision;
	std::string git_hash;
	std::string build_timestamp;
	std::string compiler;
	std::string binary_path;

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
	uint64_t neighbour_readback_count    = 0;
	uint64_t neighbour_readback_bytes    = 0;
	uint64_t image_binding_reset_entries = 0, image_binding_duplicate_skips = 0;
	bool     image_binding_dedup_enabled = false;
	uint64_t binding_range_reuses        = 0;
	uint64_t binding_lookup_reuses       = 0;
	uint64_t tile_layout_hits = 0, tile_layout_misses = 0;
	uint64_t read_buffer_hits = 0, read_buffer_misses = 0;
	uint64_t scratch_freed_bytes = 0, scratch_retirements = 0, scratch_allocations = 0;
	uint64_t compact_readback_copies = 0, compact_readback_fallbacks = 0;
	uint64_t readback_prefill_avoided_bytes = 0, readback_staging_saved_bytes = 0;
	uint64_t resource_prep_hits = 0, resource_prep_misses = 0;
	uint64_t binding_sampler_reuses     = 0;
	uint64_t binding_upload_reuses      = 0;
	uint64_t binding_upload_reuse_bytes = 0;

	// Optimization instrumentation metrics (over the captured interval)
	uint64_t texture_partial_refreshes    = 0;
	uint64_t texture_partial_fallbacks    = 0;
	uint64_t texture_partial_upload_bytes = 0;
	uint64_t cadence_held_back            = 0;
	uint64_t cadence_submits              = 0;
	uint64_t priority_wait_spins          = 0;
	uint64_t priority_wait_spin_hits      = 0;
	uint64_t pending_ops_fast_skips       = 0;
	bool     cp_sequencer_enabled         = false;
	uint64_t cp_sequencer_ops_emitted     = 0;
	uint64_t cp_sequencer_lockstep_waits  = 0;
	uint64_t cp_sequencer_snapshots_taken = 0;
	uint64_t cp_sequencer_waits_spin      = 0;
	bool     draw_prep_enabled            = false;
	uint64_t draw_prep_published          = 0;
	uint64_t draw_prep_prepared           = 0;
	uint64_t draw_prep_committed          = 0;
	uint64_t draw_prep_fallback           = 0;
	uint64_t draw_prep_avoided            = 0;
	uint64_t draw_prep_avoided_time_ns    = 0;
	double   draw_prep_duplication_pct    = 0.0;

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

	// Timing and simulation metrics (over the captured interval)
	uint64_t total_vblanks         = 0;
	double   vblank_rate_hz        = 0.0;
	uint64_t total_flips_submitted = 0;
	uint64_t total_flips_completed = 0;
	double   flip_rate_hz          = 0.0;
	uint64_t total_flip_wait_ns    = 0;
	double   total_flip_wait_ms    = 0.0;
	double   avg_flip_wait_ms      = 0.0;
	uint64_t audio_updates_count   = 0;
	uint64_t audio_total_samples   = 0;
	double   audio_effective_hz    = 0.0;
	/// Theoretical simulation speed ratio calculated as (flips_completed * (1/60s)) /
	/// real_duration_s. Note: This is an estimated model conditioned on a 1/60s fixed simulation
	/// timestep per completed flip, NOT an independently measured physics simulation rate.
	double simulation_speed_ratio = 0.0;

	// Graphics workload metrics (over the captured interval)
	uint64_t total_draws                  = 0;
	double   draws_per_frame              = 0.0;
	double   draws_per_second             = 0.0;
	uint64_t total_draw_instances         = 0;
	double   instances_per_frame          = 0.0;
	uint64_t total_indirect_draws         = 0;
	uint64_t total_dispatches             = 0;
	double   dispatches_per_frame         = 0.0;
	double   dispatches_per_second        = 0.0;
	uint64_t pipelines_created            = 0;
	uint64_t shaders_compiled             = 0;
	uint64_t gpu_thread_busy_ns           = 0;
	double   gpu_thread_busy_ms           = 0.0;
	double   gpu_thread_busy_ms_per_frame = 0.0;
	uint64_t pipeline_cache_lookups       = 0;
	uint64_t pipeline_cache_hits          = 0;
	double   pipeline_cache_hit_pct       = 0.0;
	uint64_t redundant_pipeline_binds     = 0;
	uint64_t redundant_vertex_binds       = 0;
	uint64_t redundant_index_binds        = 0;

	// Stage 8 additions: fine-grained pipeline stage timing metrics
	std::array<PipelineStageMetrics, static_cast<size_t>(PipelineStage::Count)> pipeline_stages {};
	Pm4ProfileSnapshot                                                          pm4_profile;
	double pm4_dispatch_inclusive_ms           = 0.0;
	double pm4_dispatch_exclusive_ms           = 0.0;
	double pm4_dispatch_exclusive_ms_per_frame = 0.0;

	// Stage 8 additions: CPU and GPU execution & synchronization separation
	double   gpu_thread_wall_clock_ms       = 0.0;
	double   gpu_thread_cpu_time_ms         = 0.0;
	double   gpu_thread_cpu_utilization_pct = 0.0;
	double   gpu_thread_wait_time_ms        = 0.0;
	bool     gpu_timestamps_supported       = false;
	double   gpu_execution_time_ms          = 0.0;
	double   gpu_execution_avg_per_frame_ms = 0.0;
	uint64_t gpu_queries_recorded           = 0;

	// Stage 8 additions: StreamBuffer audit snapshot
	StreamBufferSnapshotStats stream_buffer_stats {};

	// Stage 8 additions: Detailed hitch correlation records
	std::vector<DetailedHitchInfo> detailed_hitches;

	// Stage 8.2 additions: Synchronization and Readback caller breakdowns
	std::array<CallerWaitStats, static_cast<size_t>(PriorityOpCaller::Count)>
	    priority_op_callers {};
	std::array<CallerWaitStats, static_cast<size_t>(BufferReadbackCaller::Count)>
	                                  buffer_readback_callers {};
	std::vector<SignificantWaitEvent> significant_waits;
	uint64_t                          water_draws_estimated = 0;

	// Tripartite workload distribution (Queue vs CPU vs GPU Wait)
	WorkloadTimeDistribution time_distribution {};
};

[[nodiscard]] bool               IsEnabled() noexcept;
void                             SetEnabled(bool enabled) noexcept;
void                             SetExportPath(std::string_view path);
[[nodiscard]] const std::string& GetExportPath();

// Independent one-second journal; survives stalled presentation and capture restarts.
// No inference of physics/simulation speed from flips, audio or virtual clocks.
bool StartLiveDiagnostics(const std::string& path);
void StopLiveDiagnostics();

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
void AdvancePhase() noexcept;

/// Records a write fault on [fault_vaddr] caused by instruction at [rip], taking [duration_ns].
/// Zero allocation, lock-free, safe to call from exception handlers.
void RecordFault(uint64_t fault_vaddr, uint64_t rip, uint64_t duration_ns) noexcept;

/// Records a page re-protection event (when pages in [vaddr, vaddr+size) are made read-only).
void RecordReprotect(uint64_t vaddr, uint64_t size) noexcept;

/// Records a timeline semaphore wait event.
void RecordGpuWait(GpuWaitReason reason, uint64_t wait_ns) noexcept;

/// Records priority operation spin-wait attempts, hits, and lock-free fast skips.
void CountPriorityWaitSpin() noexcept;
void CountPriorityWaitSpinHit() noexcept;
void CountPendingOpsFastSkip() noexcept;

enum class ProgramSubStage : uint8_t {
	GetGraphicsPrograms = 0,
	PumpAsync,
	PrepareProgramVS,
	PrepareProgramPS,
	ShaderGetMappedData,
	ShaderStaticVertexInfo,
	ShaderAttribSemantics,
	ShaderDetectBuffers,
	ShaderStaticInputPS,
	GetShaderParams,
	GetProgramVS,
	GetProgramPS,
	BuildStageStaticKey,
	ProgramCacheFind,
	FindModule,
	MaterializeResources,
	WalkSrt,
	BuildResourceSpec,
	PixelRawParamMask,
	NoteTargetDraw,
	Count
};

void RecordProgramSubStage(ProgramSubStage stage, uint64_t ns) noexcept;
void RecordResolveTexHit() noexcept;
void RecordResolveTexMiss(bool conflict) noexcept;
void RecordShaderMapTlsHit(bool hit) noexcept;

struct ScopedProgramSubStage {
	ProgramSubStage   stage;
	bool              enabled;
	Clock::time_point start;
	ScopedProgramSubStage(ProgramSubStage s, bool en) noexcept
	    : stage(s), enabled(en), start(en ? Clock::now() : Clock::time_point {}) {}
	~ScopedProgramSubStage() {
		if (enabled) {
			RecordProgramSubStage(
			    stage, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			                                     Clock::now() - start)
			                                     .count()));
		}
	}
};

/// Records a vkQueueSubmit event.
void RecordSubmit(SubmitReason reason) noexcept;

/// Records internal GPU-to-GPU data copies.
void RecordInternalGpuTransfer(uint64_t bytes) noexcept;

/// Records a frame duration for percentile calculation.
void RecordFrame(double frame_ms) noexcept;

/// Records a VBlank tick from the presentation thread.
void RecordVblank() noexcept;

/// Records a frame flip submission to the flip queue.
void RecordFlipSubmitted() noexcept;

/// Records a completed presentation flip.
void RecordFlipCompleted() noexcept;

/// Records time spent waiting on the flip queue or flip event.
void RecordFlipWait(uint64_t wait_ns) noexcept;

/// Records an audio sample batch output.
void RecordAudioUpdate(uint32_t samples, uint32_t freq) noexcept;

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

/// Records fine-grained timing for a rendering pipeline stage.
void RecordPipelineStage(PipelineStage stage, uint64_t duration_ns) noexcept;

/// Returns human-readable label for rendering pipeline stage.
[[nodiscard]] const char* GetPipelineStageName(PipelineStage stage) noexcept;

/// Records a temporal trace event into the preallocated ring buffer.
void RecordTraceEvent(TraceEventType type, uint64_t duration_ns, uint64_t tick = 0,
                      uint64_t data = 0) noexcept;

/// Returns human-readable label for trace event type.
[[nodiscard]] const char* GetTraceEventTypeName(TraceEventType type) noexcept;

/// Records StreamBuffer allocation and wraparound event.
void RecordStreamBufferAlloc(uint64_t bytes, bool wrap, uint64_t high_water = 0) noexcept;

/// Records StreamBuffer wait event.
void RecordStreamBufferWaitEvent(uint64_t wait_ns, uint64_t tick) noexcept;

/// Notifies profiler of GPU thread start for thread CPU time sampling.
void NoteGpuThreadStart() noexcept;

/// Notifies profiler of GPU thread frame boundary.
void NoteGpuThreadFrame() noexcept;

/// Records physical GPU execution time from timestamps.
void RecordGpuExecutionTime(uint64_t duration_ns) noexcept;

/// Stage 8.2 additions: Callers and significant wait tracking
[[nodiscard]] const char* GetPriorityOpCallerName(PriorityOpCaller caller) noexcept;
[[nodiscard]] const char* GetBufferReadbackCallerName(BufferReadbackCaller caller) noexcept;
void RecordPriorityOpWait(PriorityOpCaller caller, uint64_t wait_ns, uint64_t tick,
                          uint64_t known_tick) noexcept;
void RecordBufferReadbackWait(BufferReadbackCaller caller, uint64_t wait_ns, uint64_t tick,
                              uint64_t known_tick, uint64_t address = 0,
                              uint64_t size = 0) noexcept;
void RecordSignificantWait(GpuWaitReason reason, uint32_t caller_id, const char* caller_name,
                           uint64_t wait_ns, uint64_t tick_req, uint64_t tick_done,
                           uint64_t address = 0, uint64_t size = 0) noexcept;
void RecordWaterDraw() noexcept;

/// Stage 8.4B additions: CP Sequencer and PM4 Parallelization telemetry
void RecordCpSequencerOpEmitted() noexcept;
void RecordCpSequencerLockstepWait() noexcept;
void RecordCpSequencerSnapshot() noexcept;
void RecordCpSequencerWaitSpin() noexcept;

/// Stage 8.4C additions: DrawPrep telemetry
void RecordDrawPrepPublished() noexcept;
void RecordDrawPrepPrepared() noexcept;
void RecordDrawPrepCommitted() noexcept;
void RecordDrawPrepFallback() noexcept;
void RecordDrawPrepAvoided(uint64_t ns = 0) noexcept;

/// Tripartite workload time distribution helpers
[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}
void                      RecordQueueLatency(QueueLatencyType type, uint64_t latency_ns) noexcept;
[[nodiscard]] const char* GetQueueLatencyTypeName(QueueLatencyType type) noexcept;

/// RAII stage timer; nested PM4 attribution is opt-in during captures.
class ScopedStageTimer {
public:
	explicit ScopedStageTimer(PipelineStage stage) noexcept
	    : m_stage(stage), m_active(IsCaptureRunning()) {
		if (m_active) {
			m_start     = std::chrono::steady_clock::now();
			m_pm4_child = g_pm4_profiling_active.load(std::memory_order_relaxed) &&
			              BeginPm4Child(m_start, IsPm4Wait());
		}
	}
	~ScopedStageTimer() noexcept {
		if (m_active) {
			const auto end = std::chrono::steady_clock::now();
			if (m_pm4_child) EndPm4Child(end, IsPm4Wait());
			const auto ns = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_start).count());
			RecordPipelineStage(m_stage, ns);
		}
	}
	KYTY_CLASS_NO_COPY(ScopedStageTimer);

private:
	bool IsPm4Wait() const noexcept {
		return m_stage == PipelineStage::SyncWaits || m_stage == PipelineStage::SequencerWait ||
		       m_stage == PipelineStage::DrawPrepCommitWait;
	}
	bool                                  m_pm4_child = false;
	PipelineStage                         m_stage;
	bool                                  m_active;
	std::chrono::steady_clock::time_point m_start;
};

} // namespace Common::AuditProfiler

#endif // KYTY_COMMON_AUDIT_PROFILER_H_
