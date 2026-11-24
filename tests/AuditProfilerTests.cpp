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
  const auto &rb =
      snap.waits[static_cast<size_t>(GpuWaitReason::BufferReadback)];
  EXPECT_EQ(rb.count, 2u);
  EXPECT_EQ(rb.total_ns, 30000u);
  EXPECT_EQ(rb.max_ns, 20000u);

  const auto &tw =
      snap.waits[static_cast<size_t>(GpuWaitReason::TextureWriteBack)];
  EXPECT_EQ(tw.count, 1u);
  EXPECT_EQ(tw.total_ns, 5000u);
  EXPECT_EQ(tw.max_ns, 5000u);

  const auto &sw = snap.waits[static_cast<size_t>(GpuWaitReason::Swapchain)];
  EXPECT_EQ(sw.count, 1u);
  EXPECT_EQ(sw.total_ns, 30000u);

  const auto &gc =
      snap.waits[static_cast<size_t>(GpuWaitReason::BufferGarbageCollection)];
  EXPECT_EQ(gc.count, 1u);
  EXPECT_EQ(gc.total_ns, 8000u);

  // Other reasons should remain zero
  const auto &desc =
      snap.waits[static_cast<size_t>(GpuWaitReason::DescriptorHeap)];
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
  EXPECT_EQ(
      snap.submits[static_cast<size_t>(SubmitReason::ExplicitFlush)].count, 1u);
  EXPECT_EQ(
      snap.submits[static_cast<size_t>(SubmitReason::Synchronization)].count,
      1u);
  EXPECT_EQ(snap.submits[static_cast<size_t>(SubmitReason::Presentation)].count,
            1u);
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

  for (auto &th : threads) {
    th.join();
  }

  const auto snap = TakeSnapshot();
  EXPECT_EQ(snap.total_faults, static_cast<uint64_t>(NUM_THREADS * ITERATIONS));
  EXPECT_EQ(snap.total_reprotects,
            static_cast<uint64_t>(NUM_THREADS * ITERATIONS));
  EXPECT_EQ(
      snap.waits[static_cast<size_t>(GpuWaitReason::BufferReadback)].count,
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
  EXPECT_EQ(j["write_tracking_metrics"]["total_reprotects"].get<uint64_t>(),
            1u);

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

TEST_F(AuditProfilerTest,
       ColdReprotectsDoNotSaturateTableAndHotPagesAreRetained) {
  SetEnabled(true);

  // Simulate protecting a large 64MB buffer (16,384 pages) that was never
  // faulted
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

TEST_F(AuditProfilerTest,
       SimultaneousCadenceAndPartialBufferRefreshConfigCoexistence) {
  EXPECT_FALSE(Config::GpuSubmitCadenceEnabled());
  EXPECT_FALSE(Config::PartialBufferRefreshEnabled());

  Config::ConfigOptions options{};
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
  EXPECT_EQ(
      snap.submits[static_cast<size_t>(SubmitReason::ExplicitFlush)].count, 1u);
}

TEST_F(AuditProfilerTest, SubmitCadenceChangesAtOperationBoundary) {
  EXPECT_FALSE(Config::GpuSubmitCadenceEnabled());
  Config::RequestSubmitCadenceMode(true);
  EXPECT_FALSE(Config::GpuSubmitCadenceEnabled());
  EXPECT_EQ(Config::ApplySubmitCadenceMode(), std::optional<bool>(true));
  EXPECT_TRUE(Config::GpuSubmitCadenceEnabled());
  EXPECT_FALSE(Config::ApplySubmitCadenceMode().has_value());
  Config::RequestSubmitCadenceMode(false);
  EXPECT_TRUE(Config::GpuSubmitCadenceEnabled());
  EXPECT_EQ(Config::ApplySubmitCadenceMode(), std::optional<bool>(false));
  EXPECT_FALSE(Config::GpuSubmitCadenceEnabled());
  Config::ConfigOptions options{}; options.gpu_submit_cadence_enabled=true;
  Config::Load(options);
  EXPECT_TRUE(Config::GpuSubmitCadenceEnabled());
  EXPECT_FALSE(Config::ApplySubmitCadenceMode().has_value());
}

TEST_F(AuditProfilerTest, BindingReuseChangesAtOperationBoundary) {
  EXPECT_FALSE(Config::BindingReuseEnabled());
  Config::RequestBindingReuseMode(true);
  EXPECT_FALSE(Config::BindingReuseEnabled());
  Config::ApplyBindingReuseMode();
  EXPECT_TRUE(Config::BindingReuseEnabled());
  Config::RequestBindingReuseMode(false);
  EXPECT_TRUE(Config::BindingReuseEnabled());
  Config::ApplyBindingReuseMode();
  EXPECT_FALSE(Config::BindingReuseEnabled());
  Config::ConfigOptions options{};
  options.binding_reuse_enabled = true;
  Config::Load(options);
  EXPECT_TRUE(Config::BindingReuseEnabled());
}

TEST_F(AuditProfilerTest, TileLayoutModeAndCaptureDeltas) {
  EXPECT_FALSE(Config::TileLayoutCacheEnabled());
  Config::RequestTileLayoutCacheMode(true);
  EXPECT_FALSE(Config::TileLayoutCacheEnabled());
  Config::ApplyTileLayoutCacheMode();
  EXPECT_TRUE(Config::TileLayoutCacheEnabled());
  namespace Counters=Common::DebugCounters;
  Counters::SetEnabled(true);
  Counters::Add(Counters::Counter::TileLayoutHits,99);
  StartCapture("tile-layout");
  Counters::Add(Counters::Counter::TileLayoutHits,7);
  Counters::Add(Counters::Counter::TileLayoutMisses,3);
  StopCapture(CaptureState::Completed);
  const auto snap=TakeSnapshot();
  EXPECT_TRUE(snap.tile_layout_cache_enabled);
  EXPECT_EQ(snap.tile_layout_hits,7u); EXPECT_EQ(snap.tile_layout_misses,3u);
  ASSERT_TRUE(ExportJson("test_profile.json", &snap));
  std::ifstream input("test_profile.json"); nlohmann::json data; input>>data;
  EXPECT_TRUE(data["configuration"]["tile_layout_cache"].get<bool>());
  EXPECT_EQ(data["optimizations"]["tile_layout_hits"],7u);
  StartCapture("tile-layout-next");
  EXPECT_EQ(TakeSnapshot().tile_layout_hits,0u);
  EXPECT_EQ(TakeSnapshot().tile_layout_misses,0u);
  StopCapture(CaptureState::Completed);
  Config::ConfigOptions options{}; Config::Load(options);
  EXPECT_FALSE(Config::TileLayoutCacheEnabled());
}

TEST_F(AuditProfilerTest, BindingReuseCaptureDeltas) {
  EXPECT_FALSE(Config::BindingReuseEnabled());
  Config::ConfigOptions options{};
  options.binding_reuse_enabled = true;
  Config::Load(options);
  namespace Counters = Common::DebugCounters;
  Counters::SetEnabled(true);
  Counters::Add(Counters::Counter::BindingRangeReuses, 99);
  StartCapture("binding-reuse");
  Counters::Add(Counters::Counter::BindingRangeReuses, 3);
  Counters::Add(Counters::Counter::BindingLookupReuses, 2);
  Counters::Add(Counters::Counter::BindingSamplerReuses, 1);
  Counters::Add(Counters::Counter::BindingUploadReuses, 2);
  Counters::Add(Counters::Counter::BindingUploadReuseBytes, 4096);
  StopCapture(CaptureState::Completed);
  const auto snap = TakeSnapshot();
  EXPECT_TRUE(snap.binding_reuse_enabled);
  EXPECT_EQ(snap.binding_range_reuses, 3u);
  EXPECT_EQ(snap.binding_lookup_reuses, 2u);
  EXPECT_EQ(snap.binding_sampler_reuses, 1u);
  EXPECT_EQ(snap.binding_upload_reuses, 2u);
  EXPECT_EQ(snap.binding_upload_reuse_bytes, 4096u);
  ASSERT_TRUE(ExportJson("test_profile.json", &snap));
  std::ifstream input("test_profile.json");
  nlohmann::json data;
  input >> data;
  EXPECT_TRUE(data["configuration"]["binding_reuse"].get<bool>());
  EXPECT_EQ(data["optimizations"]["binding_range_reuses"], 3u);
  StartCapture("binding-reuse-next");
  EXPECT_EQ(TakeSnapshot().binding_range_reuses, 0u);
  EXPECT_EQ(TakeSnapshot().binding_lookup_reuses, 0u);
  EXPECT_EQ(TakeSnapshot().binding_sampler_reuses, 0u);
  EXPECT_EQ(TakeSnapshot().binding_upload_reuses, 0u);
  EXPECT_EQ(TakeSnapshot().binding_upload_reuse_bytes, 0u);
}

TEST_F(AuditProfilerTest, TransferPrefetchFlagsAndCaptureDeltas) {
  EXPECT_FALSE(Config::ReadbackNeighboursEnabled());
  EXPECT_FALSE(Config::WideReadbackWindowEnabled());
  EXPECT_FALSE(Config::FaultAheadEnabled());
  Config::ConfigOptions options{};
  options.readback_neighbours = true;
  options.wide_readback_window = true;
  options.fault_ahead = true;
  Config::Load(options);
  namespace Counters = Common::DebugCounters;
  Counters::SetEnabled(true);
  Counters::Add(Counters::Counter::BufferNeighbourDownloads, 7);
  Counters::Add(Counters::Counter::BufferNeighbourDownloadBytes, 4096);
  StartCapture("transfer-prefetch");
  Counters::Add(Counters::Counter::BufferNeighbourDownloads, 2);
  Counters::Add(Counters::Counter::BufferNeighbourDownloadBytes, 1024);
  StopCapture(CaptureState::Completed);
  const auto snap = TakeSnapshot();
  EXPECT_TRUE(snap.readback_neighbours_enabled);
  EXPECT_TRUE(snap.wide_readback_window_enabled);
  EXPECT_TRUE(snap.fault_ahead_enabled);
  EXPECT_EQ(snap.neighbour_readback_count, 2u);
  EXPECT_EQ(snap.neighbour_readback_bytes, 1024u);
  ASSERT_TRUE(ExportJson("test_profile.json", &snap));
  std::ifstream input("test_profile.json");
  nlohmann::json data;
  input >> data;
  EXPECT_TRUE(data["configuration"]["readback_neighbours"].get<bool>());
  EXPECT_TRUE(data["configuration"]["wide_readback_window"].get<bool>());
  EXPECT_TRUE(data["configuration"]["fault_ahead"].get<bool>());
  EXPECT_EQ(data["memory_transfers"]["neighbour_readback_count"], 2u);
  EXPECT_EQ(data["memory_transfers"]["neighbour_readback_bytes"], 1024u);
  StartCapture("transfer-prefetch-second");
  EXPECT_EQ(TakeSnapshot().neighbour_readback_count, 0u);
  EXPECT_EQ(TakeSnapshot().neighbour_readback_bytes, 0u);
  StopCapture(CaptureState::Completed);
}

TEST_F(AuditProfilerTest, ControlledCaptureSessionLifecycleAndReset) {
  SetEnabled(true);

  // Record pre-capture activity (e.g. during loading screen)
  RecordFault(0x1000'0000, 0x1234, 1000);
  RecordSubmit(SubmitReason::ExplicitFlush);
  RecordFrame(50.0);

  EXPECT_EQ(TakeSnapshot().total_faults, 1u);
  EXPECT_EQ(TakeSnapshot().total_frames, 1u);

  // Start controlled capture session: must reset all statistics so they
  // correspond strictly to this interval
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
  Config::ConfigOptions options{};
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

  RecordFrame(16.6);   // Normal
  RecordFrame(16.7);   // Normal
  RecordFrame(55.0);   // >= 50ms
  RecordFrame(120.0);  // >= 100ms
  RecordFrame(300.0);  // >= 250ms
  RecordFrame(600.0);  // >= 500ms
  RecordFrame(1500.0); // >= 1000ms

  const auto snap = TakeSnapshot();
  EXPECT_EQ(snap.total_frames, 7u);
  EXPECT_EQ(snap.hitches_ge_50ms, 5u);
  EXPECT_EQ(snap.hitches_ge_100ms, 4u);
  EXPECT_EQ(snap.hitches_ge_250ms, 3u);
  EXPECT_EQ(snap.hitches_ge_500ms, 2u);
  EXPECT_EQ(snap.hitches_ge_1000ms, 1u);
  EXPECT_NEAR(snap.hitch_time_total_ms, 55.0 + 120.0 + 300.0 + 600.0 + 1500.0,
              0.1);

  // Slow frames array should contain frames >= 100ms sorted descending
  ASSERT_GE(snap.slow_frames.size(), 4u);
  EXPECT_DOUBLE_EQ(snap.slow_frames[0].frame_ms, 1500.0);
  EXPECT_DOUBLE_EQ(snap.slow_frames[1].frame_ms, 600.0);
  EXPECT_DOUBLE_EQ(snap.slow_frames[2].frame_ms, 300.0);
  EXPECT_DOUBLE_EQ(snap.slow_frames[3].frame_ms, 120.0);
}

TEST_F(AuditProfilerTest, ExtendedStreamBufferConfigReflectedInSnapshot) {
  Config::ConfigOptions options{};
  options.user_name = "TestUser";
  options.extended_stream_buffer_enabled = true;
  Config::Load(options);

  EXPECT_TRUE(Config::ExtendedStreamBufferEnabled());

  SetEnabled(true);
  StartCapture("ExtStreamBufTest");

  const auto snap = TakeSnapshot();
  EXPECT_TRUE(snap.extended_stream_buffer_enabled);
}

TEST_F(AuditProfilerTest,
       ManualTriggerModeWaitsForHotkeyAndDoesNotRecordBeforeF9) {
  InitCapture(0, 180, "test_profile.json", /*manual_trigger=*/true);

  EXPECT_TRUE(IsEnabled());
  EXPECT_EQ(GetCaptureState(), CaptureState::WaitingForHotkey);
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Warmup);
  EXPECT_EQ(GetTargetDurationSec(), 180u);

  // While waiting for hotkey, frame records should be ignored in the capture
  // window
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

  // Record audio updates: ~93 batches of 512 samples = ~47616 samples (~1
  // second of audio at 48kHz)
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
    EXPECT_TRUE(j["timing_and_simulation"].contains(
        "estimated_simulation_speed_ratio_model_60hz"));
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
  Common::DebugCounters::Add(
      Common::DebugCounters::Counter::PipelineCacheLookups, 100);
  Common::DebugCounters::Add(Common::DebugCounters::Counter::PipelineCacheHits,
                             85);
  Common::DebugCounters::Add(
      Common::DebugCounters::Counter::RedundantPipelineBinds, 80);
  Common::DebugCounters::Add(
      Common::DebugCounters::Counter::RedundantVertexBinds, 70);
  Common::DebugCounters::Add(
      Common::DebugCounters::Counter::RedundantIndexBinds, 60);

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
      if (line.find("GraphicsWorkload,PipelineCacheLookups,100") !=
          std::string::npos)
        found_lookups = true;
      if (line.find("GraphicsWorkload,PipelineCacheHits,85") !=
          std::string::npos)
        found_hits = true;
      if (line.find("GraphicsWorkload,RedundantPipelineBinds,80") !=
          std::string::npos)
        found_red_pipe = true;
      if (line.find("GraphicsWorkload,RedundantVertexBinds,70") !=
          std::string::npos)
        found_red_vert = true;
      if (line.find("GraphicsWorkload,RedundantIndexBinds,60") !=
          std::string::npos)
        found_red_idx = true;
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

TEST_F(AuditProfilerTest, RecordsPipelineStagesAccurately) {
  StartCapture("UnitTestStage8Pipeline");
  EXPECT_TRUE(IsCaptureRunning());

  RecordPipelineStage(PipelineStage::Pm4Decode, 1000);
  RecordPipelineStage(PipelineStage::Pm4Decode, 2000);
  RecordPipelineStage(PipelineStage::Pm4Dispatch, 5000);
  RecordPipelineStage(PipelineStage::RenderStatePrep, 3000);
  RecordPipelineStage(PipelineStage::PipelineKeyBuild, 1500);
  RecordPipelineStage(PipelineStage::PipelineLookup, 500);
  RecordPipelineStage(PipelineStage::DescriptorPrep, 4000);
  RecordPipelineStage(PipelineStage::BarriersTransitions, 2500);
  RecordPipelineStage(PipelineStage::BufferPrep, 3500);
  RecordPipelineStage(PipelineStage::CommandRecording, 6000);
  RecordPipelineStage(PipelineStage::CommandEnd, 800);
  RecordPipelineStage(PipelineStage::Submit, 7000);
  RecordPipelineStage(PipelineStage::SyncWaits, 12000);
  RecordPipelineStage(PipelineStage::Present, 8000);
  RecordPipelineStage(PipelineStage::DrawPrep, 4500);
  RecordPipelineStage(PipelineStage::SequencerWait, 2500);
  RecordPipelineStage(PipelineStage::DrawPrepWorker, 3500);
  RecordPipelineStage(PipelineStage::DrawPrepCommitWait, 1500);

  RecordFrame(16.6);

  const auto snap = TakeSnapshot();
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Decode)].count,
      2u);
  EXPECT_EQ(snap.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Decode)]
                .total_ns,
            3000u);
  EXPECT_DOUBLE_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Decode)]
          .avg_us,
      1.5);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Dispatch)]
          .count,
      1u);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::Pm4Dispatch)]
          .total_ns,
      5000u);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::SyncWaits)].count,
      1u);
  EXPECT_EQ(snap.pipeline_stages[static_cast<size_t>(PipelineStage::SyncWaits)]
                .total_ns,
            12000u);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::DrawPrep)].count,
      1u);
  EXPECT_EQ(snap.pipeline_stages[static_cast<size_t>(PipelineStage::DrawPrep)]
                .total_ns,
            4500u);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::SequencerWait)]
          .count,
      1u);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::SequencerWait)]
          .total_ns,
      2500u);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::DrawPrepWorker)]
          .count,
      1u);
  EXPECT_EQ(
      snap.pipeline_stages[static_cast<size_t>(PipelineStage::DrawPrepWorker)]
          .total_ns,
      3500u);
  EXPECT_EQ(snap.pipeline_stages[static_cast<size_t>(
                                     PipelineStage::DrawPrepCommitWait)]
                .count,
            1u);
  EXPECT_EQ(snap.pipeline_stages[static_cast<size_t>(
                                     PipelineStage::DrawPrepCommitWait)]
                .total_ns,
            1500u);

  EXPECT_STREQ(GetPipelineStageName(PipelineStage::Pm4Decode), "Pm4Decode");
  EXPECT_STREQ(GetPipelineStageName(PipelineStage::Submit), "Submit");
  EXPECT_STREQ(GetPipelineStageName(PipelineStage::Present), "Present");
  EXPECT_STREQ(GetPipelineStageName(PipelineStage::DrawPrep), "DrawPrep");
  EXPECT_STREQ(GetPipelineStageName(PipelineStage::SequencerWait),
               "SequencerWait");
  EXPECT_STREQ(GetPipelineStageName(PipelineStage::DrawPrepWorker),
               "DrawPrepWorker");
  EXPECT_STREQ(GetPipelineStageName(PipelineStage::DrawPrepCommitWait),
               "DrawPrepCommitWait");

  StopCapture(CaptureState::Completed);
}

