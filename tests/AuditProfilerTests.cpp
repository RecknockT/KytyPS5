#include "common/auditProfiler.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Common::AuditProfiler;

class AuditProfilerTest : public ::testing::Test {
protected:
	void SetUp() override {
		Config::Initialize();
		SetEnabled(false);
		Reset();
	}

	void TearDown() override {
		SetEnabled(false);
		Reset();
		Config::Shutdown();
		std::filesystem::remove("test_profile.json");
		std::filesystem::remove("test_profile.csv");
	}
};

TEST_F(AuditProfilerTest, DisabledByDefaultProducesZeroOverheadNoData) {
	EXPECT_FALSE(IsEnabled());

	RecordFault(0x1000'0000, 0x7fff'0000, 1000);
	RecordReprotect(0x1000'0000, 4096);
	RecordGpuWait(GpuWaitReason::BufferReadback, 5000);
	RecordSubmit(SubmitReason::Cadence);
	RecordInternalGpuTransfer(65536);
	RecordFrame(16.6);

	const auto snap = TakeSnapshot();
	EXPECT_FALSE(snap.enabled);
	EXPECT_EQ(snap.total_faults, 0u);
	EXPECT_EQ(snap.total_reprotects, 0u);
	EXPECT_EQ(snap.total_handler_ns, 0u);
	EXPECT_EQ(snap.total_frames, 0u);
	EXPECT_TRUE(snap.top_pages.empty());
	EXPECT_TRUE(snap.top_rips.empty());
	EXPECT_TRUE(snap.top_threads.empty());
}

TEST_F(AuditProfilerTest, RecordsFaultsAndReprotectionsAccurately) {
	SetEnabled(true);
	EXPECT_TRUE(IsEnabled());

	// 3 faults on page 0x2000'0000 from RIP 0x4000
	RecordFault(0x2000'0100, 0x4000, 1500);
	RecordFault(0x2000'0200, 0x4000, 2500);
	RecordFault(0x2000'0300, 0x4000, 3500);

	// 1 fault on page 0x2000'1000 from RIP 0x5000
	RecordFault(0x2000'1000, 0x5000, 4000);

	// Reprotect range spanning 2 pages (0x2000'0000 to 0x2000'1FFF)
	RecordReprotect(0x2000'0000, 8192);

	const auto snap = TakeSnapshot();
	EXPECT_TRUE(snap.enabled);
	EXPECT_EQ(snap.total_faults, 4u);
	EXPECT_EQ(snap.total_reprotects, 2u);
	EXPECT_EQ(snap.total_handler_ns, 11500u);
	EXPECT_EQ(snap.max_handler_ns, 4000u);
	EXPECT_DOUBLE_EQ(snap.avg_handler_us, 11.5 / 4.0);

	ASSERT_GE(snap.top_pages.size(), 2u);
	// Sorted by fault count descending: first page has 3 faults, 1 reprotect
	EXPECT_EQ(snap.top_pages[0].page_vaddr, 0x2000'0000u);
	EXPECT_EQ(snap.top_pages[0].fault_count, 3u);
	EXPECT_EQ(snap.top_pages[0].reprotect_count, 1u);

	EXPECT_EQ(snap.top_pages[1].page_vaddr, 0x2000'1000u);
	EXPECT_EQ(snap.top_pages[1].fault_count, 1u);
	EXPECT_EQ(snap.top_pages[1].reprotect_count, 1u);

	ASSERT_GE(snap.top_rips.size(), 2u);
	EXPECT_EQ(snap.top_rips[0].rip, 0x4000u);
	EXPECT_EQ(snap.top_rips[0].fault_count, 3u);
	EXPECT_EQ(snap.top_rips[1].rip, 0x5000u);
	EXPECT_EQ(snap.top_rips[1].fault_count, 1u);
}

TEST_F(AuditProfilerTest, CategorizesGpuWaitsByOrigin) {
	SetEnabled(true);

	RecordGpuWait(GpuWaitReason::BufferReadback, 10000);
	RecordGpuWait(GpuWaitReason::BufferReadback, 20000);
	RecordGpuWait(GpuWaitReason::TextureWriteBack, 5000);
	RecordGpuWait(GpuWaitReason::Swapchain, 30000);
	RecordGpuWait(GpuWaitReason::BufferGarbageCollection, 8000);

	const auto snap = TakeSnapshot();
	const auto& rb = snap.waits[static_cast<size_t>(GpuWaitReason::BufferReadback)];
	EXPECT_EQ(rb.count, 2u);
	EXPECT_EQ(rb.total_ns, 30000u);
	EXPECT_EQ(rb.max_ns, 20000u);

	const auto& tw = snap.waits[static_cast<size_t>(GpuWaitReason::TextureWriteBack)];
	EXPECT_EQ(tw.count, 1u);
	EXPECT_EQ(tw.total_ns, 5000u);
	EXPECT_EQ(tw.max_ns, 5000u);

	const auto& sw = snap.waits[static_cast<size_t>(GpuWaitReason::Swapchain)];
	EXPECT_EQ(sw.count, 1u);
	EXPECT_EQ(sw.total_ns, 30000u);

	const auto& gc = snap.waits[static_cast<size_t>(GpuWaitReason::BufferGarbageCollection)];
	EXPECT_EQ(gc.count, 1u);
	EXPECT_EQ(gc.total_ns, 8000u);

	// Other reasons should remain zero
	const auto& desc = snap.waits[static_cast<size_t>(GpuWaitReason::DescriptorHeap)];
	EXPECT_EQ(desc.count, 0u);
	EXPECT_EQ(desc.total_ns, 0u);
}

TEST_F(AuditProfilerTest, CategorizesSubmitReasons) {
	SetEnabled(true);

	RecordSubmit(SubmitReason::Cadence);
	RecordSubmit(SubmitReason::Cadence);
	RecordSubmit(SubmitReason::ExplicitFlush);
	RecordSubmit(SubmitReason::Synchronization);
	RecordSubmit(SubmitReason::Presentation);
	RecordSubmit(SubmitReason::Drain);

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Cadence)].count, 2u);
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::ExplicitFlush)].count, 1u);
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Synchronization)].count, 1u);
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Presentation)].count, 1u);
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Drain)].count, 1u);
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Other)].count, 0u);
}

