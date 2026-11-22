#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSTREAM_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSTREAM_H_

#include "common/assert.h"
#include "common/common.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Libs::Graphics::CommandStream {

enum class Op : uint16_t {
	Wrap = 0,
	DrainMarker,
	Begin,
	Submit,
	BeginRendering,
	EndRendering,
	PipelineBarrier2,
	PipelineBarrier,
	CopyBuffer,
	CopyBufferToImage,
	CopyImageToBuffer,
	CopyImage,
	FillBuffer,
	BindPipeline,
	BindDescriptorSets,
	PushDescriptorSet,
	PushConstants,
	BindVertexBuffers2,
	BindIndexBuffer,
	SetViewportWithCount,
	SetScissorWithCount,
	Draw,
	DrawIndexed,
	DrawIndirect,
	DrawIndexedIndirect,
	Dispatch,
	DispatchIndirect,
	Count,
};

[[nodiscard]] const char* OpName(Op op) noexcept;

struct Header {
	Op       op    = Op::Wrap;
	uint16_t flags = 0;
	uint32_t size  = 0; // whole packet, bytes
};
static_assert(sizeof(Header) == 8);

inline constexpr uint32_t Align8(uint64_t bytes) noexcept {
	return static_cast<uint32_t>((bytes + 7u) & ~uint64_t {7u});
}

struct WaitStats {
	uint64_t waits   = 0;
	uint64_t wakes   = 0;
	uint64_t spins   = 0;
	uint64_t blocks  = 0;
	uint64_t spin_ns = 0;
	uint64_t wait_ns = 0;
};

struct WaitPolicy {
	uint64_t spin_ns = 30000;
};

// A single-producer single-consumer byte ring of 8-byte aligned packets that never
// straddle its end (a Wrap packet pads to the end instead).
class Ring {
public:
	// capacity: a power of two, at least 64 KiB.
	explicit Ring(uint64_t capacity);
	~Ring();
	KYTY_CLASS_NO_COPY(Ring);

	[[nodiscard]] uint64_t Capacity() const noexcept { return m_capacity; }
	[[nodiscard]] uint32_t MaxPacket() const noexcept {
		return static_cast<uint32_t>(m_capacity / 4u);
	}

	// ---- Producer (one thread) ----
	[[nodiscard]] uint8_t* Reserve(uint32_t bytes, const WaitPolicy& policy, WaitStats& stats);
	void                   SetPublishBatch(uint64_t bytes) noexcept { m_publish_batch = bytes; }
	void                   Commit(uint32_t bytes) noexcept {
		m_write += bytes;
		if (m_write - m_last_published >= m_publish_batch) {
			Publish();
		}
	}
	void Publish() noexcept {
		m_last_published = m_write;
		m_published.store(m_write, std::memory_order_release);
	}
	void                        Kick(WaitStats& stats) noexcept;
	[[nodiscard]] uint64_t      WritePosition() const noexcept { return m_write; }
	[[nodiscard]] const Header* At(uint64_t position) const noexcept {
		return reinterpret_cast<const Header*>(m_data + (position & m_mask));
	}
	void WaitConsumed(uint64_t position, const WaitPolicy& policy, WaitStats& stats);
	[[nodiscard]] uint64_t Consumed() const noexcept {
		return m_consumed.load(std::memory_order_acquire);
	}

	// ---- Consumer (one thread) ----
	[[nodiscard]] const Header* Peek() noexcept {
		if (m_read == m_published_cache) {
			m_published_cache = m_published.load(std::memory_order_acquire);
			if (m_read == m_published_cache) {
				return nullptr;
			}
		}
		return reinterpret_cast<const Header*>(m_data + (m_read & m_mask));
	}
	void                   Advance(uint32_t bytes) noexcept { m_read += bytes; }
	void                   Release(WaitStats& stats) noexcept;
	[[nodiscard]] uint64_t ReadPosition() const noexcept { return m_read; }
	bool WaitPublished(const std::atomic<bool>& stop, const WaitPolicy& policy, WaitStats& stats);
	void WakeConsumer() noexcept;

private:
	bool EnsureSpace(uint64_t bytes, const WaitPolicy& policy, WaitStats& stats);

	uint8_t* m_data     = nullptr;
	uint64_t m_capacity = 0;
	uint64_t m_mask     = 0;

	alignas(64) std::atomic<uint64_t> m_published {0};
	std::atomic<uint32_t> m_consumer_parked {0};
	std::atomic<uint32_t> m_consumer_bell {0};
	alignas(64) std::atomic<uint64_t> m_consumed {0};
	std::atomic<uint32_t> m_producer_waiting {0};
	std::atomic<uint32_t> m_producer_bell {0};

	// Producer-local
	alignas(64) uint64_t m_write = 0;
	uint64_t m_last_published    = 0;
	uint64_t m_publish_batch     = 4u << 10u;
	uint64_t m_consumed_cache    = 0;

	// Consumer-local
	alignas(64) uint64_t m_read = 0;
	uint64_t m_published_cache  = 0;
};

} // namespace Libs::Graphics::CommandStream

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSTREAM_H_
