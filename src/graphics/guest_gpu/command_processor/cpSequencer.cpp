#include "graphics/guest_gpu/command_processor/cpSequencer.h"

#include "common/assert.h"
#include "common/auditProfiler.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace Libs::Graphics::CpSeq {

namespace {

uint64_t EnvUnsigned(const char* name, uint64_t fallback, uint64_t low, uint64_t high) {
	const auto* value = std::getenv(name);
	if (value == nullptr || *value == '\0') {
		return fallback;
	}
	const auto parsed = std::strtoull(value, nullptr, 10);
	return parsed < low ? low : (parsed > high ? high : parsed);
}

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

void Relax() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
	_mm_pause();
#else
	std::this_thread::yield();
#endif
}

} // namespace

uint32_t SnapshotCount() {
	static const auto count =
	    static_cast<uint32_t>(EnvUnsigned("KYTY_CP_SEQ_SNAPSHOTS", 256, 4, 4096));
	return count;
}

uint64_t SequencerSpinNs() {
	static const auto ns = EnvUnsigned("KYTY_CP_SEQ_SPIN_US", 200, 0, 1000000) * 1000u;
	return ns;
}

uint64_t ResolverSpinNs() {
	static const auto ns = EnvUnsigned("KYTY_CP_SEQ_RESOLVER_SPIN_US", 50, 0, 1000000) * 1000u;
	return ns;
}

Sequencer::Sequencer(ISubmissionSequencer& processor)
    : m_processor(processor), m_snapshots(SnapshotCount()), m_spin_ns(SequencerSpinNs()) {}

Sequencer::~Sequencer() {
	Stop();
}

void Sequencer::Start() {
	EXIT_IF(m_thread.joinable());
	m_thread = std::thread([this] { Run(); });
}

void Sequencer::Stop() {
	{
		std::lock_guard lock(m_intake_mutex);
		m_stop = true;
	}
	m_stopping.store(true, std::memory_order_seq_cst);
	m_intake_ready.notify_all();
	Bump();
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void Sequencer::Admit(const Intake& intake) {
	{
		std::lock_guard lock(m_intake_mutex);
		m_intake.push_back(intake);
	}
	m_intake_ready.notify_one();
}

void Sequencer::Bump() noexcept {
	m_progress.fetch_add(1, std::memory_order_seq_cst);
	if (m_parked.load(std::memory_order_seq_cst) != 0) {
		m_progress.notify_all();
	}
}

void Sequencer::NoteStarted(uint64_t submission) {
	m_started.store(submission, std::memory_order_seq_cst);
	Bump();
}

void Sequencer::Answer(uint64_t op_sequence, uint64_t value) {
	m_answer_value = value;
	m_answered.store(op_sequence + 1u, std::memory_order_seq_cst);
	Bump();
}

void Sequencer::NoteExecuted(uint64_t next_op) {
	m_executed.store(next_op, std::memory_order_seq_cst);
	if (m_parked.load(std::memory_order_seq_cst) != 0 &&
	    next_op >= m_wake_at.load(std::memory_order_seq_cst)) {
		Bump();
	}
}

void Sequencer::NoteHandoffDone(uint64_t submission) {
	m_handoff_done.store(submission, std::memory_order_seq_cst);
	Bump();
}

bool Sequencer::WaitSlow(const ReadyRef& ready, uint64_t wake_at, uint64_t rewake_at) {
	const auto start_total = NowNs();
	auto       start       = start_total;
	const auto spin_ns     = m_spin_ns;

	for (uint32_t spins = 0;; spins++) {
		if (ready()) {
			if (Common::AuditProfiler::IsEnabled()) {
				Common::AuditProfiler::RecordPipelineStage(
				    Common::AuditProfiler::PipelineStage::SequencerWait, NowNs() - start_total);
			}
			return true;
		}
		if (Stopping()) {
			return false;
		}
		if ((spins & 63u) != 63u || NowNs() - start < spin_ns) {
			Relax();
			continue;
		}
		m_wake_at.store(wake_at, std::memory_order_seq_cst);
		const auto observed = m_progress.load(std::memory_order_seq_cst);
		m_parked.fetch_add(1, std::memory_order_seq_cst);
		if (!ready() && !Stopping()) {
			m_progress.wait(observed, std::memory_order_seq_cst);
		}
		m_parked.fetch_sub(1, std::memory_order_seq_cst);
		wake_at = rewake_at;
		start   = NowNs();
	}
}

uint64_t Sequencer::AwaitAnswer(uint64_t op_sequence) {
	constexpr uint64_t PreWakeOps = 8;
	(void)Wait(
	    [this, op_sequence] {
		    return m_answered.load(std::memory_order_seq_cst) == op_sequence + 1u;
	    },
	    op_sequence > PreWakeOps ? op_sequence - PreWakeOps : 0u, UINT64_MAX);
	return m_answer_value;
}

void Sequencer::Run() {
#if defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
	for (;;) {
		Intake intake;
		{
			std::unique_lock lock(m_intake_mutex);
			m_intake_ready.wait(lock, [this] { return m_stop || !m_intake.empty(); });
			if (m_intake.empty()) {
				return;
			}
			intake = m_intake.front();
			m_intake.pop_front();
		}
		const bool running = m_processor.SequenceSubmission(intake);
		if (!running) {
			return;
		}
	}
}

} // namespace Libs::Graphics::CpSeq