TEST_F(AuditProfilerTest, ComputesExactFrametimePercentilesP50P95P99) {
	SetEnabled(true);

	// Record 100 sorted frames: 1.0 ms to 100.0 ms
	for (int i = 1; i <= 100; ++i) {
		RecordFrame(static_cast<double>(i));
	}

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_frames, 100u);
	EXPECT_DOUBLE_EQ(snap.p50_frame_ms, 51.0);
	EXPECT_DOUBLE_EQ(snap.p95_frame_ms, 96.0);
	EXPECT_DOUBLE_EQ(snap.p99_frame_ms, 100.0);
	EXPECT_DOUBLE_EQ(snap.max_frame_ms, 100.0);
	EXPECT_NEAR(snap.avg_frame_ms, 50.5, 0.01);
}

TEST_F(AuditProfilerTest, ConcurrentRecordingIsThreadSafeAndLockFree) {
	SetEnabled(true);

	constexpr int NUM_THREADS = 8;
	constexpr int ITERATIONS = 1000;
	std::vector<std::thread> threads;
	threads.reserve(NUM_THREADS);

	for (int t = 0; t < NUM_THREADS; ++t) {
		threads.emplace_back([t] {
			const uint64_t vaddr = 0x3000'0000 + static_cast<uint64_t>(t % 4) * 4096;
			const uint64_t rip = 0x5000'0000 + static_cast<uint64_t>(t);
			for (int i = 0; i < ITERATIONS; ++i) {
				RecordFault(vaddr, rip, 100);
				RecordReprotect(vaddr, 4096);
				RecordGpuWait(GpuWaitReason::BufferReadback, 200);
				RecordSubmit(SubmitReason::Cadence);
				RecordFrame(16.6);
			}
		});
	}

	for (auto& th: threads) {
		th.join();
	}

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_faults, static_cast<uint64_t>(NUM_THREADS * ITERATIONS));
	EXPECT_EQ(snap.total_reprotects, static_cast<uint64_t>(NUM_THREADS * ITERATIONS));
	EXPECT_EQ(snap.waits[static_cast<size_t>(GpuWaitReason::BufferReadback)].count,
	          static_cast<uint64_t>(NUM_THREADS * ITERATIONS));
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Cadence)].count,
	          static_cast<uint64_t>(NUM_THREADS * ITERATIONS));
}