TEST_F(AuditProfilerTest, RecordsStreamBufferAuditAndWaits) {
  StartCapture("UnitTestStage8StreamBuffer");
  EXPECT_TRUE(IsCaptureRunning());

  RecordStreamBufferAlloc(1024 * 1024, false, 1024 * 1024);
  RecordStreamBufferAlloc(2 * 1024 * 1024, false, 3 * 1024 * 1024);
  RecordStreamBufferAlloc(1024 * 1024, true, 4 * 1024 * 1024);

  RecordStreamBufferWaitEvent(250'000'000, 42);

  RecordFrame(20.0);

  const auto snap = TakeSnapshot();
  EXPECT_EQ(snap.stream_buffer_stats.total_allocs, 3u);
  EXPECT_EQ(snap.stream_buffer_stats.total_wraps, 1u);
  EXPECT_EQ(snap.stream_buffer_stats.total_bytes, 4 * 1024 * 1024u);
  EXPECT_DOUBLE_EQ(snap.stream_buffer_stats.total_mb, 4.0);
  EXPECT_DOUBLE_EQ(snap.stream_buffer_stats.high_water_mb, 4.0);
  EXPECT_EQ(snap.stream_buffer_stats.wait_events, 1u);
  EXPECT_DOUBLE_EQ(snap.stream_buffer_stats.wait_total_ms, 250.0);
  EXPECT_DOUBLE_EQ(snap.stream_buffer_stats.wait_max_ms, 250.0);
  EXPECT_EQ(snap.stream_buffer_stats.last_wait_tick, 42u);

  StopCapture(CaptureState::Completed);
}

