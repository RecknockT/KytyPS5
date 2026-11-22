#include "graphics/host_gpu/renderer/commandStream.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Libs::Graphics::CommandStream {

namespace {

inline void CpuRelax() noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
	_mm_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#else
	std::this_thread::yield();
#endif
}

inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

template <typename Done>
bool Spin(uint64_t spin_ns, Done&& done) {
	if (done()) {
		return true;
	}
	if (spin_ns == 0) {
		return false;
	}
	const auto start = NowNs();
	for (uint32_t i = 1;; i++) {
		CpuRelax();
		if (done()) {
			return true;
		}
		if ((i & 63u) == 0 && NowNs() - start >= spin_ns) {
			return false;
		}
	}
}

} // namespace

const char* OpName(Op op) noexcept {
	switch (op) {
		case Op::Wrap: return "Wrap";
		case Op::DrainMarker: return "DrainMarker";
		case Op::Begin: return "Begin";
		case Op::Submit: return "Submit";
		case Op::BeginRendering: return "BeginRendering";
		case Op::EndRendering: return "EndRendering";
		case Op::PipelineBarrier2: return "PipelineBarrier2";
		case Op::PipelineBarrier: return "PipelineBarrier";
		case Op::CopyBuffer: return "CopyBuffer";
		case Op::CopyBufferToImage: return "CopyBufferToImage";
		case Op::CopyImageToBuffer: return "CopyImageToBuffer";
		case Op::CopyImage: return "CopyImage";
		case Op::FillBuffer: return "FillBuffer";
		case Op::BindPipeline: return "BindPipeline";
		case Op::BindDescriptorSets: return "BindDescriptorSets";
		case Op::PushDescriptorSet: return "PushDescriptorSet";
		case Op::PushConstants: return "PushConstants";
		case Op::BindVertexBuffers2: return "BindVertexBuffers2";
		case Op::BindIndexBuffer: return "BindIndexBuffer";
		case Op::SetViewportWithCount: return "SetViewportWithCount";
		case Op::SetScissorWithCount: return "SetScissorWithCount";
		case Op::Draw: return "Draw";
		case Op::DrawIndexed: return "DrawIndexed";
		case Op::DrawIndirect: return "DrawIndirect";
		case Op::DrawIndexedIndirect: return "DrawIndexedIndirect";
		case Op::Dispatch: return "Dispatch";
		case Op::DispatchIndirect: return "DispatchIndirect";
		case Op::Count: return "Count";
	}
	return "Unknown";
}

Ring::Ring(uint64_t capacity) {
	EXIT_IF(capacity < (64u << 10u) || (capacity & (capacity - 1u)) != 0);
	m_capacity = capacity;
	m_mask     = capacity - 1u;
	m_data     = static_cast<uint8_t*>(::operator new(capacity, std::align_val_t {64}));
}

Ring::~Ring() {
	::operator delete(m_data, std::align_val_t {64});
}

bool Ring::EnsureSpace(uint64_t bytes, const WaitPolicy& policy, WaitStats& stats) {
	const auto fits = [&] { return m_write + bytes - m_consumed_cache <= m_capacity; };
	if (fits()) {
		return false;
	}
	m_consumed_cache = m_consumed.load(std::memory_order_acquire);
	if (fits()) {
		return false;
	}
	Kick(stats);
	const auto start = NowNs();
	stats.spins++;
	const bool spun = Spin(policy.spin_ns, [&] {
		m_consumed_cache = m_consumed.load(std::memory_order_acquire);
		return fits();
	});
	if (!spun) {
		stats.blocks++;
		for (;;) {
			const auto bell = m_producer_bell.load(std::memory_order_acquire);
			m_producer_waiting.store(1, std::memory_order_seq_cst);
			m_consumed_cache = m_consumed.load(std::memory_order_seq_cst);
			if (fits()) {
				break;
			}
			m_producer_bell.wait(bell, std::memory_order_acquire);
		}
		m_producer_waiting.store(0, std::memory_order_relaxed);
	}
	stats.wait_ns += NowNs() - start;
	return true;
}

