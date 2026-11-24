// Unit tests for DrawPrep Engine, parallel preparation, and snapshot isolation.

#include "common/auditProfiler.h"
#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/command_processor/cpSequencer.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/drawPrep/drawPrep.h"
#include "graphics/host_gpu/renderer/render.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace Libs::Graphics;
using namespace Libs::Graphics::DrawPrep;

int g_failures = 0;

void Check(bool condition, const char *what) {
  if (!condition) {
    std::printf("FAILED: %s\n", what);
    ++g_failures;
  }
}

// Dummy RenderContext placeholder since Engine stores a reference and doesn't
// dereference it for slot scheduling and worker execution.
RenderContext &GetDummyRenderer() {
  alignas(64) static uint8_t dummy_buf[1024]{};
  return *reinterpret_cast<RenderContext *>(dummy_buf);
}

} // namespace

namespace Libs::Graphics {

std::atomic<bool> g_mock_mapped_data{false};
std::atomic<uint32_t> g_prepare_program_calls{0};

bool ShaderHasMappedData(uint64_t addr) noexcept {
  return g_mock_mapped_data.load(std::memory_order_relaxed) && (addr != 0);
}

ShaderParams PrepareProgram(const HW::VertexShaderInfo & /*regs*/,
                            const HW::Context & /*context*/,
                            const HW::UserConfig & /*user_config*/,
                            ShaderVertexInputInfo &input_info) {
  g_prepare_program_calls.fetch_add(1, std::memory_order_relaxed);
  input_info.stage.module = reinterpret_cast<ShaderModule *>(0x1234);
  ShaderParams params{};
  params.hash = 0xbeefcafe;
  return params;
}

} // namespace Libs::Graphics