TEST_F(AuditProfilerTest, CorrelatesHitchesWithTraceEventRing) {
  StartCapture("UnitTestStage8HitchCorrelation");
  EXPECT_TRUE(IsCaptureRunning());

  RecordTraceEvent(TraceEventType::StreamBufferWait, 450'000'000, 100, 0x1000);
  RecordFrame(500.0);

  const auto snap = TakeSnapshot();
  ASSERT_GE(snap.detailed_hitches.size(), 1u);
  EXPECT_DOUBLE_EQ(snap.detailed_hitches[0].frame_ms, 500.0);
  EXPECT_STREQ(snap.detailed_hitches[0].dominant_blocker.c_str(),
               "StreamBufferWait");
  EXPECT_DOUBLE_EQ(snap.detailed_hitches[0].blocker_duration_ms, 450.0);
  EXPECT_EQ(snap.detailed_hitches[0].blocker_tick, 100u);
  EXPECT_EQ(snap.detailed_hitches[0].blocker_address, 0x1000u);
  EXPECT_STREQ(snap.detailed_hitches[0].correlation_confidence.c_str(), "High");

  StopCapture(CaptureState::Completed);
}

TEST_F(AuditProfilerTest, ExportsStage8JsonAndCsvComplete) {
  StartCapture("UnitTestStage8Export");
  EXPECT_TRUE(IsCaptureRunning());

  RecordPipelineStage(PipelineStage::PipelineKeyBuild, 50000);
  RecordStreamBufferAlloc(8 * 1024 * 1024, true, 8 * 1024 * 1024);
  RecordStreamBufferWaitEvent(120'000'000, 999);
  RecordTraceEvent(TraceEventType::TimelineWait, 120'000'000, 999, 0);
  RecordGpuExecutionTime(15'000'000);
  RecordFrame(150.0);

  const auto snap = TakeSnapshot();
  EXPECT_TRUE(ExportAll("test_stage8.json", "test_stage8.csv", &snap));

  // Verify JSON export contains all Stage 8 sections
  {
    std::ifstream jf("test_stage8.json");
    ASSERT_TRUE(jf.is_open());
    nlohmann::json j;
    jf >> j;

    ASSERT_TRUE(j.contains("pipeline_stage_timings"));
    EXPECT_TRUE(j["pipeline_stage_timings"].contains("PipelineKeyBuild"));

    ASSERT_TRUE(j.contains("cpu_gpu_separation"));
    EXPECT_TRUE(j["cpu_gpu_separation"].contains("gpu_thread_wall_clock_ms"));
    EXPECT_TRUE(j["cpu_gpu_separation"].contains("gpu_execution_time_ms"));

    ASSERT_TRUE(j.contains("stream_buffer_audit"));
    EXPECT_EQ(j["stream_buffer_audit"]["total_allocations"], 1);
    EXPECT_EQ(j["stream_buffer_audit"]["total_wraparounds"], 1);

    ASSERT_TRUE(j.contains("stuttering_analysis"));
    ASSERT_TRUE(j["stuttering_analysis"].contains("detailed_hitches"));
    EXPECT_FALSE(j["stuttering_analysis"]["detailed_hitches"].empty());

    ASSERT_TRUE(j["metadata"]["config"].contains("pipeline_key_cache"));
    ASSERT_TRUE(j["metadata"]["config"].contains("draw_state_dirty_check"));
  }

  // Verify CSV export contains Stage 8 categories
  {
    std::ifstream cf("test_stage8.csv");
    ASSERT_TRUE(cf.is_open());
    std::string line;
    bool found_pipeline_stage = false;
    bool found_stream_buffer = false;
    bool found_cpu_gpu = false;
    bool found_hitch = false;
    while (std::getline(cf, line)) {
      if (line.find("PipelineStageCount,PipelineKeyBuild,1") !=
          std::string::npos)
        found_pipeline_stage = true;
      if (line.find("StreamBuffer,TotalAllocations,1") != std::string::npos)
        found_stream_buffer = true;
      if (line.find("CpuGpuSeparation,GpuExecutionTimeMs") != std::string::npos)
        found_cpu_gpu = true;
      if (line.find("Hitch,") != std::string::npos)
        found_hitch = true;
    }
    EXPECT_TRUE(found_pipeline_stage);
    EXPECT_TRUE(found_stream_buffer);
    EXPECT_TRUE(found_cpu_gpu);
    EXPECT_TRUE(found_hitch);
  }

  StopCapture(CaptureState::Completed);

  std::filesystem::remove("test_stage8.json");
  std::filesystem::remove("test_stage8.csv");
}