uint8_t* Ring::Reserve(uint32_t bytes, const WaitPolicy& policy, WaitStats& stats) {
	EXIT_IF(bytes == 0 || (bytes & 7u) != 0 || bytes > MaxPacket());
	const auto position = m_write & m_mask;
	if (position + bytes > m_capacity) {
		const auto pad = m_capacity - position;
		EnsureSpace(pad + bytes, policy, stats);
		auto* wrap  = reinterpret_cast<Header*>(m_data + position);
		wrap->op    = Op::Wrap;
		wrap->flags = 0;
		wrap->size  = static_cast<uint32_t>(pad);
		m_write += pad;
		return m_data;
	}
	EnsureSpace(bytes, policy, stats);
	return m_data + position;
}

void Ring::Kick(WaitStats& stats) noexcept {
	m_last_published = m_write;
	m_published.store(m_write, std::memory_order_seq_cst);
	if (m_consumer_parked.load(std::memory_order_seq_cst) != 0) {
		m_consumer_bell.fetch_add(1, std::memory_order_release);
		m_consumer_bell.notify_one();
		stats.wakes++;
	}
}

void Ring::WaitConsumed(uint64_t position, const WaitPolicy& policy, WaitStats& stats) {
	EXIT_IF(position > m_write);
	const auto done = [&] {
		m_consumed_cache = m_consumed.load(std::memory_order_acquire);
		return m_consumed_cache >= position;
	};
	if (done()) {
		return;
	}
	Kick(stats);
	const auto start = NowNs();
	stats.spins++;
	const bool spun = Spin(policy.spin_ns, done);
	if (!spun) {
		stats.blocks++;
		for (;;) {
			const auto bell = m_producer_bell.load(std::memory_order_acquire);
			m_producer_waiting.store(1, std::memory_order_seq_cst);
			m_consumed_cache = m_consumed.load(std::memory_order_seq_cst);
			if (m_consumed_cache >= position) {
				break;
			}
			m_producer_bell.wait(bell, std::memory_order_acquire);
		}
		m_producer_waiting.store(0, std::memory_order_relaxed);
	}
	stats.wait_ns += NowNs() - start;
}

void Ring::Release(WaitStats& stats) noexcept {
	m_consumed.store(m_read, std::memory_order_seq_cst);
	if (m_producer_waiting.load(std::memory_order_seq_cst) != 0) {
		m_producer_bell.fetch_add(1, std::memory_order_release);
		m_producer_bell.notify_one();
		stats.wakes++;
	}
}

bool Ring::WaitPublished(const std::atomic<bool>& stop, const WaitPolicy& policy,
                         WaitStats& stats) {
	const auto available = [&] {
		m_published_cache = m_published.load(std::memory_order_acquire);
		return m_published_cache != m_read;
	};
	if (available()) {
		return true;
	}
	const auto start = NowNs();
	stats.spins++;
	const bool spun =
	    Spin(policy.spin_ns, [&] { return available() || stop.load(std::memory_order_acquire); });
	if (!spun) {
		stats.blocks++;
		for (;;) {
			const auto bell = m_consumer_bell.load(std::memory_order_acquire);
			m_consumer_parked.store(1, std::memory_order_seq_cst);
			m_published_cache = m_published.load(std::memory_order_seq_cst);
			if (m_published_cache != m_read || stop.load(std::memory_order_seq_cst)) {
				break;
			}
			m_consumer_bell.wait(bell, std::memory_order_acquire);
		}
		m_consumer_parked.store(0, std::memory_order_relaxed);
	}
	stats.wait_ns += NowNs() - start;
	return m_published_cache != m_read;
}

void Ring::WakeConsumer() noexcept {
	m_consumer_bell.fetch_add(1, std::memory_order_seq_cst);
	m_consumer_bell.notify_all();
}

} // namespace Libs::Graphics::CommandStream
