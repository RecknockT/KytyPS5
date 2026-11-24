#include "graphics/presentation/window/benchmarkReplay.h"

#include "common/auditProfiler.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"
#include "graphics/presentation/window/hostInput.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Libs::Graphics {

BenchmarkReplay::BenchmarkReplay() {
	const char* path = std::getenv("KYTY_BENCH_REPLAY");
	if (path == nullptr || *path == 0) return;
	m_requested = true;
	std::ifstream input(Common::PathFromUtf8(path));
	if (!input.is_open()) {
		std::fprintf(stderr, "BenchmarkReplay: cannot open %s\n", path);
		return;
	}
	auto script = nlohmann::json::parse(input, nullptr, false);
	if (!script.is_array() || script.empty() || script.size() > 1000) {
		std::fprintf(stderr, "BenchmarkReplay: invalid script %s\n", path);
		return;
	}
	uint64_t previous = 0;
	for (const auto& entry: script) {
		if (!entry.is_object() || !entry.contains("at_ms") ||
		    !entry["at_ms"].is_number_unsigned() || !entry.contains("action") ||
		    !entry["action"].is_string())
			return;
		Command command {};
		command.at_ms = entry["at_ms"].get<uint64_t>();
		if (command.at_ms < previous || command.at_ms > 3600000) return;
		previous          = command.at_ms;
		const auto action = entry["action"].get<std::string>();
		if (action == "down" || action == "up") {
			if (!entry.contains("key") || !entry["key"].is_string()) return;
			command.key = SDL_GetKeyFromName(entry["key"].get<std::string>().c_str());
			// Replay only the game's ordinary controls, never window/system shortcuts.
			if (command.key != SDLK_J && command.key != SDLK_L && command.key != SDLK_W &&
			    command.key != SDLK_A && command.key != SDLK_S && command.key != SDLK_D)
				return;
			command.action = action == "down" ? Action::Down : Action::Up;
		} else if (action == "compact_image_readback") {
			if (!entry.contains("enabled") || !entry["enabled"].is_boolean()) return;
			command.action = Action::CompactReadback;
			command.mode   = entry["enabled"].get<bool>();
		} else if (action == "read_buffer_reuse") {
			if (!entry.contains("enabled") || !entry["enabled"].is_boolean()) return;
			command.action = Action::ReadBuffer;
			command.mode   = entry["enabled"].get<bool>();
		} else if (action == "resource_prep_reuse") {
			if (!entry.contains("enabled") || !entry["enabled"].is_boolean()) return;
			command.action = Action::ResourcePrep;
			command.mode   = entry["enabled"].get<bool>();
		} else if (action == "binding_reuse") {
			if (!entry.contains("enabled") || !entry["enabled"].is_boolean()) return;
			command.action = Action::Mode;
			command.mode   = entry["enabled"].get<bool>();
		} else if (action == "submit_cadence") {
			if (!entry.contains("enabled") || !entry["enabled"].is_boolean()) return;
			command.action = Action::Cadence;
			command.mode   = entry["enabled"].get<bool>();
		} else if (action == "tile_layout_cache") {
			if (!entry.contains("enabled") || !entry["enabled"].is_boolean()) return;
			command.action = Action::TileLayout;
			command.mode   = entry["enabled"].get<bool>();
		} else if (action == "capture" || action == "wait_file") {
			if (!entry.contains("label") || !entry["label"].is_string()) return;
			command.label = entry["label"].get<std::string>();
			if (command.label.empty() || command.label.size() > 64 ||
			    !std::all_of(command.label.begin(), command.label.end(), [](unsigned char ch) {
				    return std::isalnum(ch) || ch == '_' || ch == '-';
			    }))
				return;
			command.action = action == "capture" ? Action::Capture : Action::WaitFile;
			if (entry.contains("confirm")) {
				if (command.action != Action::WaitFile || !entry["confirm"].is_boolean()) return;
				command.confirm = entry["confirm"].get<bool>();
			}
		} else if (action == "quit") {
			command.action = Action::Quit;
		} else
			return;
		m_commands.push_back(std::move(command));
	}
	const char* arm    = std::getenv("KYTY_BENCH_ARM_FILE");
	const bool  manual = arm != nullptr && *arm != 0;
	if (manual) {
		// Manual measurements must never drive the user's controls or close their game.
		if (std::any_of(m_commands.begin(), m_commands.end(), [](const Command& command) {
			    return command.action == Action::Down || command.action == Action::Up ||
			           command.action == Action::Quit || command.confirm;
		    }))
			return;
	} else if (m_commands.back().action != Action::Quit)
		return;
	m_export_directory =
	    std::filesystem::path(Common::AuditProfiler::GetExportPath()).parent_path().string();
	m_start_ms = SDL_GetTicks();
	if (manual) {
		m_arm_path = arm;
		m_armed    = false;
		std::printf("BenchmarkReplay: waiting for manual arm file; user controls gameplay\n");
	}
	m_enabled = true;
	std::printf("BenchmarkReplay: ready, %zu commands, host clock; %s\n", m_commands.size(), path);
	std::fflush(stdout);
}

BenchmarkReplay::~BenchmarkReplay() {
	for (const auto key: m_held_keys)
		HostInputKey(key, false);
}