TEST_F(AuditProfilerTest, ExportsJsonAndCsvCorrectly) {
	SetEnabled(true);

	RecordFault(0x4000'1000, 0x6000'1000, 2000);
	RecordReprotect(0x4000'1000, 4096);
	RecordGpuWait(GpuWaitReason::BufferReadback, 15000);
	RecordSubmit(SubmitReason::Cadence);
	RecordFrame(33.3);

	EXPECT_TRUE(ExportAll("test_profile.json", "test_profile.csv"));

	EXPECT_TRUE(std::filesystem::exists("test_profile.json"));
	EXPECT_TRUE(std::filesystem::exists("test_profile.csv"));

	// Validate JSON
	std::ifstream json_file("test_profile.json");
	ASSERT_TRUE(json_file.is_open());
	nlohmann::json j;
	json_file >> j;

	EXPECT_TRUE(j.contains("configuration"));
	EXPECT_TRUE(j.contains("frametime_metrics_ms"));
	EXPECT_TRUE(j.contains("write_tracking_metrics"));
	EXPECT_TRUE(j.contains("gpu_waits_breakdown"));
	EXPECT_TRUE(j.contains("vk_queue_submits_breakdown"));
	EXPECT_TRUE(j.contains("top_hot_pages"));
	EXPECT_TRUE(j.contains("top_faulting_rips"));
	EXPECT_TRUE(j.contains("optimization_metrics"));

	EXPECT_EQ(j["write_tracking_metrics"]["total_faults"].get<uint64_t>(), 1u);
	EXPECT_EQ(j["write_tracking_metrics"]["total_reprotects"].get<uint64_t>(), 1u);

	// Validate CSV
	std::ifstream csv_file("test_profile.csv");
	ASSERT_TRUE(csv_file.is_open());
	std::string line;
	bool found_frametime_section = false;
	bool found_faults_section = false;
	bool found_opt_section = false;
	while (std::getline(csv_file, line)) {
		if (line.find("TotalFrames") != std::string::npos) {
			found_frametime_section = true;
		}
		if (line.find("TotalFaults") != std::string::npos) {
			found_faults_section = true;
		}
		if (line.find("TexturePartialRefreshes") != std::string::npos) {
			found_opt_section = true;
		}
	}
	EXPECT_TRUE(found_frametime_section);
	EXPECT_TRUE(found_faults_section);
	EXPECT_TRUE(found_opt_section);
}

TEST_F(AuditProfilerTest, ColdReprotectsDoNotSaturateTableAndHotPagesAreRetained) {
	SetEnabled(true);

	// Simulate protecting a large 64MB buffer (16,384 pages) that was never faulted
	RecordReprotect(0x8000'0000, 64 * 1024 * 1024);
	EXPECT_EQ(TakeSnapshot().total_reprotects, 16384u);
	// Cold pages with 0 faults must NOT be in top_pages
	EXPECT_TRUE(TakeSnapshot().top_pages.empty());

	// Now record real faults on a hot page
	const uint64_t hot_page = 0x5000'0000;
	for (int i = 0; i < 50; ++i) {
		RecordFault(hot_page + 0x20, 0x9200'03e55, 1000);
	}
	// And reprotect that hot page twice
	RecordReprotect(hot_page, 4096);
	RecordReprotect(hot_page, 4096);

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_faults, 50u);
	EXPECT_EQ(snap.total_reprotects, 16384u + 2u);
	ASSERT_EQ(snap.top_pages.size(), 1u);
	EXPECT_EQ(snap.top_pages[0].page_vaddr, hot_page);
	EXPECT_EQ(snap.top_pages[0].fault_count, 50u);
	EXPECT_EQ(snap.top_pages[0].reprotect_count, 2u);
}

TEST_F(AuditProfilerTest, SimultaneousCadenceAndPartialBufferRefreshConfigCoexistence) {
	EXPECT_FALSE(Config::GpuSubmitCadenceEnabled());
	EXPECT_FALSE(Config::PartialBufferRefreshEnabled());

	Config::ConfigOptions options {};
	options.user_name = "TestUser";
	options.gpu_submit_cadence_enabled = true;
	options.partial_buffer_refresh_enabled = true;
	options.audit_faults_enabled = true;
	Config::Load(options);

	EXPECT_TRUE(Config::GpuSubmitCadenceEnabled());
	EXPECT_TRUE(Config::PartialBufferRefreshEnabled());
	EXPECT_TRUE(Config::AuditFaultsEnabled());

	SetEnabled(true);
	RecordSubmit(SubmitReason::Cadence);
	RecordSubmit(SubmitReason::ExplicitFlush);

	const auto snap = TakeSnapshot();
	EXPECT_TRUE(snap.cadence_enabled);
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Cadence)].count, 1u);
	EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::ExplicitFlush)].count, 1u);
}