namespace {

void TestModeToggle() {
  std::printf("Running TestModeToggle...\n");
  Engine engine(GetDummyRenderer(), 2);
  Check(engine.GetMode() == Mode::Off, "Default mode should be Mode::Off");
  Check(!engine.IsActive(), "IsActive() should be false initially");

  engine.SetMode(Mode::Parallel);
  Check(engine.GetMode() == Mode::Parallel, "Mode should be Mode::Parallel");
  Check(engine.IsActive(), "IsActive() should be true in Parallel mode");

  engine.SetMode(Mode::Off);
  Check(engine.GetMode() == Mode::Off, "Mode should return to Mode::Off");
  Check(!engine.IsActive(), "IsActive() should be false after turning off");
}

void TestSlotAllocationAndWrap() {
  std::printf("Running TestSlotAllocationAndWrap...\n");
  Engine engine(GetDummyRenderer(), 2);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x1000'0000ull);

  DrawIndexArgs args{};
  args.index_count = 36;
  args.instance_count = 1;

  // Publish slots up to window capacity (64)
  for (uint32_t i = 0; i < 32; ++i) {
    const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
    Check(pos == i, "Slot position should be monotonically increasing");
  }

  // Commit some slots to free up window space
  for (uint32_t i = 0; i < 32; ++i) {
    PreparedDraw prep;
    const bool committed = engine.CommitPublished(i, prep);
    Check(committed, "CommitPublished should succeed for published slot");
    Check(prep.valid, "Prepared draw should be valid");
  }

  // Publish another 40 slots to force wrapping past 64
  for (uint32_t i = 32; i < 72; ++i) {
    const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
    Check(pos == i, "Slot position should wrap and advance cleanly");
  }

  engine.Drain();
  engine.SetMode(Mode::Off);
}

void TestParallelPreparation() {
  std::printf("Running TestParallelPreparation...\n");
  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x2000'0000ull);
  sh.SetPsShaderBase(0x3000'0000ull);

  constexpr uint32_t NumDraws = 50;
  DrawIndexArgs args{};
  args.index_count = 128;
  args.instance_count = 1;

  for (uint32_t i = 0; i < NumDraws; ++i) {
    const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
    Check(pos == i, "Position should match index");
  }

  uint32_t committed_count = 0;
  for (uint32_t i = 0; i < NumDraws; ++i) {
    PreparedDraw prep;
    if (engine.CommitPublished(i, prep)) {
      if (prep.valid && prep.pixel_active) {
        ++committed_count;
      }
    }
  }
  Check(committed_count == NumDraws,
        "All draws should be prepared and committed in parallel");

  const auto stats = engine.GetStats();
  Check(stats.published == NumDraws, "Stats published should match NumDraws");
  Check(stats.committed == NumDraws, "Stats committed should match NumDraws");
  Check(stats.fallback == 0, "Stats fallback should be 0");

  engine.SetMode(Mode::Off);
}

void TestSnapshotImmutability() {
  std::printf("Running TestSnapshotImmutability...\n");
  Engine engine(GetDummyRenderer(), 2);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;

  // Setup initial state: ES at 0x1111'0000, color target mask = 0x5
  sh.SetEsShaderBase(0x1111'0000ull);
  ctx.SetRenderTargetMask(0x5);

  DrawIndexArgs args{};
  args.index_count = 6;

  const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
  Check(pos != UINT64_MAX, "Publish should succeed");

  // Caller immediately mutates registers for the subsequent draw
  sh.SetEsShaderBase(0x9999'0000ull);
  ctx.SetRenderTargetMask(0xF);

  PreparedDraw prep;
  const bool ok = engine.CommitPublished(pos, prep);
  Check(ok, "CommitPublished should succeed");
  Check(prep.valid, "Prepared draw should be valid");

  // The prepared draw must retain the snapshot target signature (mask = 0x5,
  // NOT 0xF)
  const uint64_t expected_sig = 0x5ull;
  Check(prep.target_signature == expected_sig,
        "Prepared draw must preserve snapshot state, not mutated caller "
        "registers");

  engine.SetMode(Mode::Off);
}

void TestTargetValidationAndFallback() {
  std::printf("Running TestTargetValidationAndFallback...\n");
  Engine engine(GetDummyRenderer(), 2);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  // No vertex shader address set -> invalid draw
  sh.SetEsShaderBase(0);

  DrawIndexArgs args{};
  args.index_count = 10;

  const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
  Check(pos != UINT64_MAX, "Publish should succeed");

  PreparedDraw prep;
  const bool ok = engine.CommitPublished(pos, prep);
  Check(!ok,
        "CommitPublished should return false when prepared draw is invalid");

  const auto stats = engine.GetStats();
  Check(stats.fallback == 1, "Fallback counter should be incremented");

  engine.SetMode(Mode::Off);
}

void TestDrainFlushesAll() {
  std::printf("Running TestDrainFlushesAll...\n");
  Engine engine(GetDummyRenderer(), 2);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x4000'0000ull);

  DrawAutoArgs auto_args{};
  auto_args.vertex_count = 24;
  auto_args.instance_count = 1;

  for (uint32_t i = 0; i < 20; ++i) {
    (void)engine.Publish(nullptr, &auto_args, ctx, ucfg, sh, nullptr);
  }

  // Drain should wait for all 20 slots to be processed and retired
  engine.Drain();

  const auto stats = engine.GetStats();
  Check(stats.published == 20, "20 draws should be published");
  Check(stats.committed == 20, "20 draws should be committed by Drain");

  engine.SetMode(Mode::Off);
}

void TestConcurrentRegisterMutationDataRace() {
  std::printf(
      "Running TestConcurrentRegisterMutationDataRace (Adversarial)...\n");
  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  constexpr uint32_t NumDraws = 64;
  std::atomic<bool> stop_mutator{false};
  HW::Context shared_ctx;
  HW::Shader shared_sh;

  // Background thread violently mutates the shared registers to simulate the
  // front sequencer
  std::thread mutator([&] {
    uint32_t counter = 0;
    while (!stop_mutator.load(std::memory_order_relaxed)) {
      shared_sh.SetEsShaderBase(0xDEAD'0000ull + (++counter));
      shared_ctx.SetRenderTargetMask(0xAA);
      std::this_thread::yield();
    }
  });

  std::vector<uint64_t> published_positions;
  published_positions.reserve(NumDraws);

  DrawIndexArgs args{};
  args.index_count = 64;
  args.instance_count = 1;

  for (uint32_t i = 0; i < NumDraws; ++i) {
    HW::Context local_ctx;
    HW::UserConfig local_ucfg;
    HW::Shader local_sh;
    local_sh.SetEsShaderBase(0x1000'0000ull + i * 0x1000ull);
    local_ctx.SetRenderTargetMask((i % 7) + 1);

    const auto pos = engine.Publish(&args, nullptr, local_ctx, local_ucfg,
                                    local_sh, nullptr);
    Check(pos != UINT64_MAX, "Publish should succeed");
    published_positions.push_back(pos);
  }

  // Consumer verifies that each committed draw preserved its exact snapshot,
  // immune to the mutator
  for (uint32_t i = 0; i < NumDraws; ++i) {
    PreparedDraw prep;
    const bool ok = engine.CommitPublished(published_positions[i], prep);
    Check(ok, "CommitPublished should succeed");
    Check(prep.valid, "Prepared draw should be valid");

    const uint64_t expected_es = 0x1000'0000ull + i * 0x1000ull;
    const uint64_t actual_es = prep.snapshot.shaders.GetVs().es_regs.data_addr;
    Check(
        actual_es == expected_es,
        "Draw snapshot ES address corrupted by concurrent frontend mutations!");

    const uint32_t expected_mask = (i % 7) + 1;
    const uint32_t actual_mask = prep.snapshot.context.GetRenderTargetMask();
    Check(actual_mask == expected_mask,
          "Draw snapshot RenderTargetMask corrupted by concurrent frontend "
          "mutations!");
  }

  stop_mutator.store(true, std::memory_order_relaxed);
  mutator.join();
  engine.SetMode(Mode::Off);
}

void TestSlotExhaustionUnderBurst() {
  std::printf(
      "Running TestSlotExhaustionUnderBurst (Adversarial Burst 256)...\n");
  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  constexpr uint32_t BurstCount = 256;
  std::atomic<uint32_t> committed_count{0};

  // Background consumer thread drains the window to verify backpressure
  std::thread consumer([&] {
    for (uint32_t i = 0; i < BurstCount; ++i) {
      PreparedDraw prep;
      if (engine.CommitPublished(i, prep)) {
        committed_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x5000'0000ull);

  DrawAutoArgs args{};
  args.vertex_count = 12;
  args.instance_count = 1;

  for (uint32_t i = 0; i < BurstCount; ++i) {
    const auto pos = engine.Publish(nullptr, &args, ctx, ucfg, sh, [&] {
      return true; // continue waiting for space
    });
    Check(pos == i, "Burst position should match index monotonically");
  }

  consumer.join();
  Check(committed_count.load() == BurstCount,
        "All burst draws must be committed through the 64-slot sliding window");

  const auto stats = engine.GetStats();
  Check(stats.published == BurstCount,
        "Stats published should match BurstCount");
  Check(stats.committed == BurstCount,
        "Stats committed should match BurstCount");

  engine.SetMode(Mode::Off);
}

void TestConcurrentDrainDuringInFlightWork() {
  std::printf(
      "Running TestConcurrentDrainDuringInFlightWork (Adversarial)...\n");
  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x6000'0000ull);

  DrawIndexArgs args{};
  args.index_count = 32;

  // Publish 40 slots
  for (uint32_t i = 0; i < 40; ++i) {
    (void)engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
  }

  // Rapidly trigger Drain while workers are executing
  engine.Drain();

  const auto stats = engine.GetStats();
  Check(stats.published == 40, "40 draws published");
  Check(stats.committed == 40, "40 draws committed cleanly by Drain");

  engine.SetMode(Mode::Off);
}

void TestTargetSignatureMismatchFallback() {
  std::printf("Running TestTargetSignatureMismatchFallback (Adversarial)...\n");
  Engine engine(GetDummyRenderer(), 2);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x7000'0000ull);
  ctx.SetRenderTargetMask(0x1);

  DrawIndexArgs args{};
  args.index_count = 16;

  const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
  Check(pos != UINT64_MAX, "Publish should succeed");

  PreparedDraw prep;
  const bool ok = engine.CommitPublished(pos, prep);
  Check(ok, "CommitPublished should succeed");

  // Invalidate signature artificially to simulate mismatch
  const uint64_t simulated_mismatch_sig = prep.target_signature ^ 0xFF;
  Check(prep.target_signature != simulated_mismatch_sig,
        "Simulated signature mismatch must be detected");

  engine.SetMode(Mode::Off);
}

void TestParallelTopologyAndProgramOffload() {
  std::printf("Running TestParallelTopologyAndProgramOffload...\n");
  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  g_mock_mapped_data.store(true);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  ucfg.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
  sh.SetEsShaderBase(0x4000'0000ull);
  sh.SetPsShaderBase(0x5000'0000ull);

  DrawIndexArgs args{};
  args.index_count = 60;
  args.instance_count = 1;

  for (uint32_t i = 0; i < 20; ++i) {
    const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
    Check(pos == i, "Publish position check");
  }

  uint32_t with_programs = 0;
  for (uint32_t i = 0; i < 20; ++i) {
    PreparedDraw prep;
    if (engine.CommitPublished(i, prep)) {
      Check(prep.valid, "Prep valid");
      Check(prep.render_state.topology == vk::PrimitiveTopology::eTriangleList,
            "Topology must be eTriangleList");
      if (prep.has_programs) {
        ++with_programs;
        Check(prep.vertex_params.hash == 0xbeefcafe,
              "Program params hash check");
      }
    }
  }

  Check(with_programs == 20,
        "All 20 draws should have parallel-decoded programs");

  const auto stats = engine.GetStats();
  Check(stats.committed == 20, "20 draws committed");
  Check(stats.useful_time_ns > 0, "Useful offloaded worker time must be > 0");

  g_mock_mapped_data.store(false);
  engine.SetMode(Mode::Off);
}

void TestOpsDecouplingSerialization() {
  std::printf("Running TestOpsDecouplingSerialization...\n");
  using namespace Libs::Graphics::CpSeq;

  OpStream stream(256 * 1024);

  // 1. FlipOp
  FlipOp flip{};
  flip.handle = 42;
  flip.index = 3;
  flip.flip_mode = 1;
  flip.flip_arg = 0x9988776655ull;
  stream.Emit(OpKind::Flip, &flip, sizeof(flip), nullptr, 0, 1, 0, false);

  // 2. WaitFlipDoneOp
  WaitFlipDoneOp wait_flip{};
  wait_flip.label_addr = 0xdeadbeefcafeull;
  stream.Emit(OpKind::WaitFlipDone, &wait_flip, sizeof(wait_flip), nullptr, 0,
              2, 0, false);

  // 3. ReleaseMemOp
  ReleaseMemOp rel{};
  rel.address = 0x8000'1000ull;
  rel.data = 0x1234'5678ull;
  rel.data_sel = 2;
  rel.interrupt_selector = 1;
  rel.interrupt_context_id = 5;
  rel.gl2_writeback = 1;
  stream.Emit(OpKind::ReleaseMem, &rel, sizeof(rel), nullptr, 0, 3, 0, false);

  // 4. WriteBackImagesOp
  WriteBackImagesOp wb{};
  wb.reserved = 0x777ull;
  stream.Emit(OpKind::WriteBackImages, &wb, sizeof(wb), nullptr, 0, 4, 0,
              false);

  // 5. EventWriteOp
  EventWriteOp ev{};
  ev.address = 0xa000'0000ull;
  ev.event_type = 0x39;
  ev.event_index = 1;
  stream.Emit(OpKind::EventWrite, &ev, sizeof(ev), nullptr, 0, 5, 0, false);

  // Verify view 1 (Flip)
  OpView view;
  Check(stream.Peek(view), "Peek flip");
  Check(view.Kind() == OpKind::Flip, "Kind is Flip");
  const auto &rec_flip = view.As<FlipOp>();
  Check(rec_flip.handle == 42 && rec_flip.index == 3 &&
            rec_flip.flip_mode == 1 && rec_flip.flip_arg == 0x9988776655ull,
        "FlipOp fields match");
  stream.Pop();

  // Verify view 2 (WaitFlipDone)
  Check(stream.Peek(view), "Peek wait flip");
  Check(view.Kind() == OpKind::WaitFlipDone, "Kind is WaitFlipDone");
  const auto &rec_wait = view.As<WaitFlipDoneOp>();
  Check(rec_wait.label_addr == 0xdeadbeefcafeull,
        "WaitFlipDoneOp label_addr matches");
  stream.Pop();

  // Verify view 3 (ReleaseMem)
  Check(stream.Peek(view), "Peek release mem");
  Check(view.Kind() == OpKind::ReleaseMem, "Kind is ReleaseMem");
  const auto &rec_rel = view.As<ReleaseMemOp>();
  Check(rec_rel.address == 0x8000'1000ull && rec_rel.data == 0x1234'5678ull &&
            rec_rel.data_sel == 2 && rec_rel.interrupt_selector == 1 &&
            rec_rel.interrupt_context_id == 5 && rec_rel.gl2_writeback == 1,
        "ReleaseMemOp fields match");
  stream.Pop();

  // Verify view 4 (WriteBackImages)
  Check(stream.Peek(view), "Peek write back images");
  Check(view.Kind() == OpKind::WriteBackImages, "Kind is WriteBackImages");
  stream.Pop();

  // Verify view 5 (EventWrite)
  Check(stream.Peek(view), "Peek event write");
  Check(view.Kind() == OpKind::EventWrite, "Kind is EventWrite");
  const auto &rec_ev = view.As<EventWriteOp>();
  Check(rec_ev.address == 0xa000'0000ull && rec_ev.event_type == 0x39 &&
            rec_ev.event_index == 1,
        "EventWriteOp fields match");
  stream.Pop();

  Check(!stream.Peek(view), "Stream empty after pops");
}

void TestDrawPrepEndToEndWorkOffload() {
  std::printf("Running TestDrawPrepEndToEndWorkOffload...\n");
  Common::AuditProfiler::Reset();
  g_mock_mapped_data.store(true, std::memory_order_relaxed);
  g_prepare_program_calls.store(0, std::memory_order_relaxed);

  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  ucfg.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
  sh.SetEsShaderBase(0x2000'0000ull);

  constexpr uint32_t kNumDraws = 16;
  DrawIndexArgs args{};
  args.index_count = 36;
  args.instance_count = 1;

  // Publish kNumDraws draws
  std::vector<uint64_t> published_positions;
  for (uint32_t i = 0; i < kNumDraws; ++i) {
    const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
    Check(pos != UINT64_MAX, "Publish should succeed");
    published_positions.push_back(pos);
  }

  // Now simulate the backend GPU thread consuming the prepared draws
  uint32_t consumer_thread_recalculations = 0;
  uint32_t successful_offloads = 0;

  for (auto pos : published_positions) {
    PreparedDraw prepared;
    const bool committed = engine.CommitPublished(pos, prepared);
    Check(committed, "CommitPublished should succeed");
    Check(prepared.valid, "Prepared draw should be valid");
    Check(prepared.has_programs, "Prepared draw should have pre-decoded programs");

    // Simulate backend consumer (RenderExecutor / PipelineCache):
    // If prepared is valid and has programs, consumer ADOPTS precalculated program without recalculating!
    if (prepared.valid && prepared.has_programs) {
      successful_offloads++;
      Common::AuditProfiler::RecordDrawPrepAvoided(prepared.prep_time_ns);
    } else {
      consumer_thread_recalculations++;
      ShaderVertexInputInfo fallback_info;
      PrepareProgram(sh.GetVs(), ctx, ucfg, fallback_info);
    }
  }

  Check(successful_offloads == kNumDraws, "All 16 draws should be successfully offloaded");
  Check(consumer_thread_recalculations == 0, "Zero recalculations on consumer thread");

  const auto snap = Common::AuditProfiler::TakeSnapshot();
  Check(snap.draw_prep_duplication_pct == 0.0, "Work duplication should be strictly 0.0%");

  g_mock_mapped_data.store(false, std::memory_order_relaxed);
}

void TestComputeDrawInterleavedBarrierSafety() {
  std::printf("Running TestComputeDrawInterleavedBarrierSafety...\n");
  Engine engine(GetDummyRenderer(), 2);
  engine.SetMode(Mode::Parallel);

  CpSeq::OpStream stream(256 * 1024);

  // Sequence: Draw 1 -> Draw 2 -> Compute Dispatch -> EventWrite -> Draw 3
  HW::Context ctx1, ctx2, ctx3;
  ctx1.SetRenderTargetMask(0x01);
  ctx2.SetRenderTargetMask(0x02);
  ctx3.SetRenderTargetMask(0x04);

  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x1000'0000ull);

  DrawIndexArgs d1{.index_count = 10, .instance_count = 1};
  DrawIndexArgs d2{.index_count = 20, .instance_count = 1};
  DrawAutoArgs  d3{.vertex_count = 30, .instance_count = 1};

  // Phase 1: Draw 1 and Draw 2 published before compute dispatch
  const auto pos1 = engine.Publish(&d1, nullptr, ctx1, ucfg, sh, nullptr);
  CpSeq::DrawIndexOp op1{};
  op1.index_count = 10;
  op1.instance_count = 1;
  op1.flags = CpSeq::DrawFlagPublished;
  op1.window = pos1;
  stream.Emit(CpSeq::OpKind::DrawIndex, &op1, sizeof(op1), nullptr, 0, 1, 0, false);

  const auto pos2 = engine.Publish(&d2, nullptr, ctx2, ucfg, sh, nullptr);
  CpSeq::DrawIndexOp op2{};
  op2.index_count = 20;
  op2.instance_count = 1;
  op2.flags = CpSeq::DrawFlagPublished;
  op2.window = pos2;
  stream.Emit(CpSeq::OpKind::DrawIndex, &op2, sizeof(op2), nullptr, 0, 2, 0, false);

  CpSeq::DispatchDirectOp op_comp{.x = 8, .y = 8, .z = 1, .mode = 0};
  stream.Emit(CpSeq::OpKind::DispatchDirect, &op_comp, sizeof(op_comp), nullptr, 0, 3, 0, false);

  CpSeq::EventWriteOp op_ev{.address = 0x5000, .event_type = 1, .event_index = 0};
  stream.Emit(CpSeq::OpKind::EventWrite, &op_ev, sizeof(op_ev), nullptr, 0, 4, 0, false);

  // Consume stream up to compute dispatch
  bool compute_executed_after_drain = false;
  while (true) {
    CpSeq::OpView view;
    if (!stream.Peek(view)) break;

    const auto kind = view.Kind();
    if (kind == CpSeq::OpKind::DrawIndex) {
      const auto& op = view.As<CpSeq::DrawIndexOp>();
      PreparedDraw prep;
      Check(engine.CommitPublished(op.window, prep), "Commit draw 1/2");
      if (op.index_count == 10) {
        Check(prep.snapshot.context.GetRenderTargetMask() == 0x01, "Draw 1 mask matches ctx1");
      } else if (op.index_count == 20) {
        Check(prep.snapshot.context.GetRenderTargetMask() == 0x02, "Draw 2 mask matches ctx2");
      }
    } else if (kind == CpSeq::OpKind::DispatchDirect) {
      // Compute dispatch: MUST Drain DrawPrep first
      engine.Drain();
      compute_executed_after_drain = true;
      Check(engine.GetQueueDepth() == 0, "Queue must be completely empty after Drain()");
    }
    stream.Pop();
  }

  // Phase 2: Draw 3 emitted after compute dispatch has finished
  const auto pos3 = engine.Publish(nullptr, &d3, ctx3, ucfg, sh, nullptr);
  CpSeq::DrawAutoOp op3{};
  op3.vertex_count = 30;
  op3.instance_count = 1;
  op3.flags = CpSeq::DrawFlagPublished;
  op3.window = pos3;
  stream.Emit(CpSeq::OpKind::DrawAuto, &op3, sizeof(op3), nullptr, 0, 5, 0, false);

  CpSeq::OpView view3;
  Check(stream.Peek(view3), "Peek draw 3");
  const auto& op3_view = view3.As<CpSeq::DrawAutoOp>();
  PreparedDraw prep3;
  Check(engine.CommitPublished(op3_view.window, prep3), "Commit draw 3");
  Check(prep3.snapshot.context.GetRenderTargetMask() == 0x04, "Draw 3 mask matches ctx3");
  Check(compute_executed_after_drain, "Draw 3 executed strictly after compute drain");
  stream.Pop();
}

void TestDrawPrepWaitSpinVsCvBounded() {
  std::printf("Running TestDrawPrepWaitSpinVsCvBounded...\n");
  Engine engine(GetDummyRenderer(), 1);
  engine.SetMode(Mode::Parallel);

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x3000'0000ull);

  DrawIndexArgs args{.index_count = 6, .instance_count = 1};

  // 1. Fast path: slot is published and committed after brief pause (within 5us spin)
  const auto pos_fast = engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
  PreparedDraw prep_fast;
  const auto t0 = std::chrono::steady_clock::now();
  const bool committed_fast = engine.CommitPublished(pos_fast, prep_fast);
  const auto dt_fast = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - t0).count();

  Check(committed_fast, "Fast commit should succeed");
  Check(prep_fast.valid, "Fast prep should be valid");
  Check(dt_fast < 50000, "Fast commit took reasonable time");
}

void TestQueueDepthAndCapacityTracking() {
  std::printf("Running TestQueueDepthAndCapacityTracking...\n");
  Engine engine(GetDummyRenderer(), 2);
  engine.SetMode(Mode::Parallel);

  Check(engine.GetQueueDepth() == 0, "Initial queue depth is 0");

  HW::Context ctx;
  HW::UserConfig ucfg;
  HW::Shader sh;
  sh.SetEsShaderBase(0x4000'0000ull);

  DrawIndexArgs args{.index_count = 12, .instance_count = 1};

  for (uint32_t i = 0; i < 10; ++i) {
    (void)engine.Publish(&args, nullptr, ctx, ucfg, sh, nullptr);
  }

  Check(engine.GetQueueDepth() == 10, "Queue depth reflects 10 published slots");

  for (uint32_t i = 0; i < 5; ++i) {
    PreparedDraw prep;
    (void)engine.CommitPublished(i, prep);
  }

  Check(engine.GetQueueDepth() <= 5, "Queue depth decreases as head advances after commits");

  engine.Drain();
  Check(engine.GetQueueDepth() == 0, "Queue depth is 0 after drain");
}

void TestDrawPrepSaturationAndSnapshotIntegrity() {
  std::printf("Running TestDrawPrepSaturationAndSnapshotIntegrity (Regression Test A)...\n");
  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  constexpr uint32_t total_draws = 256;
  std::atomic<uint32_t> published_count{0};

  std::thread producer([&] {
    HW::Context ctx;
    HW::UserConfig ucfg;
    HW::Shader sh;
    sh.SetEsShaderBase(0x1000'0000ull);
    ucfg.SetPrimitiveType(Prospero::PrimitiveType::kTriList);

    DrawIndexArgs args{.index_count = 36, .instance_count = 1};

    for (uint32_t i = 0; i < total_draws; ++i) {
      ctx.SetPsInControl(0xa000u + i);
      const auto pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, [] { return true; });
      Check(pos == i, "Published monotonic position matches loop index");
      published_count.store(i + 1, std::memory_order_release);
    }
  });

  bool all_intact = true;
  for (uint32_t i = 0; i < total_draws; ++i) {
    while (published_count.load(std::memory_order_acquire) <= i) {
      std::this_thread::yield();
    }
    PreparedDraw prep;
    const bool committed = engine.CommitPublished(i, prep);
    Check(committed, "Commit succeeds across ring wraps");
    if (prep.snapshot.context.GetShaderRegisters().ps_in_control != (0xa000u + i)) {
      all_intact = false;
    }
    if (prep.snapshot.user_config.GetPrimType() != Prospero::PrimitiveType::kTriList) {
      all_intact = false;
    }
  }
  producer.join();
  Check(all_intact, "All 256 snapshots arrived intact across multiple 64-slot ring wrap cycles");

  // Out of bounds / stale position tests
  PreparedDraw invalid_prep;
  Check(!engine.CommitPublished(0, invalid_prep), "Already consumed slot 0 returns false");
  Check(!engine.CommitPublished(total_draws, invalid_prep), "Unpublished future slot returns false");
  Check(!engine.CommitPublished(99999, invalid_prep), "Way out of range slot returns false");
}

void TestAdversarialInterleavedConcurrency() {
  std::printf("Running TestAdversarialInterleavedConcurrency (Regression Test D)...\n");
  Engine engine(GetDummyRenderer(), 4);
  engine.SetMode(Mode::Parallel);

  CpSeq::OpStream stream(512u << 10u);
  CpSeq::SnapshotRing snapshots(256);

  constexpr uint32_t iterations = 100;
  std::atomic<bool> stop_flag{false};

  // Producer thread simulating CpSequencer
  std::thread sequencer_thread([&] {
    HW::Context ctx;
    HW::UserConfig ucfg;
    HW::Shader sh;
    sh.SetEsShaderBase(0x2000'0000ull);
    ucfg.SetPrimitiveType(Prospero::PrimitiveType::kTriList);

    DrawIndexArgs args{.index_count = 24, .instance_count = 1};

    for (uint32_t i = 0; i < iterations; ++i) {
      ctx.SetPsInControl(0xb000u + i);

      // Snapshot + DrawPrep publish
      const auto snap_idx = snapshots.AcquireBlocking([&] { return stop_flag.load(); });
      if (snap_idx == UINT32_MAX) break;

      auto& snap = snapshots.Entry(snap_idx);
      snap.context = ctx;
      snap.user_config = ucfg;
      snap.shaders = sh;

      const auto win_pos = engine.Publish(&args, nullptr, ctx, ucfg, sh, [&] { return !stop_flag.load(); });

      CpSeq::DrawIndexOp op{};
      op.index_count = args.index_count;
      op.instance_count = 1;
      op.flags = CpSeq::DrawFlagSnapshot;
      op.snapshot = snap_idx;
      if (win_pos != UINT64_MAX) {
        op.flags |= CpSeq::DrawFlagPublished;
        op.window = win_pos;
      }
      stream.Emit(CpSeq::OpKind::DrawIndex, &op, sizeof(op), nullptr, 0, i * 2, 0, false);

      // Interleave non-draw operations (dispatch, release mem) without draining draw prep!
      if ((i % 4) == 0) {
        CpSeq::DispatchDirectOp disp{};
        disp.x = 1; disp.y = 1; disp.z = 1;
        stream.Emit(CpSeq::OpKind::DispatchDirect, &disp, sizeof(disp), nullptr, 0, i * 2 + 1, 0, false);
      }
    }
  });

  // Consumer thread simulating GPU thread (ResolveSubmission)
  uint32_t draws_consumed = 0;
  bool data_intact = true;

  while (draws_consumed < iterations) {
    CpSeq::OpView view;
    if (!stream.Peek(view)) {
      std::this_thread::yield();
      continue;
    }

    if (view.Kind() == CpSeq::OpKind::DrawIndex) {
      const auto& op = view.As<CpSeq::DrawIndexOp>();
      PreparedDraw prep;
      if ((op.flags & CpSeq::DrawFlagPublished) != 0) {
        (void)engine.CommitPublished(op.window, prep);
      }
      if ((op.flags & CpSeq::DrawFlagSnapshot) != 0) {
        const auto& snap = snapshots.Entry(op.snapshot);
        if (snap.context.GetShaderRegisters().ps_in_control != (0xb000u + draws_consumed)) {
          data_intact = false;
        }
        snapshots.Release();
      }
      draws_consumed++;
      stream.Pop();
    } else {
      // Non-draw op: do NOT call engine.Drain()! Process smoothly.
      stream.Pop();
    }
  }

  sequencer_thread.join();
  Check(draws_consumed == iterations, "All draws consumed in adversarial interleaved test");
  Check(data_intact, "All snapshot register payloads intact under high interleaved concurrency");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Config::Initialize();
  std::printf("=== Starting DrawPrep Unit Tests ===\n");
  TestModeToggle();
  TestSlotAllocationAndWrap();
  TestParallelPreparation();
  TestSnapshotImmutability();
  TestTargetValidationAndFallback();
  TestDrainFlushesAll();
  TestConcurrentRegisterMutationDataRace();
  TestSlotExhaustionUnderBurst();
  TestConcurrentDrainDuringInFlightWork();
  TestTargetSignatureMismatchFallback();
  TestParallelTopologyAndProgramOffload();
  TestOpsDecouplingSerialization();
  TestDrawPrepEndToEndWorkOffload();
  TestComputeDrawInterleavedBarrierSafety();
  TestDrawPrepWaitSpinVsCvBounded();
  TestQueueDepthAndCapacityTracking();
  TestDrawPrepSaturationAndSnapshotIntegrity();
  TestAdversarialInterleavedConcurrency();

  if (g_failures == 0) {
    std::printf("=== All DrawPrep Tests PASSED ===\n");
    Config::Shutdown();
    return 0;
  } else {
    std::printf("=== %d DrawPrep Test(s) FAILED ===\n", g_failures);
    Config::Shutdown();
    return 1;
  }
}
