#include "graphics/presentation/hostFrameLimiter.h"
#include <gtest/gtest.h>

TEST(HostFrameLimiter, CapsWithoutCatchUpOrGuestTime) {
    using Limiter = Libs::Graphics::HostFrameLimiter;
    using namespace std::chrono_literals;
    Limiter limiter;
    const auto start = Limiter::Clock::time_point(1s);
    EXPECT_EQ(limiter.Deadline(start, 30), start);
    limiter.MarkPresented(start, 30);
    EXPECT_EQ(limiter.Deadline(start + 10ms, 30), start + 33333334ns);
    // Slow frames proceed immediately; there is no attempt to recover missed frames.
    EXPECT_EQ(limiter.Deadline(start + 100ms, 30), start + 100ms);
    limiter.MarkPresented(start + 100ms, 30);
    EXPECT_EQ(limiter.Deadline(start + 101ms, 30), start + 100ms + 33333334ns);
    EXPECT_EQ(limiter.Deadline(start + 101ms, 0), start + 101ms);
    EXPECT_EQ(limiter.Deadline(start + 101ms, 60), start + 101ms);
}

TEST(HostFrameLimiter, RealHostWaitEnforcesThirtyFps) {
    using Limiter = Libs::Graphics::HostFrameLimiter;
    Libs::Graphics::g_host_fps_limit.store(30);
    Limiter limiter;
    const auto start = Limiter::Clock::now();
    for (int i = 0; i < 9; ++i) {
        limiter.Wait();
    }
    const auto elapsed = Limiter::Clock::now() - start;
    Libs::Graphics::g_host_fps_limit.store(0);
    EXPECT_GE(elapsed, std::chrono::nanoseconds(8 * 33333334ull));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
