#include "graphics/host_gpu/renderer/image/tileLayoutCache.h"

#include <algorithm>

namespace Libs::Graphics {
bool TileLayoutCache::Get(const TileSurfaceDescription& d, TileSurfaceLayout& out,
                          TileMipLayout* mips, bool* hit) {
	if (hit) *hit = false;
	const Key key {static_cast<uint32_t>(d.format),
	               static_cast<uint32_t>(d.tile_mode),
	               static_cast<uint32_t>(d.dimension),
	               d.width,
	               d.height,
	               d.depth,
	               d.levels,
	               d.layers};
	uint32_t  hash = 2166136261u;
	for (auto word: key) {
		hash ^= word;
		hash *= 16777619u;
		hash ^= hash >> 16;
	}
	const auto set     = hash & (Sets - 1);
	auto&      entries = m_entries[set];
	for (const auto& entry: entries) {
		if (entry.valid && entry.key == key) {
			out = entry.layout;
			std::copy_n(entry.mips.begin(), d.levels, mips);
			if (hit) *hit = true;
			return true;
		}
	}
	// Use the original routine even on invalid inputs, including its output semantics.
	if (!TileGetTiledTextureLayout(d, out, mips)) return false;
	auto& entry  = entries[!entries[0].valid ? 0 : !entries[1].valid ? 1 : m_next[set]];
	entry.key    = key;
	entry.layout = out;
	std::copy_n(mips, d.levels, entry.mips.begin());
	entry.valid = true;
	m_next[set] = static_cast<uint8_t>(&entry == &entries[0] ? 1 : 0);
	return true;
}
} // namespace Libs::Graphics
