#include "graphics/host_gpu/renderer/drawPrep/drawPrep.h"

#include "common/assert.h"
#include "common/auditProfiler.h"
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <chrono>
#include <cstring>

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

namespace Libs::Graphics::DrawPrep {

namespace {

void SpinWaitPause() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
	_mm_pause();
#else
	std::this_thread::yield();
#endif
}

uint64_t GetNowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

static bool ComputeDrawTopology(const HW::UserConfig&  ucfg,
                                vk::PrimitiveTopology& topology) noexcept {
	topology = vk::PrimitiveTopology::eTriangleList;
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone:
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) {
				return false;
			}
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: topology = vk::PrimitiveTopology::ePointList; break;
	}
	return true;
}

} // namespace

Engine::Engine(RenderContext& renderer, uint32_t num_workers)
    : m_renderer(renderer), m_mode(Config::DrawPrepEnabled() ? Mode::Parallel : Mode::Off),
      m_num_workers(std::clamp(num_workers, 1u, 16u)), m_window(64), m_window_mask(63) {
	if (m_mode == Mode::Parallel) {
		Start();
	}
}

Engine::~Engine() {
	Stop();
}

void Engine::Start() {
	if (m_mode != Mode::Parallel || !m_worker_threads.empty()) {
		return;
	}
	m_stopping.store(false, std::memory_order_seq_cst);
	m_worker_threads.reserve(m_num_workers);
	for (uint32_t i = 0; i < m_num_workers; ++i) {
		m_worker_threads.emplace_back([this, i] { WorkerLoop(i); });
	}
}

void Engine::Stop() {
	if (m_stopping.exchange(true, std::memory_order_seq_cst)) {
		return;
	}
	{
		std::lock_guard lock(m_queue_mutex);
		m_publish_cv.notify_all();
		m_ready_cv.notify_all();
	}
	for (auto& worker: m_worker_threads) {
		if (worker.joinable()) {
			worker.join();
		}
	}
	m_worker_threads.clear();
}

void Engine::SetMode(Mode mode) {
	if (m_mode == mode) {
		return;
	}
	if (mode == Mode::Parallel) {
		m_mode = Mode::Parallel;
		Start();
	} else {
		Stop();
		m_mode = mode;
	}
}

bool Engine::WindowHasSpace() const noexcept {
	const auto head = m_head.load(std::memory_order_acquire);
	const auto tail = m_tail.load(std::memory_order_relaxed);
	return (tail - head) < m_window.size();
}

uint64_t Engine::Publish(const DrawIndexArgs* index_args, const DrawAutoArgs* auto_args,
                         const HW::Context& context, const HW::UserConfig& user_config,
                         const HW::Shader& shaders, const std::function<bool()>& wait_for_space) {
	if (m_mode == Mode::Off) {
		return UINT64_MAX;
	}

	while (!WindowHasSpace()) {
		if (wait_for_space && !wait_for_space()) {
			return UINT64_MAX;
		}
		SpinWaitPause();
		if (m_stopping.load(std::memory_order_relaxed)) {
			return UINT64_MAX;
		}
	}

	const uint64_t pos      = m_tail.fetch_add(1, std::memory_order_acq_rel);
	const uint32_t slot_idx = static_cast<uint32_t>(pos & m_window_mask);
	auto&          slot     = m_window[slot_idx];

	while (slot.state.load(std::memory_order_acquire) != SlotState::Free) {
		SpinWaitPause();
	}

	slot.position             = pos;
	slot.snapshot.context     = context;
	slot.snapshot.user_config = user_config;
	slot.snapshot.shaders     = shaders;
	slot.prepared.valid       = false;

	if (index_args != nullptr) {
		slot.kind       = CpSeq::OpKind::DrawIndex;
		slot.index_args = *index_args;
	} else if (auto_args != nullptr) {
		slot.kind      = CpSeq::OpKind::DrawAuto;
		slot.auto_args = *auto_args;
	}

	slot.state.store(SlotState::Published, std::memory_order_release);
	m_stat_published.fetch_add(1, std::memory_order_relaxed);
	Common::AuditProfiler::RecordDrawPrepPublished();

	m_publish_cv.notify_all();
	return pos;
}

