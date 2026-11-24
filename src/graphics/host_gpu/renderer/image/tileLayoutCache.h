#pragma once
#include "graphics/guest_gpu/tile.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {
// Pure arithmetic memo: no guest bytes, image identities, mapping or dirty-state decisions.
// Callers own one cache per thread. Only successful layouts are retained.
class TileLayoutCache {
public:
	bool Get(const TileSurfaceDescription& description, TileSurfaceLayout& layout,
	         TileMipLayout* mips, bool* hit = nullptr);

private:
	static constexpr size_t Sets = 64, Ways = 2;
	using Key = std::array<uint32_t, 8>;
	struct Entry {
		Key                           key {};
		TileSurfaceLayout             layout;
		std::array<TileMipLayout, 16> mips {};
		bool                          valid = false;
	};
	std::array<std::array<Entry, Ways>, Sets> m_entries {};
	std::array<uint8_t, Sets>                 m_next {};
};
} // namespace Libs::Graphics
