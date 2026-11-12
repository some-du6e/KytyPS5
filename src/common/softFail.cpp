#include "common/softFail.h"

#include "common/logging/log.h"

#include <cinttypes>
#include <cstdio>

namespace Common::SoftFail {

std::string FormatLine(std::string_view api, std::string_view message) {
	std::string line = "Soft-fail: ";
	line += api;
	line += ": ";
	line += message;
	line += "\n";
	return line;
}

std::string FormatParamSizeMessage(std::string_view what, uint64_t actual, uint64_t expected) {
	const auto check = CheckParamSize(actual, expected);
	if (check == ParamSize::Exact) {
		return {};
	}
	char text[96] = {};
	std::snprintf(text, sizeof(text), " size 0x%" PRIx64 " %s than 0x%" PRIx64 "%s", actual,
	              check == ParamSize::Larger ? "larger" : "smaller", expected,
	              check == ParamSize::Larger ? ", ignoring tail" : "");
	return std::string(what) + text;
}

void Report(std::string_view api, std::string_view message) {
	Log::WriteToConsoleAndLog(FormatLine(api, message));
}

} // namespace Common::SoftFail