TEST_F(AuditProfilerTest, ExecutionPhaseLandingTransitionsAndAdvancePhase) {
  SetExecutionPhase(ExecutionPhase::Gameplay);
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Gameplay);

  AdvancePhase();
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::WorldEntryPrep);
  EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::WorldEntryPrep),
               "WorldEntryPrep");

  AdvancePhase();
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::DescentStart);
  EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::DescentStart),
               "DescentStart");

  AdvancePhase();
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::ActiveDescent);
  EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::ActiveDescent),
               "ActiveDescent");

  AdvancePhase();
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::GroundApproach);
  EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::GroundApproach),
               "GroundApproach");

  AdvancePhase();
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Touchdown);
  EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::Touchdown), "Touchdown");

  AdvancePhase();
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::EarlyGameplay);
  EXPECT_STREQ(GetExecutionPhaseName(ExecutionPhase::EarlyGameplay),
               "EarlyGameplay");

  AdvancePhase();
  EXPECT_EQ(GetExecutionPhase(), ExecutionPhase::Gameplay);
}

TEST_F(AuditProfilerTest, Stage8_1CpuGpuSeparationAndExclusiveDispatch) {
  StartCapture("UnitTestStage8_1Exclusive");
  EXPECT_TRUE(IsCaptureRunning());

  // Record Pm4Dispatch = 50ms (inclusive)
  RecordPipelineStage(PipelineStage::Pm4Dispatch, 50'000'000);
  // Inner draw stages = 15ms + 10ms + 5ms + 5ms + 5ms = 40ms
  RecordPipelineStage(PipelineStage::RenderStatePrep, 15'000'000);
  RecordPipelineStage(PipelineStage::DescriptorPrep, 10'000'000);
  RecordPipelineStage(PipelineStage::BarriersTransitions, 5'000'000);
  RecordPipelineStage(PipelineStage::BufferPrep, 5'000'000);
  RecordPipelineStage(PipelineStage::CommandRecording, 5'000'000);

  RecordFrame(50.0);

  const auto snap = TakeSnapshot();
  EXPECT_DOUBLE_EQ(snap.pm4_dispatch_inclusive_ms, 50.0);
  EXPECT_DOUBLE_EQ(snap.pm4_dispatch_exclusive_ms, 10.0);
  EXPECT_DOUBLE_EQ(snap.pm4_dispatch_exclusive_ms_per_frame, 10.0);
  EXPECT_GT(snap.gpu_thread_wall_clock_ms, 0.0);
  EXPECT_FALSE(snap.git_commit.empty());

  StopCapture(CaptureState::Completed);
}

