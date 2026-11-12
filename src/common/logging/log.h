#ifndef KYTY_COMMON_LOGGING_LOG_H_
#define KYTY_COMMON_LOGGING_LOG_H_

#include "common/common.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/printf.h>
#include <string>
#include <string_view>

namespace Log {

void Initialize();
void Flush();

struct Lifecycle {
	static constexpr const char* name               = "Log";
	static constexpr auto        initialize         = Log::Initialize;
	static constexpr auto        shutdown           = Log::Flush;
	static constexpr auto        emergency_shutdown = Log::Flush;
};

// Set by LoadExec for the program it starts, which continues the log file of the one it replaces
// instead of starting it over.
inline constexpr const char* RelaunchedVariable = "KYTY_RELAUNCHED";

bool IsSilent();
void Write(fmt::text_style style, std::string_view text);
// Also shown on the console when the log goes to a file or nowhere, and kept in the session log.
void WriteToConsoleAndLog(std::string_view text);
// The session log (--session-log): the console messages above and the reports of a fatal error,
// whatever the log direction and without the printf log. Buffered: Flush() writes it out.
void WriteToSessionLog(std::string_view text);

// gta6-prep: diag
// --session-log auto: opens <executable folder>/_Logs/<title>_<yyyy-MM-dd_HH-mm-ss>.log once the
// title is known (Emulator::Init, after param.json), keeping the SESSION_LOGS_KEPT newest logs of
// the folder. A program a LoadExec starts continues the same file. Does nothing without the
// option or when a session log is open.
inline constexpr size_t SESSION_LOGS_KEPT = 20;
void OpenAutoSessionLog(std::string_view title_id);
// "<title>_<yyyy-MM-dd_HH-mm-ss>.log"; characters other than A-Z a-z 0-9 _ . - become '_', and an
// empty title is "unknown".
[[nodiscard]] std::string SessionLogFileName(std::string_view title_id, const std::tm& time);
// Whether `file_name` has the shape SessionLogFileName() gives, with an optional "_<n>" copy
// number before ".log": "<title>_<yyyy-MM-dd_HH-mm-ss>[_<n>].log". Only those are pruned.
[[nodiscard]] bool IsSessionLogFileName(std::string_view file_name);
// A session log modified this recently may belong to a session that is still running (another
// emulator instance, a launcher session): never pruned.
inline constexpr auto SESSION_LOG_MIN_PRUNE_AGE = std::chrono::minutes(10);
// Deletes the oldest session logs of `folder` (IsSessionLogFileName, by modification time) until at
// most `keep` remain, skipping those modified in the last SESSION_LOG_MIN_PRUNE_AGE. Other files,
// such as a user's own *.log, are never touched.
void PruneSessionLogs(const std::filesystem::path& folder, size_t keep);

namespace Color {

inline constexpr auto Default       = fmt::text_style {};
inline constexpr auto Red           = fmt::fg(fmt::terminal_color::red);
inline constexpr auto Green         = fmt::fg(fmt::terminal_color::green);
inline constexpr auto Yellow        = fmt::fg(fmt::terminal_color::yellow);
inline constexpr auto Cyan          = fmt::fg(fmt::terminal_color::cyan);
inline constexpr auto White         = fmt::fg(fmt::terminal_color::white);
inline constexpr auto BrightRed     = fmt::fg(fmt::terminal_color::bright_red);
inline constexpr auto BrightYellow  = fmt::fg(fmt::terminal_color::bright_yellow);
inline constexpr auto BrightMagenta = fmt::fg(fmt::terminal_color::bright_magenta);
inline constexpr auto BrightWhite   = fmt::fg(fmt::terminal_color::bright_white);

} // namespace Color

// Out of line, so callers on small guest stacks (fibers) do not reserve the formatting frame.
template <typename... Args>
[[gnu::noinline]] void Printf(fmt::text_style style, const char* format, const Args&... args) {
	Write(style, fmt::sprintf(format, args...));
}

} // namespace Log

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF(...)                                                                                  \
	do {                                                                                           \
		if (!::Log::IsSilent()) {                                                                  \
			::Log::Printf({}, __VA_ARGS__);                                                        \
		}                                                                                          \
	} while (false)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		if (!::Log::IsSilent()) {                                                                  \
			::Log::Printf((style), __VA_ARGS__);                                                   \
		}                                                                                          \
	} while (false)

#endif /* KYTY_COMMON_LOGGING_LOG_H_ */
