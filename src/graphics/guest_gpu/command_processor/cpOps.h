#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPOPS_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPOPS_H_

#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/commandStream.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Libs::Graphics::CpSeq {

enum class Mode : uint8_t { Off, Inline, Thread };

[[nodiscard]] Mode ConfiguredMode();
[[nodiscard]] int  VerifyMode();
[[nodiscard]] bool PacketHashing();

enum class OpKind : uint16_t {
	DrawIndex,
	DrawAuto,
	DrawIndirect,
	DrawIndirectMulti,
	DispatchDirect,
	DispatchIndirect,
	EndOfPipe,
	ReleaseMem,
	EventWrite,
	WriteData,
	ReferenceClock,
	DmaData,
	LodStats,
	Pssr,
	Flip,
	WaitRegMem,
	WaitFlipDone,
	DumpConstRam,
	Predication,
	CondExec,
	Branch,
	ReadCheck,
	StreamBegin,
	StreamEnd,
	Handoff,
	LockstepRead,
	SkipSlots,
	WriteBackImages,
	Sync,
	Count,
};

[[nodiscard]] const char* OpKindName(OpKind kind) noexcept;

[[nodiscard]] constexpr bool IsLockstep(OpKind kind) noexcept {
	switch (kind) {
		case OpKind::WaitRegMem:
		case OpKind::WaitFlipDone:
		case OpKind::Predication:
		case OpKind::CondExec:
		case OpKind::Branch:
		case OpKind::LockstepRead: return true;
		default: return false;
	}
}

struct Result {
	bool     suspended = false;
	uint64_t value     = 0;
};

[[nodiscard]] constexpr bool IsTransport(OpKind kind) noexcept {
	switch (kind) {
		case OpKind::StreamBegin:
		case OpKind::StreamEnd:
		case OpKind::Handoff:
		case OpKind::LockstepRead:
		case OpKind::SkipSlots: return true;
		default: return false;
	}
}

inline constexpr uint16_t FlagVerify       = 1u << 0u;
inline constexpr uint16_t FlagAdvanceEpoch = 1u << 1u;

struct OpHeader {
	CommandStream::Header ring;
	OpKind                kind         = OpKind::Count;
	uint16_t              flags        = 0;
	uint32_t              data_size    = 0;
	uint64_t              sequence     = 0;
	uint64_t              packet       = 0;
	uint64_t              packets_hash = 0;
	uint64_t              verify_hash  = 0;
	uint64_t              emit_time_ns = 0;
};
static_assert(sizeof(OpHeader) == 56);

// ---- Payloads ----

inline constexpr uint32_t DrawFlagInheritInstances = 1u << 0u;
inline constexpr uint32_t DrawFlagPublished        = 1u << 1u;
inline constexpr uint32_t DrawFlagSnapshot         = 1u << 2u;

struct DrawIndexOp {
	uint64_t index_addr                 = 0;
	uint32_t index_count                = 0;
	uint32_t instance_count             = 0;
	uint32_t index_type_and_size        = 0;
	int32_t  base_vertex                = 0;
	uint32_t first_instance             = 0;
	uint32_t render_target_slice_offset = 0;
	uint32_t offset_source              = 0;
	uint32_t flags                      = 0;
	uint64_t window                     = 0;
	uint32_t snapshot                   = 0;
	uint32_t reserved                   = 0;
};

struct DrawAutoOp {
	uint32_t vertex_count               = 0;
	uint32_t instance_count             = 0;
	uint32_t first_vertex               = 0;
	uint32_t first_instance             = 0;
	uint32_t offset_source              = 0;
	uint32_t render_target_slice_offset = 0;
	uint32_t flags                      = 0;
	uint32_t snapshot                   = 0;
	uint64_t window                     = 0;
};