TEST_F(AuditProfilerTest, ControlledCaptureSessionLifecycleAndReset) {
	SetEnabled(true);

	// Record pre-capture activity (e.g. during loading screen)
	RecordFault(0x1000'0000, 0x1234, 1000);
	RecordSubmit(SubmitReason::ExplicitFlush);
	RecordFrame(50.0);

	EXPECT_EQ(TakeSnapshot().total_faults, 1u);
	EXPECT_EQ(TakeSnapshot().total_frames, 1u);

	// Start controlled capture session: must reset all statistics so they correspond strictly to this interval
	StartCapture("TestSceneBenchmark");

	EXPECT_TRUE(IsCaptureRunning());
	EXPECT_EQ(GetCaptureState(), CaptureState::Running);
	EXPECT_EQ(TakeSnapshot().total_faults, 0u);
	EXPECT_EQ(TakeSnapshot().total_frames, 0u);

	// Now record frames and activity in test scene
	RecordFault(0x2000'0000, 0x5678, 2000);
	RecordFault(0x2000'0000, 0x5678, 2500);
	RecordSubmit(SubmitReason::Cadence);
	RecordFrame(33.3);
	RecordFrame(33.7);

	// Stop capture session
	StopCapture(CaptureState::Completed);

	EXPECT_FALSE(IsCaptureRunning());
	EXPECT_EQ(GetCaptureState(), CaptureState::Completed);

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_faults, 2u);
	EXPECT_EQ(snap.total_frames, 2u);
	EXPECT_EQ(snap.capture_state_str, "Completed");
	EXPECT_GT(snap.capture_id.size(), 0u);
	EXPECT_DOUBLE_EQ(snap.avg_frame_ms, 33.5);

	// Further events recorded after capture stop must be ignored
	RecordFault(0x3000'0000, 0x9999, 1000);
	RecordFrame(60.0);
	EXPECT_EQ(TakeSnapshot().total_faults, 2u);
	EXPECT_EQ(TakeSnapshot().total_frames, 2u);
}

TEST_F(AuditProfilerTest, HotkeyToggleCaptureStartsAndStops) {
	SetEnabled(true);
	StopCapture(CaptureState::Completed); // Ensure stopped

	EXPECT_FALSE(IsCaptureRunning());

	// First F9 press: starts capture
	ToggleCapture();
	EXPECT_TRUE(IsCaptureRunning());

	RecordFrame(16.6);
	RecordFault(0x4000'0000, 0x1111, 1500);
	EXPECT_EQ(TakeSnapshot().total_frames, 1u);

	// Second F9 press: stops capture and finalizes snapshot
	ToggleCapture();
	EXPECT_FALSE(IsCaptureRunning());
	EXPECT_EQ(GetCaptureState(), CaptureState::Completed);
	EXPECT_EQ(TakeSnapshot().total_frames, 1u);
}

TEST_F(AuditProfilerTest, ConfigurableCadenceIntervalReflectedInSnapshot) {
	Config::ConfigOptions options {};
	options.user_name = "TestUser";
	options.gpu_submit_cadence_enabled = true;
	options.gpu_submit_cadence_interval_us = 2000; // 2 ms
	Config::Load(options);

	EXPECT_EQ(Config::GetGpuSubmitCadenceIntervalUs(), 2000u);

	SetEnabled(true);
	StartCapture("CadenceIntervalTest");

	const auto snap = TakeSnapshot();
	EXPECT_TRUE(snap.cadence_enabled);
	EXPECT_EQ(snap.cadence_interval_us, 2000u);
}

TEST_F(AuditProfilerTest, RecordsStutteringHitchesAndSlowestFrames) {
	SetEnabled(true);
	StartCapture("StutterTest");

	RecordFrame(16.6);  // Normal
	RecordFrame(16.7);  // Normal
	RecordFrame(55.0);  // >= 50ms
	RecordFrame(120.0); // >= 100ms
	RecordFrame(300.0); // >= 250ms
	RecordFrame(600.0); // >= 500ms
	RecordFrame(1500.0);// >= 1000ms

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_frames, 7u);
	EXPECT_EQ(snap.hitches_ge_50ms, 5u);
	EXPECT_EQ(snap.hitches_ge_100ms, 4u);
	EXPECT_EQ(snap.hitches_ge_250ms, 3u);
	EXPECT_EQ(snap.hitches_ge_500ms, 2u);
	EXPECT_EQ(snap.hitches_ge_1000ms, 1u);
	EXPECT_NEAR(snap.hitch_time_total_ms, 55.0 + 120.0 + 300.0 + 600.0 + 1500.0, 0.1);

	// Slow frames array should contain frames >= 100ms sorted descending
	ASSERT_GE(snap.slow_frames.size(), 4u);
	EXPECT_DOUBLE_EQ(snap.slow_frames[0].frame_ms, 1500.0);
	EXPECT_DOUBLE_EQ(snap.slow_frames[1].frame_ms, 600.0);
	EXPECT_DOUBLE_EQ(snap.slow_frames[2].frame_ms, 300.0);
	EXPECT_DOUBLE_EQ(snap.slow_frames[3].frame_ms, 120.0);
}

TEST_F(AuditProfilerTest, ExtendedStreamBufferConfigReflectedInSnapshot) {
	Config::ConfigOptions options {};
	options.user_name = "TestUser";
	options.extended_stream_buffer_enabled = true;
	Config::Load(options);

	EXPECT_TRUE(Config::ExtendedStreamBufferEnabled());

	SetEnabled(true);
	StartCapture("ExtStreamBufTest");

	const auto snap = TakeSnapshot();
	EXPECT_TRUE(snap.extended_stream_buffer_enabled);
}

TEST_F(AuditProfilerTest, ManualTriggerModeWaitsForHotkeyAndDoesNotRecordBeforeF9) {
	InitCapture(0, 180, "test_profile.json", /*manual_trigger=*/true);

	EXPECT_TRUE(IsEnabled());
	EXPECT_EQ(GetCaptureState(), CaptureState::WaitingForHotkey);
	EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Warmup);
	EXPECT_EQ(GetTargetDurationSec(), 180u);

	// While waiting for hotkey, frame records should be ignored in the capture window
	RecordFrame(16.6);
	RecordFrame(16.7);
	auto snap1 = TakeSnapshot();
	EXPECT_EQ(snap1.total_frames, 0u);
	EXPECT_EQ(snap1.capture_state_str, "WaitingForHotkey");

	// User presses F9: ToggleCapture() triggers StartCapture with clean baseline
	ToggleCapture();
	EXPECT_EQ(GetCaptureState(), CaptureState::Running);
	EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Gameplay);

	RecordFrame(16.5);
	RecordFrame(16.4);
	auto snap2 = TakeSnapshot();
	EXPECT_EQ(snap2.total_frames, 2u);
	EXPECT_EQ(snap2.capture_state_str, "Running");
	EXPECT_EQ(snap2.capture_phase_str, "Gameplay");
	EXPECT_EQ(snap2.trigger_reason, "ManualHotkeyF9");
}

TEST_F(AuditProfilerTest, ExecutionPhaseTrackingAndSeparation) {
	SetEnabled(true);

	// Phase 1: Loading
	SetExecutionPhase(ExecutionPhase::Loading);
	EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Loading);
	EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::Loading), "Loading");

	RecordFault(0x1000, 0x2000, 500);
	RecordReprotect(0x1000, 4096);
	RecordGpuWait(GpuWaitReason::BufferReadback, 1000000);
	RecordSubmit(SubmitReason::ExplicitFlush);
	RecordFrame(33.3);

	// Phase 2: Gameplay
	SetExecutionPhase(ExecutionPhase::Gameplay);
	EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Gameplay);
	EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::Gameplay), "Gameplay");

	RecordFault(0x3000, 0x4000, 600);
	RecordReprotect(0x3000, 4096);
	RecordGpuWait(GpuWaitReason::BufferReadback, 500000);
	RecordSubmit(SubmitReason::Cadence);
	RecordFrame(16.6);

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.phase_loading.frame_count, 1u);
	EXPECT_EQ(snap.phase_loading.faults, 1u);
	EXPECT_EQ(snap.phase_loading.reprotects, 1u);
	EXPECT_EQ(snap.phase_loading.submits, 1u);
	EXPECT_EQ(snap.phase_loading.gpu_wait_ns, 1000000u);

	EXPECT_EQ(snap.phase_gameplay.frame_count, 1u);
	EXPECT_EQ(snap.phase_gameplay.faults, 1u);
	EXPECT_EQ(snap.phase_gameplay.reprotects, 1u);
	EXPECT_EQ(snap.phase_gameplay.submits, 1u);
	EXPECT_EQ(snap.phase_gameplay.gpu_wait_ns, 500000u);
}

