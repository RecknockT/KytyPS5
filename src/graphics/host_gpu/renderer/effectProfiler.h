#pragma once

#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
// Diagnostic only. Owned and accessed by the render thread, never by completion callbacks.
class EffectProfiler {
public:
	using Clock = std::chrono::steady_clock;
	struct Key {
		uint64_t shader = 0, pixel = 0;
		uint32_t kind = 0, targets = 0; // 0 draw, 1 direct compute, 2 indirect compute
		auto     operator<=>(const Key&) const = default;
	};
	enum class Section {
		Programs,
		Targets,
		DescriptorDecode,
		BufferLookup,
		ImageViews,
		BufferRebind,
		Pipeline,
		Commit,
		Emit,
		VertexBuffers,
		Count
	};
	static constexpr size_t SectionCount = static_cast<size_t>(Section::Count);
	class CpuScope {
	public:
		explicit CpuScope(EffectProfiler& owner): m_owner(owner) {
			if (owner.Enabled()) {
				m_start    = Clock::now();
				m_previous = s_current;
				s_current  = this;
			}
		}
		~CpuScope();
		void AddReadback(uint64_t ns) {
			m_readback_ns += ns;
			++m_readback_count;
		}
		void AddSection(Section section, uint64_t ns) {
			m_sections[static_cast<size_t>(section)] += ns;
		}
		void Identify(Key key) { m_key = key; }
		void Prepared() {
			if (m_owner.Enabled()) m_prepared = Clock::now();
		}

	private:
		CpuScope*                          m_previous    = nullptr;
		uint64_t                           m_readback_ns = 0, m_readback_count = 0;
		std::array<uint64_t, SectionCount> m_sections {};
		EffectProfiler&                    m_owner;
		Key                                m_key;
		Clock::time_point                  m_start {}, m_prepared {};
	};
	class SectionScope {
	public:
		explicit SectionScope(Section section): m_scope(s_current), m_section(section) {
			if (m_scope) m_start = Clock::now();
		}
		~SectionScope() {
			if (m_scope)
				m_scope->AddSection(
				    m_section,
				    static_cast<uint64_t>(
				        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start)
				            .count()));
		}

	private:
		CpuScope*         m_scope;
		Section           m_section;
		Clock::time_point m_start {};
	};
	class ReadbackScope {
	public:
		ReadbackScope(): m_scope(s_current) {
			if (m_scope) m_start = Clock::now();
		}
		~ReadbackScope() {
			if (m_scope)
				m_scope->AddReadback(static_cast<uint64_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start)
				        .count()));
		}

	private:
		CpuScope*         m_scope;
		Clock::time_point m_start {};
	};
	explicit EffectProfiler(GraphicContext& graphics): m_graphics(graphics) {}
	~EffectProfiler();
	bool     Enabled() const noexcept { return m_enabled; }
	void     BeginCommand(vk::CommandBuffer command, uint64_t tick, uint64_t completed);
	void     EndCommand(vk::CommandBuffer command);
	uint32_t BeginSample(vk::CommandBuffer command, Key key);
	void     EndSample(vk::CommandBuffer command, uint32_t sample);
	void     Frame(uint64_t frame);
	void     Finish(uint64_t completed);
	// Modular subtraction for devices whose timestamps have fewer than 64 valid bits.
	static uint64_t TimestampDelta(uint64_t begin, uint64_t end, uint32_t bits);

private:
	static constexpr uint32_t MaxSamples = 128, QueryCount = 2 + MaxSamples * 2;
	struct Stat {
		uint64_t calls = 0, cpu_ns = 0, prep_ns = 0, gpu_samples = 0, gpu_ns = 0;
		uint64_t readback_ns = 0, readback_count = 0;
		std::array<uint64_t, SectionCount> sections {};
	};
	struct Batch {
		vk::QueryPool               pool   = nullptr;
		uint64_t                    tick   = 0;
		uint32_t                    count  = 0;
		bool                        sealed = false;
		std::array<Key, MaxSamples> keys {};
	};
	void Initialize();
	void Poll(uint64_t completed);
	void Export();
	void RecordCpu(Key key, uint64_t cpu, uint64_t prep, uint64_t readback, uint64_t readback_count,
	               const std::array<uint64_t, SectionCount>& sections);
	inline static thread_local CpuScope* s_current = nullptr;
	GraphicContext&                      m_graphics;
	bool                                 m_initialized = false, m_enabled = false;
	uint32_t                             m_bits = 0, m_rng = 0x12345678;
	double                               m_period = 0;
	std::array<Batch, 32>                m_batches {};
	Batch*                               m_active = nullptr;
	std::map<Key, Stat>                  m_stats;
	std::ofstream                        m_output;
	uint64_t m_frame = 0, m_last_export = 0, m_gpu_ns = 0, m_batches_done = 0, m_dropped = 0;
};
} // namespace Libs::Graphics
