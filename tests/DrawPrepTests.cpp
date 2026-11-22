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

} // namespace

int main() {
  std::printf("=== Starting DrawPrep Unit Tests ===\n");
  TestModeToggle();
  TestSlotAllocationAndWrap();
  TestParallelPreparation();
  TestSnapshotImmutability();
  TestTargetValidationAndFallback();
  TestDrainFlushesAll();

  if (g_failures == 0) {
    std::printf("=== All DrawPrep Tests PASSED ===\n");
    return 0;
  } else {
    std::printf("=== %d DrawPrep Test(s) FAILED ===\n", g_failures);
    return 1;
  }
}