inline constexpr uint32_t IndirectFlagIndexed      = 1u << 0u;
inline constexpr uint32_t IndirectFlagSetInstances = 1u << 1u;
inline constexpr uint32_t IndirectFlagSnapshot     = 1u << 2u;
inline constexpr uint32_t IndirectFlagPrepared     = 1u << 3u;

struct DrawIndirectOp {
	uint64_t args_base           = 0;
	uint64_t index_base_addr     = 0;
	uint64_t count_addr          = 0;
	uint32_t data_offset         = 0;
	uint32_t draw_initiator      = 0;
	uint32_t index_buffer_size   = 0;
	uint32_t index_type_and_size = 0;
	uint32_t max_count_or_count  = 0;
	uint32_t stride              = 0;
	uint32_t flags               = 0;
	uint32_t num_instances       = 0;
	uint32_t snapshot            = 0;
	uint32_t draw_index_sgpr     = 0;
	uint32_t vertex_offset       = 0;
	uint32_t instance_offset     = 0;
	uint32_t index_offset        = 0;
	uint32_t register_padding    = 0;
	uint64_t window              = 0;
};

inline constexpr uint32_t DispatchFlagSnapshot = 1u << 0u;
inline constexpr uint32_t DispatchFlagPrepared = 1u << 1u;

struct DispatchDirectOp {
	uint32_t x        = 0;
	uint32_t y        = 0;
	uint32_t z        = 0;
	uint32_t mode     = 0;
	uint32_t flags    = 0;
	uint32_t snapshot = 0;
	uint64_t window   = 0;
};

struct DispatchIndirectOp {
	uint64_t args_addr = 0;
	uint32_t mode      = 0;
	uint32_t flags     = 0;
	uint32_t snapshot  = 0;
	uint32_t reserved  = 0;
	uint64_t window    = 0;
};

struct EndOfPipeOp {
	uint64_t dst                  = 0;
	uint64_t value                = 0;
	uint32_t cache_policy         = 0;
	uint32_t event_write_dest     = 0;
	uint32_t eop_event_type       = 0;
	uint32_t cache_action         = 0;
	uint32_t event_index          = 0;
	uint32_t event_write_source   = 0;
	uint32_t interrupt_selector   = 0;
	uint32_t interrupt_context_id = 0;
	uint32_t size                 = 0;
	uint32_t reserved             = 0;
};

struct ReleaseMemOp {
	uint64_t address              = 0;
	uint64_t data                 = 0;
	uint32_t data_sel             = 0;
	uint32_t interrupt_selector   = 0;
	uint32_t interrupt_context_id = 0;
	uint32_t gl2_writeback        = 0;
};

struct EventWriteOp {
	uint64_t address     = 0;
	uint32_t event_type  = 0;
	uint32_t event_index = 0;
};

struct SyncOp {
	uint32_t action   = 0; // 1: flush, 2: finish, 3: promote guest writes.
	uint32_t reserved = 0;
};

struct WriteDataOp {
	uint64_t dst           = 0;
	uint32_t dw_num        = 0;
	uint32_t write_control = 0;
};

struct ReferenceClockOp {
	uint64_t dst       = 0;
	uint32_t num_bytes = 0;
	uint32_t reserved  = 0;
};

struct DmaDataOp {
	uint64_t dst               = 0;
	uint64_t src               = 0;
	uint32_t num_bytes         = 0;
	uint8_t  engine            = 0;
	uint8_t  dst_sel           = 0;
	uint8_t  dst_cache_policy  = 0;
	uint8_t  src_sel           = 0;
	uint8_t  src_cache_policy  = 0;
	uint8_t  wait_for_previous = 0;
	uint8_t  write_confirm     = 0;
	uint8_t  block_engine      = 0;
	uint32_t reserved          = 0;
};

struct LodStatsOp {
	uint32_t body[4] {};
};

struct PssrOp {
	uint32_t request[52] {};
};

enum class FlipVariant : uint32_t { Plain = 0, Label = 1, LabelInterrupt = 2 };