void Engine::WorkerLoop(uint32_t worker_id) {
#if defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

	while (!m_stopping.load(std::memory_order_acquire)) {
		uint64_t claim_pos = UINT64_MAX;
		uint32_t slot_idx  = 0;

		{
			std::unique_lock lock(m_queue_mutex);
			m_publish_cv.wait(lock, [this] {
				if (m_stopping.load(std::memory_order_relaxed)) {
					return true;
				}
				const auto head = m_head.load(std::memory_order_acquire);
				const auto tail = m_tail.load(std::memory_order_acquire);
				for (auto p = head; p < tail; ++p) {
					const auto idx = static_cast<uint32_t>(p & m_window_mask);
					if (m_window[idx].state.load(std::memory_order_relaxed) ==
					    SlotState::Published) {
						return true;
					}
				}
				return false;
			});

			if (m_stopping.load(std::memory_order_relaxed)) {
				return;
			}

			const auto head = m_head.load(std::memory_order_acquire);
			const auto tail = m_tail.load(std::memory_order_acquire);
			for (auto p = head; p < tail; ++p) {
				const auto idx      = static_cast<uint32_t>(p & m_window_mask);
				SlotState  expected = SlotState::Published;
				if (m_window[idx].state.compare_exchange_strong(expected, SlotState::Claimed,
				                                                std::memory_order_acq_rel)) {
					claim_pos = p;
					slot_idx  = idx;
					break;
				}
			}
		}

		if (claim_pos != UINT64_MAX) {
			PrepareSlot(m_window[slot_idx], worker_id);
		}
	}
}

void Engine::PrepareSlot(Slot& slot, uint32_t /*worker_id*/) {
	const auto t_start = GetNowNs();

	const auto& ctx    = slot.snapshot.context;
	const auto& sh_ctx = slot.snapshot.shaders;
	const auto& ucfg   = slot.snapshot.user_config;

	// Invalidation guard: calculate target signature from active color & depth masks
	slot.prepared.target_signature =
	    static_cast<uint64_t>(ctx.GetRenderTargetMask()) |
	    (static_cast<uint64_t>(ctx.GetShaderRegisters().m_cbShaderMask) << 32u);

	slot.prepared.snapshot = slot.snapshot;

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!ComputeDrawTopology(ucfg, topology)) {
		slot.prepared.valid = false;
		slot.state.store(SlotState::Ready, std::memory_order_release);
		m_ready_cv.notify_all();
		return;
	}
	slot.prepared.render_state.topology = topology;

	// Check eligibility: valid vertex shader code address (ES, LS, or GS)
	const auto vs_addr   = sh_ctx.GetVs().es_regs.data_addr;
	const bool vs_active = (vs_addr != 0 || sh_ctx.GetVs().ls_regs.data_addr != 0 ||
	                        sh_ctx.GetVs().gs_regs.data_addr != 0);

	if (vs_active) {
		slot.prepared.valid        = true;
		slot.prepared.pixel_active = (sh_ctx.GetPs().ps_regs.data_addr != 0);

		// Pre-populate DrawRenderState properties from snapshot
		auto& state                      = slot.prepared.render_state;
		state.ps_active                  = slot.prepared.pixel_active;
		state.mrt_mask                   = ctx.GetRenderTargetMask();
		state.vertex_info[0].buffers_num = static_cast<uint32_t>(ucfg.GetPrimType()) == 0 ? 0 : 1;

		const bool tess_active = ucfg.GetPrimType() == Prospero::PrimitiveType::kPatch;
		if (!tess_active && vs_addr != 0 && ShaderHasMappedData(vs_addr)) {
			// Real CPU parallel decoding: extract shader params, ELF parsing, vertex layout
			slot.prepared.vertex_params =
			    PrepareProgram(sh_ctx.GetVs(), ctx, ucfg, state.vertex_info[0]);
			slot.prepared.has_programs = true;
		}
	} else {
		slot.prepared.valid = false;
	}

	const auto t_end           = GetNowNs();
	slot.prepared.prep_time_ns = (t_end >= t_start) ? (t_end - t_start) : 0;
	m_stat_useful_ns.fetch_add(slot.prepared.prep_time_ns, std::memory_order_relaxed);

	if (Common::AuditProfiler::IsEnabled()) {
		Common::AuditProfiler::RecordPipelineStage(
		    Common::AuditProfiler::PipelineStage::DrawPrepWorker, slot.prepared.prep_time_ns);
	}

	m_stat_prepared.fetch_add(1, std::memory_order_relaxed);
	Common::AuditProfiler::RecordDrawPrepPrepared();

	slot.state.store(SlotState::Ready, std::memory_order_release);
	m_ready_cv.notify_all();
}

