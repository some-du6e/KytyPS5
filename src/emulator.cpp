#include "emulator.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/presets.h"
#include "common/titleProfile.h"
#include "common/threads.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kytyGitVersion.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/additionalContent.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <cstdlib>
#include <filesystem>
#include <thread>

namespace Emulator {

static void PrintSystemInfo() {
	const Common::SystemInfo info = Common::GetSystemInfo();

#if defined(__APPLE__)
	static constexpr auto platform_name = "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static constexpr auto platform_name = "Windows";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static constexpr auto platform_name = "Linux";
#else
	static constexpr auto platform_name = "Unknown";
#endif

	LOGF("Build\n"
	     "  version: %s\n\n"
	     "Host\n"
	     "  os:      %s\n"
	     "  cpu:     %s\n"
	     "  threads: %u\n\n",
	     KYTY_BUILD_LABEL, platform_name, info.ProcessorName.c_str(),
	     std::thread::hardware_concurrency());
}

static void KytyClose() {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();

	LOGF("done!\n");

	Common::Subsystems::EmergencyShutdownActive();
}

static void MountOrCreateDir(const std::filesystem::path& dir, const std::string& point) {
	if (!Common::File::IsDirectoryExisting(dir)) {
		Common::File::CreateDirectories(dir);
	}

	EXIT_NOT_IMPLEMENTED(!Common::File::IsDirectoryExisting(dir));

	Libs::LibKernel::FileSystem::Mount(dir, point);
	auto dir_text = Common::PathToString(dir);
	LOGF("Mounted %s -> %s\n", point.c_str(), dir_text.c_str());
}

static void MountSandboxDirs() {
	// Only a TITLE_ID names the sandbox, as before the title resolver: the folders of a dump
	// without one stay where they were.
	const auto& title    = Loader::SystemContentTitle();
	std::string title_id = title.id_from_content_id || title.id.empty() ? "UNKNOWN" : title.id;

	MountOrCreateDir("_DownloadData/" + title_id, "/download0");
	MountOrCreateDir("_TempData/" + title_id, "/temp0");
	MountOrCreateDir("_TempData/" + title_id, "/temp");
}

// gta6-prep: config
// Logs the presets of the command line, matches the title to its profile (Common::MatchTitle),
// applies the profile's default preset where the user set nothing, and sets the quirks in effect.
// Returns the options in effect, which are loaded.
static Config::ConfigOptions ApplyTitleProfile(const RunOptions& options) {
	const auto& cfg = options.config;
	for (const auto& name: cfg.presets) {
		if (const auto* preset = Config::FindPreset(name); preset != nullptr) {
			Log::WriteToConsoleAndLog(
			    fmt::format("Preset: {} -> {}\n", name, Config::PresetTokenList(*preset)));
		}
	}
	const auto& title    = Loader::SystemContentTitle();
	const char* gta6_ids = std::getenv("KYTY_GTA6_TITLE_IDS");
	const auto  match =
	    Common::MatchTitle(title, cfg.title_profile, gta6_ids != nullptr ? gta6_ids : "");
	Config::ConfigOptions effective = cfg;
	std::string_view      preset_name;
	if (match.profile != nullptr && !match.profile->default_preset.empty()) {
		const auto* preset = Config::FindPreset(match.profile->default_preset);
		if (preset != nullptr && options.apply_title_preset != nullptr &&
		    options.apply_title_preset(effective, preset->name)) {
			preset_name = preset->name;
			Log::WriteToConsoleAndLog(
			    fmt::format("Preset: {} -> {}\n", preset->name, Config::PresetTokenList(*preset)));
			Config::Load(effective);
			// The param.json was loaded with the options before the preset.
			Loader::SystemContentRefreshTrinityMode();
		} else {
			effective = cfg;
			LOGF("Title profile: the default preset %.*s does not apply\n",
			     static_cast<int>(match.profile->default_preset.size()),
			     match.profile->default_preset.data());
		}
	}
	const auto quirks = Common::ResolveQuirks(match.profile, cfg.quirks_on, cfg.quirks_off);
	Config::SetTitleQuirks(quirks);
	Log::WriteToConsoleAndLog(
	    Common::FormatTitleProfileLine(title, match.profile, quirks, preset_name) + "\n");
	const auto reason = Common::TitleMatchReasonName(match.reason);
	LOGF("Title profile: chosen by %.*s\n", static_cast<int>(reason.size()), reason.data());
	return effective;
}

static void Init(const RunOptions& options, const std::filesystem::path& param_json,
                 Common::Subsystems& subsystems) {
	EXIT_IF(!Common::Thread::IsMainThread());

	subsystems.Initialize<Config::Lifecycle>();
	Config::Load(options.config);
	subsystems.Initialize<Log::Lifecycle>();

	if (Common::File::IsFileExisting(param_json)) {
		Loader::SystemContentLoadParamSfo(param_json);
	}
	// gta6-prep: config
	const auto cfg = ApplyTitleProfile(options);
	if (cfg.splash_enabled) {
		Libs::Graphics::InitializeSplashScreen(param_json.parent_path());
	}

	// Initialization order is explicit; destruction is automatic and reversed.
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
	subsystems.Initialize<Profiler::Lifecycle>();
	subsystems.Initialize<Libs::Network::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
	subsystems.Initialize<Libs::Controller::Lifecycle>();
	subsystems.Initialize<Libs::Audio::Lifecycle>();
	subsystems.Initialize<Libs::Graphics::Lifecycle>();
}

static void LoadElf(const std::filesystem::path& elf, bool dbg_print_reloc = false,
                    const std::filesystem::path& save_name = {}) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(
	    Libs::LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(elf)));

	if (dbg_print_reloc) {
		program->dbg_print_reloc = true;
	}

	if (!save_name.empty()) {
		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(
		                             Common::PathToGenericString(save_name)));
	}
}

static void Execute(const std::filesystem::path& game_patch) {
	std::thread([game_patch] {
		Common::Singleton<Loader::RuntimeLinker>::Instance()->Execute(game_patch);
	}).detach();
	Libs::Graphics::WindowRun();
	std::quick_exit(0);
}

void Run(const RunOptions& options) {
	if (options.app0_dir.empty()) {
		EXIT("app0 directory is required\n");
	}

	if (options.elf.empty()) {
		EXIT("ELF is required\n");
	}

	const auto         param_json = options.app0_dir / "sce_sys" / "param.json";
	Common::Subsystems subsystems(true);
	Init(options, param_json, subsystems);

	PrintSystemInfo();
	const std::string title_id = Loader::SystemContentTitle().id;
	if (!title_id.empty()) {
		Log::WriteToConsoleAndLog(fmt::format("Title ID: {}\n", title_id));
	}

	int ok = atexit(KytyClose);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	// Guest threads are still running, so skip KytyClose() and only flush emergency state.
	ok = at_quick_exit(Common::Subsystems::EmergencyShutdownActive);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/app0");
	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/hostapp");

	MountSandboxDirs();

	Loader::AddContentLoad(options.addcont_dir, options.addcont, title_id);
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitAll(rt->Symbols());

	LoadElf(options.elf);

	Execute(options.game_patch);
}

} // namespace Emulator
