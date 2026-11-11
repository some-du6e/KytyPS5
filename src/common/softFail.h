#ifndef KYTY_COMMON_SOFT_FAIL_H_
#define KYTY_COMMON_SOFT_FAIL_H_

// Soft failures: HLE paths that used to EXIT on an input a newer SDK may pass (a larger param
// struct, an unknown ID) and now return an error code or a plausible value instead. Each call site
// writes one "Soft-fail: <api>: <message>" line (Log::WriteToConsoleAndLog) the first time it
// fails, so a run that survives still shows what it skipped.

#include <atomic>
#include <cstdint>
#include <fmt/printf.h>
#include <string>
#include <string_view>

namespace Common::SoftFail {

enum class ParamSize { Exact, Larger, Smaller };

constexpr ParamSize CheckParamSize(uint64_t actual, uint64_t expected) {
	return actual == expected  ? ParamSize::Exact
	       : actual > expected ? ParamSize::Larger
	                           : ParamSize::Smaller;
}

// "Soft-fail: <api>: <message>\n"
std::string FormatLine(std::string_view api, std::string_view message);
// "<what> size 0x<actual> larger than 0x<expected>, ignoring tail" or
// "<what> size 0x<actual> smaller than 0x<expected>"; empty for an exact size.
std::string FormatParamSizeMessage(std::string_view what, uint64_t actual, uint64_t expected);

// Writes FormatLine(api, message) with Log::WriteToConsoleAndLog.
void Report(std::string_view api, std::string_view message);

// Reports once per `once` flag. Returns whether this call reported.
inline bool ReportOnce(std::atomic_bool& once, std::string_view api, std::string_view message) {
	if (once.exchange(true, std::memory_order_relaxed)) {
		return false;
	}
	Report(api, message);
	return true;
}

// A guest struct with a size field: accepts the size the HLE code knows and any larger one (newer
// SDKs append fields; only the known prefix is read). Reports a larger or smaller size once per
// `once` flag. Returns false only for a smaller size, which the caller turns into the library's
// invalid-argument error.
inline bool AcceptParamSize(std::atomic_bool& once, std::string_view api, std::string_view what,
                            uint64_t actual, uint64_t expected) {
	const auto check = CheckParamSize(actual, expected);
	if (check != ParamSize::Exact) {
		ReportOnce(once, api, FormatParamSizeMessage(what, actual, expected));
	}
	return check != ParamSize::Smaller;
}

} // namespace Common::SoftFail

// Reports "Soft-fail: <api>: <printf-style message>" once for this call site.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_SOFT_FAIL(api, ...)                                                                   \
	do {                                                                                           \
		static std::atomic_bool kyty_soft_fail_once_ {false};                                      \
		if (!kyty_soft_fail_once_.exchange(true, std::memory_order_relaxed)) {                     \
			::Common::SoftFail::Report(api, ::fmt::sprintf(__VA_ARGS__));                          \
		}                                                                                          \
	} while (false)

// Common::SoftFail::AcceptParamSize with a flag of its own for this call site.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_ACCEPT_PARAM_SIZE(api, what, actual, expected)                                        \
	([&]() {                                                                                       \
		static std::atomic_bool kyty_param_size_once_ {false};                                     \
		return ::Common::SoftFail::AcceptParamSize(kyty_param_size_once_, api, what,               \
		                                           static_cast<uint64_t>(actual),                  \
		                                           static_cast<uint64_t>(expected));               \
	}())

#endif /* KYTY_COMMON_SOFT_FAIL_H_ */
