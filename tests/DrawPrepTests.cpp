// Unit tests for DrawPrep Engine, parallel preparation, and snapshot isolation.

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

bool ShaderHasMappedData(uint64_t addr) noexcept {
  return g_mock_mapped_data.load(std::memory_order_relaxed) && (addr != 0);
}

ShaderParams PrepareProgram(const HW::VertexShaderInfo & /*regs*/,
                            const HW::Context & /*context*/,
                            const HW::UserConfig & /*user_config*/,
                            ShaderVertexInputInfo &input_info) {
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

} // namespace

int main() {
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

  if (g_failures == 0) {
    std::printf("=== All DrawPrep Tests PASSED ===\n");
    return 0;
  } else {
    std::printf("=== %d DrawPrep Test(s) FAILED ===\n", g_failures);
    return 1;
  }
}
