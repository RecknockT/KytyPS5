#include "graphics/host_gpu/renderer/effectProfiler.h"

#include "common/auditProfiler.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace Libs::Graphics {
uint64_t EffectProfiler::TimestampDelta(uint64_t begin, uint64_t end, uint32_t bits) {
	if (bits == 0 || bits > 64) return 0;
	return bits == 64 ? end - begin : (end - begin) & ((uint64_t {1} << bits) - 1);
}
EffectProfiler::CpuScope::~CpuScope() {
	if (!m_owner.Enabled()) return;
	s_current = m_previous;
	if (m_key.shader == 0) return;
	const auto end = Clock::now();
	const auto ns  = [](auto duration) {
		return static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
	};
	m_owner.RecordCpu(m_key, ns(end - m_start),
	                  m_prepared == Clock::time_point {} ? 0 : ns(m_prepared - m_start),
	                  m_readback_ns, m_readback_count, m_sections);
}
void EffectProfiler::Initialize() {
	m_initialized = true;
	if (!Config::EffectProfileEnabled()) return;
	auto path = Common::PathFromUtf8(Common::AuditProfiler::GetExportPath()).parent_path() /
	            "effects.jsonl";
	m_output.open(path, std::ios::out | std::ios::trunc);
	if (!m_output) {
		std::fprintf(stderr, "EffectProfiler: cannot open output\n");
		return;
	}
	m_enabled             = true;
	const auto properties = m_graphics.physical_device.getQueueFamilyProperties();
	if (m_graphics.queue_family < properties.size())
		m_bits = properties[m_graphics.queue_family].timestampValidBits;
	m_period = m_graphics.physical_device_properties.limits.timestampPeriod;
	if (!Config::GpuTimestampsEnabled() || m_period <= 0) m_bits = 0;
	std::printf("EffectProfiler: enabled, timestamp bits=%u, ns/tick=%.3f\n", m_bits, m_period);
	Export();
}
EffectProfiler::~EffectProfiler() {
	// Scheduler shutdown already waited on its own timeline before destroying these pools.
	for (auto& batch: m_batches)
		if (batch.pool) m_graphics.device.destroyQueryPool(batch.pool);
}
void EffectProfiler::RecordCpu(Key key, uint64_t cpu, uint64_t prep, uint64_t readback,
                               uint64_t                                  readback_count,
                               const std::array<uint64_t, SectionCount>& sections) {
	if (m_stats.size() >= 4096 && !m_stats.contains(key)) {
		++m_dropped;
		return;
	}
	auto& stat = m_stats[key];
	++stat.calls;
	stat.cpu_ns += cpu;
	stat.prep_ns += prep;
	stat.readback_ns += readback;
	stat.readback_count += readback_count;
	for (size_t i = 0; i < SectionCount; ++i)
		stat.sections[i] += sections[i];
}
void EffectProfiler::BeginCommand(vk::CommandBuffer command, uint64_t tick, uint64_t completed) {
	if (!m_initialized) Initialize();
	if (!m_enabled || m_bits == 0) return;
	Poll(completed);
	m_active = nullptr;
	for (auto& batch: m_batches) {
		if (batch.tick != 0) continue;
		if (!batch.pool) {
			vk::QueryPoolCreateInfo info {};
			info.queryType  = vk::QueryType::eTimestamp;
			info.queryCount = QueryCount;
			if (m_graphics.device.createQueryPool(&info, nullptr, &batch.pool) !=
			    vk::Result::eSuccess) {
				++m_dropped;
				return;
			}
		}
		batch.tick   = tick;
		batch.count  = 0;
		batch.sealed = false;
		m_active     = &batch;
		command.resetQueryPool(batch.pool, 0, QueryCount);
		command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, batch.pool, 0);
		return;
	}
	++m_dropped; // Do not block, allocate without bound, or reuse an in-flight pool.
}
void EffectProfiler::EndCommand(vk::CommandBuffer command) {
	if (!m_active) return;
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_active->pool, 1);
	m_active->sealed = true;
	m_active         = nullptr;
}
uint32_t EffectProfiler::BeginSample(vk::CommandBuffer command, Key key) {
	if (!m_active) return UINT32_MAX;
	// Randomized draw sampling avoids always selecting the same ordinal in a repeating frame.
	m_rng ^= m_rng << 13;
	m_rng ^= m_rng >> 17;
	m_rng ^= m_rng << 5;
	if (key.kind == 0 && (m_rng & 63u) != 0) return UINT32_MAX;
	if (m_active->count == MaxSamples) {
		++m_dropped;
		return UINT32_MAX;
	}
	const auto index      = m_active->count++;
	m_active->keys[index] = key;
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_active->pool, 2 + index * 2);
	return index;
}
void EffectProfiler::EndSample(vk::CommandBuffer command, uint32_t sample) {
	if (sample == UINT32_MAX) return;
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_active->pool,
	                       3 + sample * 2);
}
void EffectProfiler::Poll(uint64_t completed) {
	for (auto& batch: m_batches) {
		if (!batch.sealed || batch.tick == 0 || batch.tick > completed) continue;
		std::array<uint64_t, QueryCount> values {};
		const auto                       count  = 2 + batch.count * 2;
		const auto                       result = m_graphics.device.getQueryPoolResults(
		    batch.pool, 0, count, count * sizeof(uint64_t), values.data(), sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64);
		if (result != vk::Result::eSuccess) {
			++m_dropped;
			continue;
		}
		const auto ns =
		    static_cast<uint64_t>(TimestampDelta(values[0], values[1], m_bits) * m_period);
		m_gpu_ns += ns;
		++m_batches_done;
		// Submission intervals include bubbles/overlap; they are not GPU utilization.
		for (uint32_t i = 0; i < batch.count; ++i) {
			if (m_stats.size() >= 4096 && !m_stats.contains(batch.keys[i])) {
				++m_dropped;
				continue;
			}
			auto& stat = m_stats[batch.keys[i]];
			++stat.gpu_samples;
			stat.gpu_ns += static_cast<uint64_t>(
			    TimestampDelta(values[2 + i * 2], values[3 + i * 2], m_bits) * m_period);
		}
		batch.tick   = 0;
		batch.sealed = false;
	}
}
void EffectProfiler::Frame(uint64_t frame) {
	if (!m_enabled) return;
	m_frame = frame;
	if (frame >= m_last_export + 120) {
		Export();
		m_last_export = frame;
	}
}
void EffectProfiler::Finish(uint64_t completed) {
	if (!m_enabled) return;
	Poll(completed);
	Export();
}
void EffectProfiler::Export() {
	nlohmann::json rows = nlohmann::json::array();
	for (const auto& [key, stat]: m_stats) {
		constexpr const char* names[] = {"programs",      "targets",     "descriptor_decode",
		                                 "buffer_lookup", "image_views", "buffer_rebind",
		                                 "pipeline",      "commit",      "emit",
		                                 "vertex_buffers"};
		static_assert(std::size(names) == SectionCount);
		nlohmann::json sections = nlohmann::json::object();
		for (size_t i = 0; i < SectionCount; ++i)
			sections[names[i]] = stat.sections[i];
		char shader[17], pixel[17];
		std::snprintf(shader, sizeof(shader), "%016llx",
		              static_cast<unsigned long long>(key.shader));
		std::snprintf(pixel, sizeof(pixel), "%016llx", static_cast<unsigned long long>(key.pixel));
		rows.push_back({{"kind", key.kind},
		                {"shader", shader},
		                {"pixel", pixel},
		                {"targets", key.targets},
		                {"calls", stat.calls},
		                {"cpu_ns", stat.cpu_ns},
		                {"prep_ns", stat.prep_ns},
		                {"sections_ns", sections},
		                {"readback_ns", stat.readback_ns},
		                {"readback_count", stat.readback_count},
		                {"gpu_samples", stat.gpu_samples},
		                {"gpu_sample_ns", stat.gpu_ns}});
	}
	const nlohmann::json output = {
	    {"frame", m_frame},
	    {"host_ns", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                          Clock::now().time_since_epoch())
	                                          .count())},
	    {"timestamp_bits", m_bits},
	    {"timestamp_period_ns", m_period},
	    {"submission_interval_ns", m_gpu_ns},
	    {"completed_batches", m_batches_done},
	    {"dropped", m_dropped},
	    {"draw_sample_probability", 1.0 / 64},
	    {"rows", rows}};
	m_output << output.dump() << '\n';
	m_output.flush();
}
} // namespace Libs::Graphics
