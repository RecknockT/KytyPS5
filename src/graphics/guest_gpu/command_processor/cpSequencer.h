#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPSEQUENCER_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPSEQUENCER_H_

#include "graphics/guest_gpu/command_processor/cpOps.h"
#include "graphics/guest_gpu/hardwareContext.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace Libs::Graphics::CpSeq {

class ISubmissionSequencer {
public:
	virtual ~ISubmissionSequencer()                              = default;
	virtual bool SequenceSubmission(const struct Intake& intake) = 0;
};

struct RegisterState {
	HW::Context    context;
	HW::UserConfig user_config;
	HW::Shader     shaders;
};

class SnapshotRing {
public:
	explicit SnapshotRing(uint32_t capacity): m_entries(capacity) {}

	[[nodiscard]] uint32_t Capacity() const noexcept {
		return static_cast<uint32_t>(m_entries.size());
	}
	[[nodiscard]] bool HasFree() const noexcept {
		return m_acquired - m_released.load(std::memory_order_seq_cst) < m_entries.size();
	}
	[[nodiscard]] uint32_t Acquire() noexcept {
		return static_cast<uint32_t>((m_acquired++) % m_entries.size());
	}
	[[nodiscard]] uint32_t AcquireBlocking(const std::function<bool()>& should_stop = nullptr) {
		uint32_t spins = 0;
		while (!HasFree()) {
			if (should_stop && should_stop()) {
				return UINT32_MAX;
			}
#if defined(_M_X64) || defined(__x86_64__)
			if (++spins > 64) {
				std::this_thread::yield();
				spins = 0;
			} else {
				_mm_pause();
			}
#else
			std::this_thread::yield();
#endif
		}
		return static_cast<uint32_t>((m_acquired++) % m_entries.size());
	}
	[[nodiscard]] RegisterState& Entry(uint32_t index) noexcept { return m_entries[index]; }
	void Release() noexcept { m_released.fetch_add(1, std::memory_order_seq_cst); }
	void Reset() noexcept {
		m_acquired = 0;
		m_released.store(0, std::memory_order_seq_cst);
	}

private:
	std::vector<RegisterState> m_entries;
	uint64_t                   m_acquired = 0;
	alignas(64) std::atomic<uint64_t> m_released {0};
};

struct Intake {
	uint64_t                  sequence    = 0;
	uint64_t                  frame_fence = 0;
	std::span<const uint32_t> commands;
	std::span<const uint32_t> constant_commands;
	bool                      graphics        = true;
	bool                      reset_processor = false;
	uint64_t                  flip_watermark  = UINT64_MAX;
};

class Sequencer {
public:
	explicit Sequencer(ISubmissionSequencer& processor);
	~Sequencer();
	Sequencer(const Sequencer&)            = delete;
	Sequencer& operator=(const Sequencer&) = delete;

	void Start();
	void Stop();

	void Admit(const Intake& intake);

	// ---- Resolver (GPU thread) ----
	void                        NoteStarted(uint64_t submission);
	void                        Answer(uint64_t op_sequence, uint64_t value);
	void                        NoteExecuted(uint64_t next_op);
	void                        NoteHandoffDone(uint64_t submission);
	[[nodiscard]] SnapshotRing& Snapshots() noexcept { return m_snapshots; }
	[[nodiscard]] uint32_t AcquireSnapshot(const HW::Context& ctx, const HW::UserConfig& ucfg,
	                                       const HW::Shader&            sh,
	                                       const std::function<bool()>& should_stop = nullptr) {
		const uint32_t idx = m_snapshots.AcquireBlocking(should_stop);
		if (idx != UINT32_MAX) {
			auto& entry       = m_snapshots.Entry(idx);
			entry.context     = ctx;
			entry.user_config = ucfg;
			entry.shaders     = sh;
		}
		return idx;
	}
	void ResetSnapshots() noexcept { m_snapshots.Reset(); }

	// ---- Sequencer thread (the processor's front) ----
	template <typename Ready>
	bool Wait(Ready&& ready, uint64_t wake_at = 0, uint64_t rewake_at = 0) {
		if (ready()) {
			return true;
		}
		return WaitSlow(ReadyRef(ready), wake_at, rewake_at);
	}
	[[nodiscard]] uint64_t AwaitAnswer(uint64_t op_sequence);
	[[nodiscard]] uint64_t Executed() const noexcept {
		return m_executed.load(std::memory_order_seq_cst);
	}
	[[nodiscard]] bool Answered(uint64_t op_sequence) const noexcept {
		return m_answered.load(std::memory_order_acquire) == op_sequence + 1u;
	}
	[[nodiscard]] uint64_t Started() const noexcept {
		return m_started.load(std::memory_order_seq_cst);
	}
	[[nodiscard]] uint64_t HandoffsDone() const noexcept {
		return m_handoff_done.load(std::memory_order_seq_cst);
	}
	[[nodiscard]] bool Stopping() const noexcept {
		return m_stopping.load(std::memory_order_acquire);
	}

private:
	class ReadyRef {
	public:
		template <typename Ready>
		explicit ReadyRef(Ready& ready)
		    : m_object(const_cast<void*>(static_cast<const void*>(&ready))),
		      m_call([](void* object) {
			      return static_cast<bool>((*static_cast<Ready*>(object))());
		      }) {}
		bool operator()() const { return m_call(m_object); }

	private:
		void* m_object;
		bool (*m_call)(void*);
	};
	bool WaitSlow(const ReadyRef& ready, uint64_t wake_at, uint64_t rewake_at);
	void Bump() noexcept;
	void Run();

	ISubmissionSequencer& m_processor;
	SnapshotRing          m_snapshots;
	uint64_t              m_spin_ns = 0;

	std::mutex              m_intake_mutex;
	std::condition_variable m_intake_ready;
	std::deque<Intake>      m_intake;
	bool                    m_stop = false;
	std::atomic<bool>       m_stopping {false};
	std::thread             m_thread;

	alignas(64) std::atomic<uint64_t> m_progress {0};
	std::atomic<uint32_t> m_parked {0};
	std::atomic<uint64_t> m_wake_at {0};
	std::atomic<uint64_t> m_executed {0};
	std::atomic<uint64_t> m_answered {0};
	uint64_t              m_answer_value = 0;
	std::atomic<uint64_t> m_started {0};
	std::atomic<uint64_t> m_handoff_done {0};
};

[[nodiscard]] uint32_t SnapshotCount();
[[nodiscard]] uint64_t SequencerSpinNs();
[[nodiscard]] uint64_t ResolverSpinNs();

} // namespace Libs::Graphics::CpSeq

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPSEQUENCER_H_
