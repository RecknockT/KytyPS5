#include "common/pm4OpcodeProfiler.h"

#include "common/auditProfiler.h"

#include <algorithm>
#include <array>
#include <mutex>

namespace Common::AuditProfiler {
std::atomic<bool> g_pm4_profiling_active {false};
namespace {
std::mutex                        g_pm4_mutex;
std::atomic<uint64_t>             g_pm4_epoch {0};
Pm4ProfileSnapshot                g_pm4_config;
std::array<Pm4OpcodeMetrics, 256> g_pm4_buckets {};
thread_local Pm4OpcodeScope*      g_pm4_current = nullptr;
uint64_t                          Ns(Pm4Clock::time_point start, Pm4Clock::time_point end) {
	return end >= start
	           ? static_cast<uint64_t>(
	                 std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count())
	           : 0;
}
} // namespace
void ResetPm4Profile(bool enabled) {
	g_pm4_profiling_active.store(false, std::memory_order_relaxed);
	std::lock_guard lock(g_pm4_mutex);
	g_pm4_epoch.fetch_add(1, std::memory_order_relaxed);
	g_pm4_config         = {};
	g_pm4_config.enabled = enabled;
	g_pm4_buckets        = {};
}
void EnablePm4Profile() {
	std::lock_guard lock(g_pm4_mutex);
	g_pm4_profiling_active.store(g_pm4_config.enabled, std::memory_order_relaxed);
}
void StopPm4Profile() {
	g_pm4_profiling_active.store(false, std::memory_order_relaxed);
}
Pm4ProfileSnapshot TakePm4Snapshot() {
	std::lock_guard lock(g_pm4_mutex);
	auto            result = g_pm4_config;
	for (const auto& row: g_pm4_buckets)
		if (row.calls) result.opcodes.push_back(row);
	return result;
}
Pm4OpcodeScope::Pm4OpcodeScope(uint32_t opcode, Pm4Clock::time_point start) noexcept {
	std::lock_guard lock(g_pm4_mutex);
	if (!g_pm4_profiling_active.load(std::memory_order_relaxed) || !IsCaptureRunning() ||
	    opcode >= 256)
		return;
	m_active = true;
	m_epoch  = g_pm4_epoch.load(std::memory_order_relaxed);
	m_opcode = opcode;
	m_start  = start;
	m_parent = g_pm4_current;
	m_nested = m_parent && m_parent->m_epoch == m_epoch;
	if (m_nested) BeginPm4Child(start);
	g_pm4_current = this;
}
Pm4OpcodeScope::~Pm4OpcodeScope() noexcept {
	if (m_active) Finish(Pm4Clock::now());
}
void Pm4OpcodeScope::Finish(Pm4Clock::time_point end) noexcept {
	if (!m_active) return;
	const auto inclusive = Ns(m_start, end);
	{
		std::lock_guard lock(g_pm4_mutex);
		if (m_epoch == g_pm4_epoch.load(std::memory_order_relaxed)) {
			if (g_pm4_profiling_active.load(std::memory_order_relaxed) && IsCaptureRunning() &&
			    !m_invalid && !m_child_depth && !m_wait_depth && end >= m_start &&
			    m_excluded_ns <= inclusive && m_wait_ns <= m_excluded_ns) {
				auto& row  = g_pm4_buckets[m_opcode];
				row.opcode = m_opcode;
				++row.calls;
				row.inclusive_ns += inclusive;
				row.excluded_ns += m_excluded_ns;
				row.host_wait_ns += m_wait_ns;
				row.max_ns = std::max(row.max_ns, inclusive);
			} else
				++g_pm4_config.discarded_scopes;
		}
	}
	g_pm4_current = m_parent;
	if (m_nested) EndPm4Child(end);
	m_active = false;
}
bool BeginPm4Child(Pm4Clock::time_point now, bool wait) noexcept {
	auto* scope = g_pm4_current;
	if (!scope || !scope->m_active) return false;
	if (scope->m_child_depth++ == 0) scope->m_child_start = now;
	if (wait && scope->m_wait_depth++ == 0) scope->m_wait_start = now;
	return true;
}
void EndPm4Child(Pm4Clock::time_point now, bool wait) noexcept {
	auto* scope = g_pm4_current;
	if (!scope || !scope->m_active) return;
	if (!scope->m_child_depth || (wait && !scope->m_wait_depth)) {
		scope->m_invalid = true;
		return;
	}
	if (--scope->m_child_depth == 0) {
		scope->m_invalid |= now < scope->m_child_start;
		scope->m_excluded_ns += Ns(scope->m_child_start, now);
	}
	if (wait && --scope->m_wait_depth == 0) {
		scope->m_invalid |= now < scope->m_wait_start;
		scope->m_wait_ns += Ns(scope->m_wait_start, now);
	}
}
Pm4ChildScope::Pm4ChildScope(bool wait) noexcept: m_wait(wait) {
	if (g_pm4_profiling_active.load(std::memory_order_relaxed) && g_pm4_current)
		m_active = BeginPm4Child(Pm4Clock::now(), wait);
}
Pm4ChildScope::~Pm4ChildScope() noexcept {
	if (m_active) EndPm4Child(Pm4Clock::now(), m_wait);
}
} // namespace Common::AuditProfiler
