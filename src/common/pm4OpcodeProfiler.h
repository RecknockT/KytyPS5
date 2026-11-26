#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace Common::AuditProfiler {
using Pm4Clock = std::chrono::steady_clock;
struct Pm4OpcodeMetrics {
	uint32_t opcode = 0;
	uint64_t calls = 0, inclusive_ns = 0, excluded_ns = 0, host_wait_ns = 0, max_ns = 0;
};
struct Pm4ProfileSnapshot {
	bool                          enabled          = false;
	uint64_t                      discarded_scopes = 0;
	std::vector<Pm4OpcodeMetrics> opcodes;
};
extern std::atomic<bool> g_pm4_profiling_active;
void                     ResetPm4Profile(bool enabled);
void                     EnablePm4Profile();
void                     StopPm4Profile();
Pm4ProfileSnapshot       TakePm4Snapshot();
bool                     BeginPm4Child(Pm4Clock::time_point now, bool wait = false) noexcept;
void                     EndPm4Child(Pm4Clock::time_point now, bool wait = false) noexcept;

class Pm4OpcodeScope {
public:
	Pm4OpcodeScope(uint32_t opcode, Pm4Clock::time_point start) noexcept;
	~Pm4OpcodeScope() noexcept;
	void Finish(Pm4Clock::time_point end) noexcept;
	Pm4OpcodeScope(const Pm4OpcodeScope&)            = delete;
	Pm4OpcodeScope& operator=(const Pm4OpcodeScope&) = delete;

private:
	friend bool          BeginPm4Child(Pm4Clock::time_point, bool) noexcept;
	friend void          EndPm4Child(Pm4Clock::time_point, bool) noexcept;
	bool                 m_active = false, m_nested = false, m_invalid = false;
	Pm4OpcodeScope*      m_parent = nullptr;
	uint64_t             m_epoch  = 0;
	uint32_t             m_opcode = 0, m_child_depth = 0, m_wait_depth = 0;
	Pm4Clock::time_point m_start {}, m_child_start {}, m_wait_start {};
	uint64_t             m_excluded_ns = 0, m_wait_ns = 0;
};

// Extra coverage for manual timing paths. Does not modify existing profiler counters.
class Pm4ChildScope {
public:
	explicit Pm4ChildScope(bool wait = false) noexcept;
	~Pm4ChildScope() noexcept;
	Pm4ChildScope(const Pm4ChildScope&)            = delete;
	Pm4ChildScope& operator=(const Pm4ChildScope&) = delete;

private:
	bool m_active = false, m_wait;
};
} // namespace Common::AuditProfiler