TEST_F(AuditProfilerTest, PhaseMetricsDoNotLeakEventsOutsideActiveCapture) {
	// Initialize in manual trigger mode (Warmup)
	InitCapture(0, 180, "test_leak.json", true);
	EXPECT_EQ(GetCaptureState(), CaptureState::WaitingForHotkey);
	EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Warmup);

	// Events during Warmup must NOT record into capture or phase metrics
	RecordFault(0x1000, 0x2000, 500);
	RecordReprotect(0x1000, 4096);
	RecordGpuWait(GpuWaitReason::BufferReadback, 1000000);
	RecordSubmit(SubmitReason::Cadence);
	RecordFrame(50.0);

	auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_frames, 0u);
	EXPECT_EQ(snap.total_faults, 0u);
	EXPECT_EQ(snap.phase_gameplay.frame_count, 0u);
	EXPECT_EQ(snap.phase_gameplay.faults, 0u);

	// User presses F9 to begin capture
	StartCapture("ManualHotkeyF9");
	EXPECT_EQ(GetCaptureState(), CaptureState::Running);
	EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Gameplay);

	RecordFault(0x3000, 0x4000, 400);
	RecordFault(0x3000, 0x4000, 600);
	RecordReprotect(0x3000, 4096);
	RecordGpuWait(GpuWaitReason::BufferReadback, 800000);
	RecordSubmit(SubmitReason::Cadence);
	RecordFrame(20.0);
	RecordFrame(22.0);

	snap = TakeSnapshot();
	EXPECT_EQ(snap.total_frames, 2u);
	EXPECT_EQ(snap.total_faults, 2u);
	EXPECT_EQ(snap.phase_gameplay.frame_count, 2u);
	EXPECT_EQ(snap.phase_gameplay.faults, 2u);
	EXPECT_EQ(snap.total_frames, snap.phase_gameplay.frame_count);

	// Capture completes (e.g. 180s duration expired)
	StopCapture(CaptureState::Completed);
	EXPECT_EQ(GetCaptureState(), CaptureState::Completed);

	// Late events happening while game is still running before closing window
	for (int i = 0; i < 50; ++i) {
		RecordFault(0x9000 + i * 4096, 0x5000, 1000);
		RecordReprotect(0x9000 + i * 4096, 4096);
		RecordGpuWait(GpuWaitReason::BufferReadback, 200000);
		RecordSubmit(SubmitReason::ExplicitFlush);
		RecordFrame(60.0);
	}

	// Verify invariant: post-capture activity does NOT inflate gameplay metrics
	const auto final_snap = TakeSnapshot();
	EXPECT_EQ(final_snap.total_frames, 2u);
	EXPECT_EQ(final_snap.total_faults, 2u);
	EXPECT_EQ(final_snap.phase_gameplay.frame_count, 2u);
	EXPECT_EQ(final_snap.phase_gameplay.faults, 2u);
	EXPECT_EQ(final_snap.phase_gameplay.submits, 1u);
	EXPECT_EQ(final_snap.total_frames, final_snap.phase_gameplay.frame_count);
	EXPECT_DOUBLE_EQ(final_snap.phase_gameplay.total_frame_ms, 42.0);

	std::filesystem::remove("test_leak.json");
	std::filesystem::remove("test_leak.csv");
}