bool Engine::CommitPublished(uint64_t position, PreparedDraw& out_prepared) {
	if (m_mode == Mode::Off) {
		return false;
	}

	const auto     t_wait_start = GetNowNs();
	const uint32_t slot_idx     = static_cast<uint32_t>(position & m_window_mask);
	auto&          slot         = m_window[slot_idx];

	// Bounded spin wait for slot readiness (up to 5 us to prevent thread starvation)
	constexpr uint64_t max_spin_ns = 5'000;
	bool               ready       = false;

	while (GetNowNs() - t_wait_start < max_spin_ns) {
		const auto s = slot.state.load(std::memory_order_acquire);
		if (s == SlotState::Ready) {
			ready = true;
			break;
		}
		SpinWaitPause();
	}

	if (!ready) {
		std::unique_lock lock(m_queue_mutex);
		m_ready_cv.wait(lock, [&] {
			return slot.state.load(std::memory_order_acquire) == SlotState::Ready ||
			       m_stopping.load(std::memory_order_relaxed);
		});
		ready = (slot.state.load(std::memory_order_acquire) == SlotState::Ready);
	}

	const auto t_wait_end = GetNowNs();
	if (Common::AuditProfiler::IsEnabled() && t_wait_end > t_wait_start) {
		Common::AuditProfiler::RecordPipelineStage(
		    Common::AuditProfiler::PipelineStage::DrawPrepCommitWait, t_wait_end - t_wait_start);
	}

	// Always transfer snapshot to caller so registers are isolated even during fallback
	out_prepared.snapshot = slot.snapshot;

	bool success = false;
	if (ready && slot.prepared.valid) {
		out_prepared = slot.prepared;
		success      = true;
		m_stat_committed.fetch_add(1, std::memory_order_relaxed);
		Common::AuditProfiler::RecordDrawPrepCommitted();
	} else {
		m_stat_fallback.fetch_add(1, std::memory_order_relaxed);
		Common::AuditProfiler::RecordDrawPrepFallback();
	}

	slot.state.store(SlotState::Free, std::memory_order_release);

	auto head = m_head.load(std::memory_order_acquire);
	while (head < m_tail.load(std::memory_order_acquire)) {
		const auto idx = static_cast<uint32_t>(head & m_window_mask);
		if (m_window[idx].state.load(std::memory_order_acquire) == SlotState::Free) {
			if (m_head.compare_exchange_strong(head, head + 1, std::memory_order_acq_rel)) {
				// Advanced head by 1, recheck next slot
				head = head + 1;
			}
		} else {
			break;
		}
	}
	return success;
}

void Engine::Drain() {
	if (m_mode == Mode::Off) {
		return;
	}
	while (m_head.load(std::memory_order_acquire) < m_tail.load(std::memory_order_acquire)) {
		PreparedDraw dummy;
		(void)CommitPublished(m_head.load(std::memory_order_acquire), dummy);
	}
}

Engine::Stats Engine::GetStats() const noexcept {
	return {
	    .published      = m_stat_published.load(std::memory_order_relaxed),
	    .prepared       = m_stat_prepared.load(std::memory_order_relaxed),
	    .committed      = m_stat_committed.load(std::memory_order_relaxed),
	    .fallback       = m_stat_fallback.load(std::memory_order_relaxed),
	    .useful_time_ns = m_stat_useful_ns.load(std::memory_order_relaxed),
	};
}

} // namespace Libs::Graphics::DrawPrep