TEST_F(AuditProfilerTest, Stage8_1FrameTimeSeriesExport) {
  StartCapture("UnitTestStage8_1TimeSeries");
  EXPECT_TRUE(IsCaptureRunning());

  SetExecutionPhase(ExecutionPhase::ActiveDescent);
  RecordPipelineStage(PipelineStage::Pm4Dispatch, 20'000'000);
  RecordPipelineStage(PipelineStage::RenderStatePrep, 8'000'000);
  RecordFrame(25.0);

  const auto snap = TakeSnapshot();
  EXPECT_TRUE(ExportAll("test_timeseries.json", "test_timeseries.csv", &snap));

  // Verify frames CSV was created and contains the expected header and row
  {
    std::ifstream ff("test_timeseries_frames.csv");
    ASSERT_TRUE(ff.is_open());
    std::string header;
    ASSERT_TRUE(std::getline(ff, header));
    EXPECT_NE(header.find("frame_index,frame_ms,fps,phase,draws"),
              std::string::npos);
    EXPECT_NE(header.find("pm4_dispatch_us"), std::string::npos);

    std::string row;
    ASSERT_TRUE(std::getline(ff, row));
    EXPECT_NE(row.find("ActiveDescent"), std::string::npos);
  }

  StopCapture(CaptureState::Completed);

  std::filesystem::remove("test_timeseries.json");
  std::filesystem::remove("test_timeseries.csv");
  std::filesystem::remove("test_timeseries_frames.csv");
}