TEST_F(AuditProfilerTest, RecordsTimingAndSimulationMetricsAccurately) {
	SetEnabled(true);
	InitCapture(0, 180, "test_timing.json", true);
	EXPECT_EQ(GetCaptureState(), CaptureState::WaitingForHotkey);

	// In warmup, timing metrics must not record
	RecordVblank();
	RecordFlipSubmitted();
	RecordFlipCompleted();
	RecordFlipWait(10000000);
	RecordAudioUpdate(512, 48000);

	auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_vblanks, 0u);
	EXPECT_EQ(snap.total_flips_submitted, 0u);
	EXPECT_EQ(snap.total_flips_completed, 0u);
	EXPECT_EQ(snap.total_flip_wait_ns, 0u);
	EXPECT_EQ(snap.audio_updates_count, 0u);

	// Start capture (simulates user pressing F9 in gameplay scene)
	StartCapture("ManualHotkeyF9");
	EXPECT_EQ(GetCaptureState(), CaptureState::Running);

	// Record 60 VBlanks
	for (int i = 0; i < 60; ++i) {
		RecordVblank();
	}

	// Record 20 Flips (submitted & completed) at ~20 FPS pacing
	for (int i = 0; i < 20; ++i) {
		RecordFlipSubmitted();
		RecordFlipWait(25'000'000); // 25 ms wait per flip
		RecordFlipCompleted();
		RecordFrame(50.0);
	}

	// Record audio updates: ~93 batches of 512 samples = ~47616 samples (~1 second of audio at 48kHz)
	for (int i = 0; i < 93; ++i) {
		RecordAudioUpdate(512, 48000);
	}

	snap = TakeSnapshot();
	EXPECT_EQ(snap.total_vblanks, 60u);
	EXPECT_EQ(snap.total_flips_submitted, 20u);
	EXPECT_EQ(snap.total_flips_completed, 20u);
	EXPECT_EQ(snap.total_flip_wait_ns, 20u * 25'000'000u);
	EXPECT_DOUBLE_EQ(snap.total_flip_wait_ms, 500.0);
	EXPECT_DOUBLE_EQ(snap.avg_flip_wait_ms, 25.0);
	EXPECT_EQ(snap.audio_updates_count, 93u);
	EXPECT_EQ(snap.audio_total_samples, 93u * 512u);

	// Export to JSON and CSV
	EXPECT_TRUE(ExportAll("test_timing.json", "test_timing.csv", &snap));

	// Verify JSON contents
	{
		std::ifstream jf("test_timing.json");
		ASSERT_TRUE(jf.is_open());
		nlohmann::json j;
		jf >> j;
		ASSERT_TRUE(j.contains("timing_and_simulation"));
		EXPECT_EQ(j["timing_and_simulation"]["total_vblanks"], 60);
		EXPECT_EQ(j["timing_and_simulation"]["total_flips_submitted"], 20);
		EXPECT_EQ(j["timing_and_simulation"]["total_flips_completed"], 20);
		EXPECT_DOUBLE_EQ(j["timing_and_simulation"]["total_flip_wait_ms"], 500.0);
		EXPECT_DOUBLE_EQ(j["timing_and_simulation"]["avg_flip_wait_ms"], 25.0);
		EXPECT_EQ(j["timing_and_simulation"]["audio_updates_count"], 93);
		EXPECT_EQ(j["timing_and_simulation"]["audio_total_samples"], 93 * 512);
		ASSERT_TRUE(j.contains("graphics_workload"));
		EXPECT_TRUE(j["timing_and_simulation"].contains("estimated_simulation_speed_ratio_model_60hz"));
	}

	// Verify CSV contents
	{
		std::ifstream cf("test_timing.csv");
		ASSERT_TRUE(cf.is_open());
		std::string line;
		bool found_vblanks = false;
		bool found_flips = false;
		bool found_audio = false;
		bool found_sim_ratio = false;
		bool found_graphics_workload = false;
		while (std::getline(cf, line)) {
			if (line.find("Timing,TotalVblanks,60") != std::string::npos) {
				found_vblanks = true;
			}
			if (line.find("Timing,TotalFlipsCompleted,20") != std::string::npos) {
				found_flips = true;
			}
			if (line.find("Timing,AudioUpdatesCount,93") != std::string::npos) {
				found_audio = true;
			}
			if (line.find("Timing,SimulationSpeedRatio,") != std::string::npos) {
				found_sim_ratio = true;
			}
			if (line.find("GraphicsWorkload,TotalDraws,") != std::string::npos) {
				found_graphics_workload = true;
			}
		}
		EXPECT_TRUE(found_vblanks);
		EXPECT_TRUE(found_flips);
		EXPECT_TRUE(found_audio);
		EXPECT_TRUE(found_sim_ratio);
		EXPECT_TRUE(found_graphics_workload);
	}

	// Stop capture and verify late events do not leak
	StopCapture(CaptureState::Completed);

	for (int i = 0; i < 20; ++i) {
		RecordVblank();
		RecordFlipCompleted();
		RecordAudioUpdate(512, 48000);
	}
	const auto final_snap = TakeSnapshot();
	EXPECT_EQ(final_snap.total_vblanks, 60u);
	EXPECT_EQ(final_snap.total_flips_completed, 20u);
	EXPECT_EQ(final_snap.audio_updates_count, 93u);

	std::filesystem::remove("test_timing.json");
	std::filesystem::remove("test_timing.csv");
}

