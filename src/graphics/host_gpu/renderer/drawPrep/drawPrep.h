#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_DRAWPREP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_DRAWPREP_H_

#include "graphics/guest_gpu/command_processor/cpOps.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/shader/shaderCompiler.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace Libs::Graphics::DrawPrep {

enum class Mode : uint8_t { Off, Inline, Parallel };

struct RegisterSnapshot {
	HW::Context    context;
	HW::UserConfig user_config;
	HW::Shader     shaders;
};

enum class SlotState : uint8_t { Free = 0, Published, Claimed, Ready, Retired };

struct PreparedDraw {
	bool                     valid            = false;
	uint64_t                 target_signature = 0;
	DrawRenderState          render_state;
	PipelineCache::Pipeline* pipeline     = nullptr;
	uint64_t                 prep_time_ns = 0;
	bool                     pixel_active = false;
	RegisterSnapshot         snapshot;
	bool                     has_programs = false;
	ShaderParams             vertex_params {};
};

struct Slot {
	std::atomic<SlotState> state {SlotState::Free};
	uint64_t               position = 0;
	CpSeq::OpKind          kind     = CpSeq::OpKind::DrawIndex;
	DrawIndexArgs          index_args {};
	DrawAutoArgs           auto_args {};
	RegisterSnapshot       snapshot;
	PreparedDraw           prepared;
};

class Engine {
public:
	explicit Engine(RenderContext& renderer, uint32_t num_workers = 4);
	~Engine();
	Engine(const Engine&)            = delete;
	Engine& operator=(const Engine&) = delete;

	void Start();
	void Stop();

	[[nodiscard]] bool IsActive() const noexcept { return m_mode != Mode::Off; }
	[[nodiscard]] Mode GetMode() const noexcept { return m_mode; }
	void               SetMode(Mode mode);

	// Window management
	[[nodiscard]] bool     WindowHasSpace() const noexcept;
	[[nodiscard]] uint64_t Publish(const DrawIndexArgs* index_args, const DrawAutoArgs* auto_args,
	                               const HW::Context& context, const HW::UserConfig& user_config,
	                               const HW::Shader&            shaders,
	                               const std::function<bool()>& wait_for_space);

	// Resolver execution
	[[nodiscard]] bool     CommitPublished(uint64_t position, PreparedDraw& out_prepared,
	                                       const std::function<void()>& service_pending = {});
	void                   Drain();
	[[nodiscard]] uint32_t GetQueueDepth() const noexcept {
		const auto head = m_head.load(std::memory_order_acquire);
		const auto tail = m_tail.load(std::memory_order_relaxed);
		return (tail >= head) ? static_cast<uint32_t>(tail - head) : 0u;
	}

	// Telemetry totals
	struct Stats {
		uint64_t published      = 0;
		uint64_t prepared       = 0;
		uint64_t committed      = 0;
		uint64_t fallback       = 0;
		uint64_t useful_time_ns = 0;
	};
	[[nodiscard]] Stats GetStats() const noexcept;

private:
	void Trace(const char* phase, uint64_t position) const;
	void WorkerLoop(uint32_t worker_id);
	void PrepareSlot(Slot& slot, uint32_t worker_id);

	[[maybe_unused]] RenderContext& m_renderer;
	Mode                            m_mode;
	uint32_t                        m_num_workers;
	std::vector<Slot>               m_window;
	uint32_t                        m_window_mask;

	alignas(64) std::atomic<uint64_t> m_head {0};
	alignas(64) std::atomic<uint64_t> m_tail {0};

	std::mutex              m_queue_mutex;
	std::condition_variable m_publish_cv;
	std::condition_variable m_ready_cv;

	std::atomic<bool>        m_stopping {false};
	std::vector<std::thread> m_worker_threads;

	std::atomic<uint64_t> m_stat_published {0};
	std::atomic<uint64_t> m_stat_prepared {0};
	std::atomic<uint64_t> m_stat_committed {0};
	std::atomic<uint64_t> m_stat_fallback {0};
	std::atomic<uint64_t> m_stat_useful_ns {0};
};

} // namespace Libs::Graphics::DrawPrep

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_DRAWPREP_H_