TEST_F(AuditProfilerTest, Stage8_1OptimizationFlagsConfig) {
  EXPECT_TRUE(Config::EnsurePipelineCacheEnabled());
  EXPECT_TRUE(Config::DescriptorCacheEnabled());
  EXPECT_FALSE(Config::DrawStateDirtyCheckEnabled());

  StartCapture("UnitTestStage8_1Config");
  const auto snap = TakeSnapshot();
  EXPECT_TRUE(snap.ensure_pipeline_cache_enabled);
  EXPECT_TRUE(snap.descriptor_cache_enabled);
  EXPECT_FALSE(snap.draw_state_dirty_check_enabled);

  StopCapture(CaptureState::Completed);
}

TEST_F(AuditProfilerTest, Stage8_2DrawStateDirtyCheckRegressionProtection) {
  // Verify that DrawStateDirtyCheck is disabled by default to prevent visual
  // regressions demonstrated in Astro Bot Sky Garden (phantom shadows, intense
  // flashing, binding desync)
  EXPECT_FALSE(Config::DrawStateDirtyCheckEnabled());

  const auto snap = TakeSnapshot();
  EXPECT_FALSE(snap.draw_state_dirty_check_enabled);
}

TEST_F(AuditProfilerTest, Stage8_4SynchronizationFlags) {
  // Verify Stage 8.4 low-latency synchronization and spin wait defaults
  EXPECT_TRUE(Config::FastPendingOpsEnabled());
  EXPECT_TRUE(Config::PrioritySpinWaitEnabled());
  EXPECT_EQ(Config::PrioritySpinDurationUs(), 100u);

  StartCapture("UnitTestStage8_4Sync");
  CountPriorityWaitSpin();
  CountPriorityWaitSpin();
  CountPriorityWaitSpinHit();
  CountPendingOpsFastSkip();

  const auto snap = TakeSnapshot();
  EXPECT_TRUE(snap.fast_pending_ops_enabled);
  EXPECT_TRUE(snap.priority_spin_wait_enabled);
  EXPECT_EQ(snap.priority_spin_duration_us, 100u);
  EXPECT_EQ(snap.priority_wait_spins, 2u);
  EXPECT_EQ(snap.priority_wait_spin_hits, 1u);
  EXPECT_EQ(snap.pending_ops_fast_skips, 1u);

  StopCapture(CaptureState::Completed);
}

TEST_F(AuditProfilerTest, ResourcePrepModeAndCounterBaselines) {
  namespace Counters = Common::DebugCounters;
  Config::ConfigOptions cfg;
  Config::Load(cfg);
  EXPECT_FALSE(Config::ResourcePrepReuseEnabled());
  Config::RequestResourcePrepReuseMode(true);
  EXPECT_FALSE(Config::ResourcePrepReuseEnabled());
  Config::ApplyResourcePrepReuseMode();
  EXPECT_TRUE(Config::ResourcePrepReuseEnabled());
  Counters::SetEnabled(true);
  Counters::Add(Counters::Counter::ResourcePrepHits,7);
  StartCapture("resource-prep");
  Counters::Add(Counters::Counter::ResourcePrepHits,3);
  Counters::Add(Counters::Counter::ResourcePrepMisses,2);
  const auto snap=TakeSnapshot();
  EXPECT_TRUE(snap.resource_prep_reuse_enabled);
  EXPECT_EQ(snap.resource_prep_hits,3u);
  EXPECT_EQ(snap.resource_prep_misses,2u);
  StopCapture();
  Config::Load(cfg);
  EXPECT_FALSE(Config::ResourcePrepReuseEnabled());
}

