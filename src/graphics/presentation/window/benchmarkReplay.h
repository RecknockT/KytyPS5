#ifndef KYTY_GRAPHICS_BENCHMARK_REPLAY_H_
#define KYTY_GRAPHICS_BENCHMARK_REPLAY_H_

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics {

// Opt-in, host-time input replay for reproducible benchmarks. Never changes guest timing.
class BenchmarkReplay {
public:
	BenchmarkReplay();
	~BenchmarkReplay();
	[[nodiscard]] bool Update(); // False requests an ordinary emulator shutdown.
	[[nodiscard]] int  WaitLimit(int ordinary_wait) const;

private:
	enum class Action {
		Down,
		Up,
		Mode,
		Cadence,
		TileLayout,
		ResourcePrep,
		ReadBuffer,
		CompactReadback,
		WaitFile,
		Capture,
		Quit
	};
	struct Command {
		uint64_t    at_ms;
		Action      action;
		int         key     = 0;
		bool        mode    = false;
		bool        confirm = false;
		std::string label;
	};
	std::vector<Command> m_commands;
	std::vector<int>     m_held_keys;
	std::string          m_export_directory;
	std::string          m_arm_path;
	bool                 m_armed             = true;
	bool                 m_waiting_file      = false;
	bool                 m_gate_confirm_down = false;
	uint64_t             m_gate_confirm_at   = 0;
	uint64_t             m_start_ms          = 0;
	size_t               m_next              = 0;
	bool                 m_enabled           = false;
	bool                 m_requested         = false;
};

} // namespace Libs::Graphics
#endif
