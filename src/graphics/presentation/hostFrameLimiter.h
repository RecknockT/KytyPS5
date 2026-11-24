#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace Libs::Graphics {

// Host presentation only. Does not change guest clocks, vblank or flip queue policy.
inline std::atomic<uint32_t> g_host_fps_limit {0};

class HostFrameLimiter {
public:
	using Clock = std::chrono::steady_clock;

	Clock::time_point Deadline(Clock::time_point now, uint32_t fps) const {
		if (fps == 0 || fps != m_last_fps || !m_started) {
			return now;
		}
		const auto interval = std::chrono::nanoseconds((1000000000ull + fps - 1) / fps);
		const auto next     = m_last_present + interval;
		return next > now ? next : now;
	}

	void MarkPresented(Clock::time_point now, uint32_t fps) {
		m_last_present = now;
		m_last_fps     = fps;
		m_started      = fps != 0;
	}

	void Wait() {
		const auto fps      = g_host_fps_limit.load(std::memory_order_relaxed);
		const auto now      = Clock::now();
		const auto deadline = Deadline(now, fps);
		if (deadline > now) {
			std::this_thread::sleep_until(deadline);
		}
		// Base the next deadline on actual wake-up time; never burst to catch up.
		MarkPresented(Clock::now(), fps);
	}

private:
	Clock::time_point m_last_present {};
	uint32_t          m_last_fps = 0;
	bool              m_started  = false;
};

} // namespace Libs::Graphics