TEST_F(AuditProfilerTest, ReadBufferModeAndCounterBaselines) {
  namespace Counters = Common::DebugCounters;
  Config::ConfigOptions cfg;
  Config::Load(cfg);
  EXPECT_FALSE(Config::ReadBufferReuseEnabled());
  Config::RequestReadBufferReuseMode(true);
  EXPECT_FALSE(Config::ReadBufferReuseEnabled());
  Config::ApplyReadBufferReuseMode();
  EXPECT_TRUE(Config::ReadBufferReuseEnabled());
  Counters::SetEnabled(true);
  Counters::Add(Counters::Counter::ReadBufferHits,7);
  StartCapture("resource-prep");
  Counters::Add(Counters::Counter::ReadBufferHits,3);
  Counters::Add(Counters::Counter::ReadBufferMisses,2);
  const auto snap=TakeSnapshot();
  EXPECT_TRUE(snap.read_buffer_reuse_enabled);
  EXPECT_EQ(snap.read_buffer_hits,3u);
  EXPECT_EQ(snap.read_buffer_misses,2u);
  StopCapture();
  Config::Load(cfg);
  EXPECT_FALSE(Config::ReadBufferReuseEnabled());
}

TEST_F(AuditProfilerTest, CompactReadbackModeAndCounterBaselines) {
  namespace Counters = Common::DebugCounters;
  Config::ConfigOptions cfg;
  Config::Load(cfg);
  EXPECT_FALSE(Config::CompactImageReadbackEnabled());
  Config::RequestCompactImageReadbackMode(true);
  EXPECT_FALSE(Config::CompactImageReadbackEnabled());
  Config::ApplyCompactImageReadbackMode();
  EXPECT_TRUE(Config::CompactImageReadbackEnabled());
  Counters::SetEnabled(true);
  Counters::Add(Counters::Counter::CompactReadbackCopies,7);
  StartCapture("resource-prep");
  Counters::Add(Counters::Counter::CompactReadbackCopies,3);
  Counters::Add(Counters::Counter::CompactReadbackFallbacks,2);
  const auto snap=TakeSnapshot();
  EXPECT_TRUE(snap.compact_image_readback_enabled);
  EXPECT_EQ(snap.compact_readback_copies,3u);
  EXPECT_EQ(snap.compact_readback_fallbacks,2u);
  StopCapture();
  Config::Load(cfg);
  EXPECT_FALSE(Config::CompactImageReadbackEnabled());
}

TEST_F(AuditProfilerTest, LiveJournalSurvivesCaptureResetAndFlushesOnStop) {
  const auto path = std::filesystem::temp_directory_path() / "kyty_test_live_journal.jsonl";
  ASSERT_TRUE(StartLiveDiagnostics(path.string()));
  RecordFrame(100.0);
  RecordFlipSubmitted();
  RecordFlipCompleted();
  RecordVblank();
  RecordAudioUpdate(480, 48000);
  RecordGpuWait(GpuWaitReason::BufferReadback, 2'000'000);
  RecordPipelineStage(PipelineStage::DescriptorPrep, 3'000'000);
  Reset(); // Ordinary capture resets must not erase the independent journal.
  RecordFrame(20.0);
  StopLiveDiagnostics();
  std::ifstream input(path);
  std::string line;
  uint64_t frames = 0, flips = 0, hitches = 0;
  double audio = 0, wait = 0, stage = 0;
  bool metadata = false, final = false;
  while (std::getline(input, line)) {
    const auto row = nlohmann::json::parse(line);
    EXPECT_FALSE(row["simulation_speed_measured"].get<bool>());
    if (row["type"] == "metadata") { metadata = true; continue; }
    frames += row["frame_intervals"].get<uint64_t>();
    flips += row["completed_flip_hz"].get<double>() * row["interval_seconds"].get<double>() + 0.5;
    hitches += row["hitches_100ms"].get<uint64_t>();
    audio += row["audio_media_seconds"].get<double>();
    wait += row["thread_accumulated_wait_ms"]["BufferReadback"].get<double>();
    stage += row["host_stage_ms"]["DescriptorPrep"].get<double>();
    if (row["type"] == "final") final = true;
  }
  EXPECT_TRUE(metadata);
  EXPECT_TRUE(final);
  EXPECT_EQ(frames, 2u);
  EXPECT_EQ(flips, 1u);
  EXPECT_EQ(hitches, 1u);
  EXPECT_NEAR(audio, 0.01, 0.000001);
  EXPECT_DOUBLE_EQ(wait, 2.0);
  EXPECT_DOUBLE_EQ(stage, 3.0);
  input.close();
  std::filesystem::remove(path);
}