struct FlipOp {
	int64_t  flip_arg       = 0;
	uint64_t dst            = 0;
	int32_t  handle         = 0;
	int32_t  index          = 0;
	int32_t  flip_mode      = 0;
	uint32_t variant        = 0;
	uint32_t value          = 0;
	uint32_t eop_event_type = 0;
	uint32_t cache_action   = 0;
	uint32_t reserved       = 0;
};

struct WaitRegMemOp {
	uint64_t addr    = 0;
	uint64_t ref     = 0;
	uint64_t mask    = 0;
	uint32_t func    = 0;
	uint32_t poll    = 0;
	uint32_t wait_op = 0;
	uint32_t size    = 0;
};

struct WaitFlipDoneOp {
	uint32_t video_out_handle     = 0;
	uint32_t display_buffer_index = 0;
	uint64_t flip_watermark       = UINT64_MAX;
	uint64_t label_addr           = 0;
};

struct DumpConstRamOp {
	uint64_t dst    = 0;
	uint32_t offset = 0;
	uint32_t dw_num = 0;
};

struct PredicationOp {
	uint64_t address         = 0;
	uint32_t condition       = 0;
	uint32_t op              = 0;
	uint32_t wait_op         = 0;
	uint32_t count_in_dwords = 0;
};

struct CondExecOp {
	uint64_t address = 0;
};

struct BranchOp {
	uint64_t compare_addr = 0;
	uint64_t mask         = 0;
	uint64_t reference    = 0;
	uint32_t function     = 0;
	uint32_t reserved     = 0;
};

struct ReadCheckOp {
	uint64_t address = 0;
	uint64_t size    = 0;
	uint64_t hash    = 0;
};

struct StreamBeginOp {
	uint64_t submission = 0;
	uint64_t state      = 0;
	uint32_t verify     = 0;
	uint32_t reserved   = 0;
};

struct StreamEndOp {
	uint64_t submission = 0;
	uint64_t packets    = 0;
};

struct HandoffOp {
	uint64_t submission = 0;
};

struct LockstepReadOp {
	uint64_t address     = 0;
	uint64_t size        = 0;
	uint64_t destination = 0;
};

struct SkipSlotsOp {
	uint32_t count    = 0;
	uint32_t reserved = 0;
};

struct WriteBackImagesOp {
	uint64_t reserved = 0;
};

template <typename T>
inline constexpr bool IsPayload =
    std::is_trivially_copyable_v<T> && alignof(T) <= 8 && (sizeof(T) % 8) == 0 &&
    std::has_unique_object_representations_v<T>;

static_assert(IsPayload<DrawIndexOp> && IsPayload<DrawAutoOp> && IsPayload<DrawIndirectOp> &&
              IsPayload<DispatchDirectOp> && IsPayload<DispatchIndirectOp> &&
              IsPayload<EndOfPipeOp> && IsPayload<ReleaseMemOp> && IsPayload<EventWriteOp> &&
              IsPayload<WriteDataOp> && IsPayload<ReferenceClockOp> && IsPayload<DmaDataOp> &&
              IsPayload<LodStatsOp> && IsPayload<PssrOp> && IsPayload<FlipOp> &&
              IsPayload<WaitRegMemOp> && IsPayload<WaitFlipDoneOp> && IsPayload<DumpConstRamOp> &&
              IsPayload<PredicationOp> && IsPayload<CondExecOp> && IsPayload<BranchOp> &&
              IsPayload<ReadCheckOp> && IsPayload<StreamBeginOp> && IsPayload<StreamEndOp> &&
              IsPayload<HandoffOp> && IsPayload<LockstepReadOp> && IsPayload<SkipSlotsOp> &&
              IsPayload<WriteBackImagesOp>);

[[nodiscard]] uint32_t PayloadSize(OpKind kind) noexcept;

[[nodiscard]] constexpr uint32_t RecordSize(uint32_t payload_size, uint32_t data_size) noexcept {
	return static_cast<uint32_t>(sizeof(OpHeader)) + payload_size +
	       CommandStream::Align8(data_size);
}