int BenchmarkReplay::WaitLimit(int ordinary_wait) const {
	if (m_enabled && (!m_armed || m_waiting_file)) return std::min(ordinary_wait, 100);
	if (!m_enabled || m_next == m_commands.size()) return ordinary_wait;
	const auto elapsed = SDL_GetTicks() - m_start_ms;
	const auto due     = m_commands[m_next].at_ms;
	return due <= elapsed ? 0 : std::min(ordinary_wait, static_cast<int>(due - elapsed));
}

bool BenchmarkReplay::Update() {
	if (!m_enabled) return !m_requested;
	if (!m_armed) {
		std::error_code error;
		if (!std::filesystem::exists(Common::PathFromUtf8(m_arm_path), error)) return true;
		m_start_ms = SDL_GetTicks();
		m_armed    = true;
		std::printf("BenchmarkReplay: manual measurement armed, no game input\n");
		std::fflush(stdout);
	}
	const auto elapsed = SDL_GetTicks() - m_start_ms;
	while (m_next < m_commands.size() && m_commands[m_next].at_ms <= elapsed) {
		const auto& command = m_commands[m_next];
		if (command.action == Action::WaitFile) {
			if ((!m_held_keys.empty() && !m_gate_confirm_down) ||
			    Common::AuditProfiler::IsCaptureRunning())
				return false;
			std::error_code error;
			const auto      gate  = std::filesystem::path(m_export_directory) / command.label;
			const bool      ready = std::filesystem::exists(gate, error);
			const auto      now   = SDL_GetTicks();
			if (m_gate_confirm_down && (ready || now - m_gate_confirm_at >= 5000)) {
				HostInputKey(SDLK_J, false);
				std::erase(m_held_keys, SDLK_J);
				m_gate_confirm_down = false;
			} else if (!ready && command.confirm && !m_gate_confirm_down &&
			           now - m_gate_confirm_at >= 20000) {
				HostInputKey(SDLK_J, true);
				m_held_keys.push_back(SDLK_J);
				m_gate_confirm_down = true;
				m_gate_confirm_at   = now;
				std::printf("BenchmarkReplay: retry held X at load gate %s\n",
				            command.label.c_str());
				std::fflush(stdout);
			}
			if (!ready) {
				if (!m_waiting_file) {
					std::printf("BenchmarkReplay: waiting for load gate %s\n",
					            command.label.c_str());
					std::fflush(stdout);
				}
				m_waiting_file = true;
				// Freeze replay time at the gate; all later input/capture delays survive.
				m_start_ms += elapsed - command.at_ms;
				return true;
			}
			m_waiting_file = false;
			std::printf("BenchmarkReplay: load gate passed %s\n", command.label.c_str());
		}
		++m_next;
		std::printf("BenchmarkReplay: at %llu ms, command %zu\n",
		            static_cast<unsigned long long>(elapsed), m_next);
		std::fflush(stdout);
		switch (command.action) {
			case Action::Down:
				HostInputKey(command.key, true);
				if (std::find(m_held_keys.begin(), m_held_keys.end(), command.key) ==
				    m_held_keys.end())
					m_held_keys.push_back(command.key);
				break;
			case Action::Up:
				HostInputKey(command.key, false);
				std::erase(m_held_keys, command.key);
				break;
			case Action::Mode:
				if (Common::AuditProfiler::IsCaptureRunning()) return false;
				Config::RequestBindingReuseMode(command.mode);
				std::printf("BenchmarkReplay: binding_reuse requested=%d\n", int(command.mode));
				break;
			case Action::Cadence:
				if (Common::AuditProfiler::IsCaptureRunning()) return false;
				Config::RequestSubmitCadenceMode(command.mode);
				std::printf("BenchmarkReplay: submit_cadence requested=%d\n", int(command.mode));
				break;
			case Action::TileLayout:
				if (Common::AuditProfiler::IsCaptureRunning()) return false;
				Config::RequestTileLayoutCacheMode(command.mode);
				std::printf("BenchmarkReplay: tile_layout_cache requested=%d\n", int(command.mode));
				break;
			case Action::CompactReadback:
				if (Common::AuditProfiler::IsCaptureRunning()) return false;
				Config::RequestCompactImageReadbackMode(command.mode);
				break;
			case Action::ReadBuffer:
				if (Common::AuditProfiler::IsCaptureRunning()) return false;
				Config::RequestReadBufferReuseMode(command.mode);
				break;
			case Action::ResourcePrep:
				if (Common::AuditProfiler::IsCaptureRunning()) return false;
				Config::RequestResourcePrepReuseMode(command.mode);
				break;
			case Action::Capture: {
				if (!m_held_keys.empty() || Common::AuditProfiler::IsCaptureRunning()) return false;
				const auto output =
				    (std::filesystem::path(m_export_directory) / (command.label + ".json"))
				        .string();
				Common::AuditProfiler::SetExportPath(output);
				Common::AuditProfiler::StartCapture(command.label.c_str());
				break;
			}
			case Action::WaitFile: break;
			case Action::Quit:
				std::printf("BenchmarkReplay: completed\n");
				// Shutdown exports a fresh summary; keep the completed sample untouched.
				Common::AuditProfiler::SetExportPath(
				    (std::filesystem::path(m_export_directory) / "audit.json").string());
				return false;
		}
	}
	return true;
}

} // namespace Libs::Graphics