TEST_F(AuditProfilerTest, Stage7RenderOptimizationMetricsAndConfig) {
	// Verify defaults
	EXPECT_TRUE(Config::RedundantBindElisionEnabled());
	EXPECT_TRUE(Config::PipelineCacheMruEnabled());

	SetEnabled(true);
	Common::DebugCounters::SetEnabled(true);

	StartCapture("Stage7OptimizationTest");

	// Simulate draw calls, pipeline lookups and elisions
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Draws, 100);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::PipelineCacheLookups, 100);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::PipelineCacheHits, 85);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::RedundantPipelineBinds, 80);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::RedundantVertexBinds, 70);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::RedundantIndexBinds, 60);

	RecordFrame(16.6);

	const auto snap = TakeSnapshot();
	EXPECT_EQ(snap.total_draws, 100u);
	EXPECT_EQ(snap.pipeline_cache_lookups, 100u);
	EXPECT_EQ(snap.pipeline_cache_hits, 85u);
	EXPECT_DOUBLE_EQ(snap.pipeline_cache_hit_pct, 85.0);
	EXPECT_EQ(snap.redundant_pipeline_binds, 80u);
	EXPECT_EQ(snap.redundant_vertex_binds, 70u);
	EXPECT_EQ(snap.redundant_index_binds, 60u);
	EXPECT_TRUE(snap.redundant_bind_elision_enabled);
	EXPECT_TRUE(snap.pipeline_cache_mru_enabled);

	// Export to JSON and CSV
	EXPECT_TRUE(ExportAll("test_stage7.json", "test_stage7.csv", &snap));

	// Verify JSON export
	{
		std::ifstream jf("test_stage7.json");
		ASSERT_TRUE(jf.is_open());
		nlohmann::json j;
		jf >> j;
		ASSERT_TRUE(j.contains("graphics_workload"));
		EXPECT_EQ(j["graphics_workload"]["pipeline_cache_lookups"], 100);
		EXPECT_EQ(j["graphics_workload"]["pipeline_cache_hits"], 85);
		EXPECT_DOUBLE_EQ(j["graphics_workload"]["pipeline_cache_hit_pct"], 85.0);
		EXPECT_EQ(j["graphics_workload"]["redundant_pipeline_binds"], 80);
		EXPECT_EQ(j["graphics_workload"]["redundant_vertex_binds"], 70);
		EXPECT_EQ(j["graphics_workload"]["redundant_index_binds"], 60);

		ASSERT_TRUE(j["metadata"]["config"].contains("redundant_bind_elision"));
		EXPECT_TRUE(j["metadata"]["config"]["redundant_bind_elision"].get<bool>());
		ASSERT_TRUE(j["metadata"]["config"].contains("pipeline_cache_mru"));
		EXPECT_TRUE(j["metadata"]["config"]["pipeline_cache_mru"].get<bool>());
	}

	// Verify CSV export
	{
		std::ifstream cf("test_stage7.csv");
		ASSERT_TRUE(cf.is_open());
		std::string line;
		bool found_lookups = false;
		bool found_hits = false;
		bool found_red_pipe = false;
		bool found_red_vert = false;
		bool found_red_idx = false;
		while (std::getline(cf, line)) {
			if (line.find("GraphicsWorkload,PipelineCacheLookups,100") != std::string::npos) found_lookups = true;
			if (line.find("GraphicsWorkload,PipelineCacheHits,85") != std::string::npos) found_hits = true;
			if (line.find("GraphicsWorkload,RedundantPipelineBinds,80") != std::string::npos) found_red_pipe = true;
			if (line.find("GraphicsWorkload,RedundantVertexBinds,70") != std::string::npos) found_red_vert = true;
			if (line.find("GraphicsWorkload,RedundantIndexBinds,60") != std::string::npos) found_red_idx = true;
		}
		EXPECT_TRUE(found_lookups);
		EXPECT_TRUE(found_hits);
		EXPECT_TRUE(found_red_pipe);
		EXPECT_TRUE(found_red_vert);
		EXPECT_TRUE(found_red_idx);
	}

	StopCapture(CaptureState::Completed);

	std::filesystem::remove("test_stage7.json");
	std::filesystem::remove("test_stage7.csv");
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
