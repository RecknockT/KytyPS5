#include "common/auditProfiler.h"
#include "common/emulatorConfig.h"
#include "graphics/presentation/window/benchmarkReplay.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace Libs::Graphics {
void HostInputKey(int, bool) { ADD_FAILURE() << "Unexpected game input"; }
}

TEST(BenchmarkReplay, PendingModeAndCaptureWaitWithoutClosingTheEmulator) {
    namespace Audit = Common::AuditProfiler;
    const auto folder = std::filesystem::temp_directory_path() /
        ("kyty_replay_regression_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto script = folder / "replay.json";
    std::ofstream(script) << R"([{"at_ms":0,"action":"binding_reuse","enabled":true},
        {"at_ms":0,"action":"profile_pm4","enabled":true},{"at_ms":0,"action":"capture","label":"next"},{"at_ms":100000,"action":"quit"}])";
#ifdef _WIN32
    _putenv_s("KYTY_BENCH_REPLAY", script.string().c_str());
    _putenv_s("KYTY_BENCH_ARM_FILE", "");
#else
    setenv("KYTY_BENCH_REPLAY", script.string().c_str(), 1);
    unsetenv("KYTY_BENCH_ARM_FILE");
#endif
    Config::Initialize();
    Config::Load({});
    Audit::Reset();
    Audit::SetEnabled(true);
    Audit::SetExportPath((folder / "first.json").string());
    Audit::StartCapture("first");
    {
        Libs::Graphics::BenchmarkReplay replay;
        EXPECT_TRUE(replay.Update());
        EXPECT_EQ(replay.WaitLimit(50), 10);
        Config::ApplyBindingReuseMode();
        EXPECT_FALSE(Config::BindingReuseEnabled());
        Audit::StopCapture();
        EXPECT_TRUE(replay.Update());
        Config::ApplyBindingReuseMode();
        EXPECT_TRUE(Config::BindingReuseEnabled());
        EXPECT_TRUE(Audit::IsCaptureRunning());
        EXPECT_TRUE(Audit::TakeSnapshot().pm4_profile.enabled);
        Audit::StopCapture();
    }
    Audit::SetEnabled(false);
    Audit::Reset();
    Config::Shutdown();
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
