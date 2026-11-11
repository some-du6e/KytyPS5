#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "common/additionalContent.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <filesystem>

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions                config;
	std::filesystem::path                app0_dir;
	std::filesystem::path                elf;
	std::filesystem::path                game_patch;
	std::filesystem::path                addcont_dir;
	std::vector<Common::AddContentEntry> addcont;
};

void Run(const RunOptions& options);
// gta6-prep: hle. --dump-imports: loads options.elf and every /app0/sce_module/*.prx, writes the
// import report (Loader::ImportDiagnostics::FormatImportReport) to the --dump-imports file and
// returns the process exit code. Initializes only what loading needs: no GPU, audio, input or
// network, and no guest code runs.
int DumpImports(const RunOptions& options);
int LoadExec(const char* path, const char* const argv[]);
// In a process LoadExec started: waits until the process that started it has exited.
void WaitForLoadExecParent();

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
