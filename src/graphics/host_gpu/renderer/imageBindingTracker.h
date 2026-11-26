#pragma once

#include "common/slotVector.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {
// Tracks IDs to clear after an operation, independently of mutable ImageBinding flags.
// IDs retain their first occurrence order; reused slots are still cleared at the boundary.
class ImageBindingTracker {
public:
	static constexpr uint32_t MaxTrackedSlots = 1u << 20; // At most 4 MiB of generation stamps.
	bool                      Append(Common::SlotId id, bool deduplicate) {
		if (!id) return false;
		if (deduplicate && id.index < MaxTrackedSlots) {
			if (id.index >= m_generations.size()) {
				const size_t required = size_t(id.index) + 1;
				if (required > m_generations.capacity())
					m_generations.reserve(std::min<size_t>(
					    MaxTrackedSlots, std::max(required, 2 * m_generations.capacity())));
				m_generations.resize(required, 0);
			}
			if (m_generations[id.index] == m_generation) {
				++m_duplicate_skips;
				return false;
			}
			m_generations[id.index] = m_generation;
		}
		m_ids.push_back(id);
		return true;
	}
	void clear() {
		m_ids.clear();
		m_duplicate_skips = 0;
		if (++m_generation == 0) {
			std::fill(m_generations.begin(), m_generations.end(), 0);
			m_generation = 1;
		}
	}
	auto           begin() const { return m_ids.begin(); }
	auto           end() const { return m_ids.end(); }
	size_t         size() const { return m_ids.size(); }
	Common::SlotId operator[](size_t index) const { return m_ids[index]; }
	uint64_t       DuplicateSkips() const { return m_duplicate_skips; }
	size_t         TrackingBytes() const { return m_generations.capacity() * sizeof(uint32_t); }

private:
	friend struct ImageBindingTrackerTestAccess;
	std::vector<Common::SlotId> m_ids;
	std::vector<uint32_t>       m_generations;
	uint32_t                    m_generation      = 1;
	uint64_t                    m_duplicate_skips = 0;
};
} // namespace Libs::Graphics
