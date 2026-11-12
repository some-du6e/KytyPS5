#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "common/additionalContent.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <filesystem>
#include <string_view>

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions                config;
	std::filesystem::path                app0_dir;
	std::filesystem::path                elf;
	std::filesystem::path                game_patch;
	std::filesystem::path                addcont_dir;
	std::vector<Common::AddContentEntry> addcont;
	// gta6-prep: config
	// Applies the named preset to the options the user did not set, as the command line would
	// (main.cpp); false when it does not apply. Null: title profiles apply no preset.
	bool (*apply_title_preset)(Config::ConfigOptions& config, std::string_view preset) = nullptr;
};

void Run(const RunOptions& options);
int LoadExec(const char* path, const char* const argv[]);
// In a process LoadExec started: waits until the process that started it has exited.
void WaitForLoadExecParent();

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