void NormalizeForCompare(OpKind kind, void* payload) noexcept;

[[nodiscard]] uint64_t HashOp(OpKind kind, const void* payload, uint32_t payload_size,
                              const void* data, uint32_t data_size) noexcept;

struct OpView {
	const OpHeader* header  = nullptr;
	const uint8_t*  payload = nullptr;
	const uint8_t*  data    = nullptr;

	template <typename T>
	[[nodiscard]] const T& As() const noexcept {
		return *reinterpret_cast<const T*>(payload);
	}
	[[nodiscard]] OpKind   Kind() const noexcept { return header->kind; }
	[[nodiscard]] uint32_t DataSize() const noexcept { return header->data_size; }
};

// The resolver and concurrency tests use the same interpretation of snapshot ownership.
[[nodiscard]] inline uint32_t SnapshotIndex(const OpView& view) noexcept {
	switch (view.Kind()) {
		case OpKind::DrawIndex: {
			const auto& op = view.As<DrawIndexOp>();
			return (op.flags & DrawFlagSnapshot) ? op.snapshot : UINT32_MAX;
		}
		case OpKind::DrawAuto: {
			const auto& op = view.As<DrawAutoOp>();
			return (op.flags & DrawFlagSnapshot) ? op.snapshot : UINT32_MAX;
		}
		case OpKind::DrawIndirectMulti: {
			const auto& op = view.As<DrawIndirectOp>();
			return (op.flags & IndirectFlagSnapshot) ? op.snapshot : UINT32_MAX;
		}
		case OpKind::DispatchDirect: {
			const auto& op = view.As<DispatchDirectOp>();
			return (op.flags & DispatchFlagSnapshot) ? op.snapshot : UINT32_MAX;
		}
		case OpKind::DispatchIndirect: {
			const auto& op = view.As<DispatchIndirectOp>();
			return (op.flags & DispatchFlagSnapshot) ? op.snapshot : UINT32_MAX;
		}
		default: return UINT32_MAX;
	}
}

class OpStream {
public:
	explicit OpStream(uint64_t capacity);
	~OpStream();
	KYTY_CLASS_NO_COPY(OpStream);

	uint64_t Emit(OpKind kind, const void* payload, uint32_t payload_size, const void* data,
	              uint32_t data_size, uint64_t packet, uint64_t packets_hash, bool verify,
	              uint16_t flags = 0);

	[[nodiscard]] bool Peek(OpView& view);
	void               Pop();

	[[nodiscard]] uint64_t Emitted() const noexcept { return m_next_sequence; }
	[[nodiscard]] uint64_t Bytes() const noexcept { return m_bytes; }
	[[nodiscard]] uint32_t MaxRecord() const noexcept { return m_ring.MaxPacket(); }
	[[nodiscard]] const CommandStream::WaitStats& ProducerStats() const noexcept {
		return m_producer_stats;
	}
	void SetSpinNs(uint64_t spin_ns) noexcept { m_policy.spin_ns = spin_ns; }

private:
	CommandStream::Ring       m_ring;
	CommandStream::WaitPolicy m_policy;
	CommandStream::WaitStats  m_producer_stats;
	CommandStream::WaitStats  m_consumer_stats;
	std::atomic<uint64_t>     m_next_sequence {0};
	std::atomic<uint64_t>     m_bytes {0};
	uint32_t                  m_peeked_size = 0;
};

struct VerifyTotals {
	std::atomic<uint64_t> streams {0};
	std::atomic<uint64_t> checks {0};
	std::atomic<uint64_t> mismatches {0};
	std::atomic<uint64_t> read_divergences {0};
	std::atomic<uint64_t> lockstep_answers {0};
};
[[nodiscard]] VerifyTotals& GetVerifyTotals();

} // namespace Libs::Graphics::CpSeq

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPOPS_H_