TEST_F(AuditProfilerTest, LiveJournalRecordsAnIntervalWithoutFrames) {
  const auto path = std::filesystem::temp_directory_path() / "kyty_test_live_idle.jsonl";
  ASSERT_TRUE(StartLiveDiagnostics(path.string()));
  StopLiveDiagnostics();
  std::ifstream input(path);
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
  ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
  const auto row = nlohmann::json::parse(line);
  EXPECT_EQ(row["type"], "final");
  EXPECT_EQ(row["frame_intervals"], 0);
  EXPECT_TRUE(row["no_frame_seen"]);
  EXPECT_TRUE(row["average_frame_ms"].is_null());
  EXPECT_GE(row["seconds_without_frame"].get<double>(), 0.0);
  input.close();
  std::filesystem::remove(path);
}

TEST_F(AuditProfilerTest, ScratchMemoryCounterBaselines) {
  namespace Counters = Common::DebugCounters;
  Config::ConfigOptions cfg;
  cfg.trim_scratch_memory_enabled = true;
  Config::Load(cfg);
  Counters::SetEnabled(true);
  Counters::Add(Counters::Counter::ScratchFreedBytes, 100);
  StartCapture("scratch-memory");
  Counters::Add(Counters::Counter::ScratchFreedBytes, 64);
  Counters::Add(Counters::Counter::ScratchRetirements, 2);
  Counters::Add(Counters::Counter::ScratchAllocations, 1);
  const auto snap = TakeSnapshot();
  EXPECT_TRUE(snap.trim_scratch_memory_enabled);
  EXPECT_EQ(snap.scratch_freed_bytes, 64u);
  EXPECT_EQ(snap.scratch_retirements, 2u);
  EXPECT_EQ(snap.scratch_allocations, 1u);
  StopCapture();
  Config::Load({});
  EXPECT_FALSE(Config::TrimScratchMemoryEnabled());
}

TEST_F(AuditProfilerTest, WorkloadTimeDistributionGpuBound) {
  SetEnabled(true);
  StartCapture("tripartite-gpu-bound");

  // Queue: 2 ms
  RecordQueueLatency(QueueLatencyType::Submission, 1'000'000);
  RecordQueueLatency(QueueLatencyType::OpStream, 1'000'000);

  // CPU Active: 8 ms
  RecordPipelineStage(PipelineStage::Pm4Decode, 3'000'000);
  RecordPipelineStage(PipelineStage::CommandRecording, 5'000'000);

  // GPU Wait: 30 ms (75% -> GpuBound)
  RecordGpuWait(GpuWaitReason::StreamBufferWait, 20'000'000);
  RecordGpuWait(GpuWaitReason::Swapchain, 10'000'000);

  const auto snap = TakeSnapshot();
  const auto &td  = snap.time_distribution;

  EXPECT_DOUBLE_EQ(td.queue_submission_ms, 1.0);
  EXPECT_DOUBLE_EQ(td.queue_opstream_ms, 1.0);
  EXPECT_DOUBLE_EQ(td.total_queue_ms, 2.0);
  EXPECT_DOUBLE_EQ(td.total_cpu_active_ms, 8.0);
  EXPECT_DOUBLE_EQ(td.total_gpu_wait_ms, 30.0);
  EXPECT_DOUBLE_EQ(td.total_workload_ms, 40.0);

  EXPECT_NEAR(td.queue_pct, 5.0, 0.01);
  EXPECT_NEAR(td.cpu_active_pct, 20.0, 0.01);
  EXPECT_NEAR(td.gpu_wait_pct, 75.0, 0.01);
  EXPECT_EQ(td.bottleneck, WorkloadTimeDistribution::Bottleneck::GpuBound);

  const auto path = std::filesystem::temp_directory_path() / "kyty_test_time_dist.json";
  ASSERT_TRUE(ExportJson(path.string(), &snap));
  std::ifstream in(path);
  const auto j = nlohmann::json::parse(in);
  EXPECT_TRUE(j.contains("time_distribution"));
  EXPECT_NEAR(j["time_distribution"]["gpu_wait_pct"].get<double>(), 75.0, 0.01);
  in.close();
  std::filesystem::remove(path);

  StopCapture();
}

TEST_F(AuditProfilerTest, WorkloadTimeDistributionQueueAndCpuBound) {
  SetEnabled(true);
  StartCapture("tripartite-queue-bound");

  // Queue: 15 ms (60%)
  RecordQueueLatency(QueueLatencyType::Intake, 5'000'000);
  RecordQueueLatency(QueueLatencyType::Backpressure, 10'000'000);
  // CPU: 6 ms (24%)
  RecordPipelineStage(PipelineStage::DescriptorPrep, 6'000'000);
  // GPU: 4 ms (16%)
  RecordGpuWait(GpuWaitReason::BufferReadback, 4'000'000);

  auto snap = TakeSnapshot();
  EXPECT_NEAR(snap.time_distribution.queue_pct, 60.0, 0.01);
  EXPECT_EQ(snap.time_distribution.bottleneck,
            WorkloadTimeDistribution::Bottleneck::QueueSyncBound);
  StopCapture();

  // Now CPU bound test
  StartCapture("tripartite-cpu-bound");
  RecordQueueLatency(QueueLatencyType::Submission, 1'000'000);
  RecordPipelineStage(PipelineStage::DrawPrepWorker, 14'000'000);
  RecordPipelineStage(PipelineStage::Submit, 2'000'000);
  RecordGpuWait(GpuWaitReason::PriorityOperation, 3'000'000);

  snap = TakeSnapshot();
  EXPECT_NEAR(snap.time_distribution.cpu_active_pct, 80.0, 0.01);
  EXPECT_EQ(snap.time_distribution.bottleneck, WorkloadTimeDistribution::Bottleneck::CpuBound);
  StopCapture();
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
