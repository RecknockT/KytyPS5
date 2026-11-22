#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/auditProfiler.h"
#include "common/debugCounters.h"
#include "graphics/host_gpu/graphicContext.h"

#include <chrono>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	m_graphics.device.destroySemaphore(m_semaphore, nullptr);
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick, Common::AuditProfiler::GpuWaitReason reason) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::GpuWaits,
	                                         Common::DebugCounters::Counter::GpuWaitNs);
	const auto                         start = std::chrono::steady_clock::now();
	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	const auto end    = std::chrono::steady_clock::now();
	if (result != vk::Result::eSuccess) {
		LOGF("masterSemaphore waitSemaphores failed: %s (%d) for tick %" PRIu64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), tick);
		EXIT("masterSemaphore waitSemaphores failed: %s (%d)\n", vk::to_string(result).c_str(),
		     static_cast<int>(result));
	}
	if (Common::AuditProfiler::IsEnabled()) {
		const auto wait_ns = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
		Common::AuditProfiler::RecordGpuWait(reason, wait_ns);
		Common::AuditProfiler::RecordPipelineStage(Common::AuditProfiler::PipelineStage::SyncWaits,
		                                           wait_ns);
		auto trace_type = Common::AuditProfiler::TraceEventType::TimelineWait;
		if (reason == Common::AuditProfiler::GpuWaitReason::StreamBufferWait) {
			trace_type = Common::AuditProfiler::TraceEventType::StreamBufferWait;
		} else if (reason == Common::AuditProfiler::GpuWaitReason::BufferReadback) {
			trace_type = Common::AuditProfiler::TraceEventType::BufferReadback;
		} else if (reason == Common::AuditProfiler::GpuWaitReason::PriorityOperation) {
			trace_type = Common::AuditProfiler::TraceEventType::PriorityOperation;
		}
		Common::AuditProfiler::RecordTraceEvent(trace_type, wait_ns, tick,
		                                        static_cast<uint64_t>(reason));
	}
	Refresh();
}

} // namespace Libs::Graphics
