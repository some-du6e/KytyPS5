#ifndef KYTY_COMMON_ASSERT_H_
#define KYTY_COMMON_ASSERT_H_

#include "common/common.h"
#include "common/logging/log.h"

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace Common {

// While a SoftExitScope is alive on a thread, the fatal errors that thread reaches (EXIT,
// EXIT_IF, EXIT_NOT_IMPLEMENTED) throw a SoftExitError instead of ending the process, for the
// caller to catch: work that is not the guest's own, such as compiling a record that came from a
// file, can then drop what it cannot handle. Needs a build with exceptions; without them the
// scope changes nothing.
class SoftExitError: public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class SoftExitScope {
public:
	SoftExitScope();
	~SoftExitScope();
	SoftExitScope(const SoftExitScope&)            = delete;
	SoftExitScope& operator=(const SoftExitScope&) = delete;

private:
	bool m_previous;
};

// The shader this thread is compiling, named in the report of a fatal error (see DbgReport), so
// that a crash log of a recompiler failure says which shader to look at. Header-only: tools and
// tests that compile the recompiler without the common library need nothing more.
struct ShaderCompileContext {
	const char* stage = nullptr; // null: none
	uint64_t    hash  = 0;
};
inline thread_local ShaderCompileContext t_shader_compile_context;

// Marks the shader being compiled until the end of the scope; nests.
class ShaderCompileScope {
public:
	ShaderCompileScope(const char* stage, uint64_t hash): m_previous(t_shader_compile_context) {
		t_shader_compile_context = {stage != nullptr ? stage : "?", hash};
	}
	~ShaderCompileScope() { t_shader_compile_context = m_previous; }
	ShaderCompileScope(const ShaderCompileScope&)            = delete;
	ShaderCompileScope& operator=(const ShaderCompileScope&) = delete;

private:
	ShaderCompileContext m_previous;
};

[[noreturn]] void DbgExitHandler(char const* file, int line, std::string_view text);
[[noreturn]] void DbgExitIfHandler(char const* expr, char const* file, int line);
[[noreturn]] void DbgNotImplementedHandler(char const* expr, char const* file, int line);

} // namespace Common

#ifndef KYTY_FINAL
#define EXIT_IF(x) ((x) ? Common::DbgExitIfHandler(#x, __FILE__, __LINE__) : (void)0)
#else
#define EXIT_IF(x) ((void)(false && (x)))
#endif
#define EXIT(...) Common::DbgExitHandler(__FILE__, __LINE__, ::fmt::sprintf(__VA_ARGS__))
#define EXIT_NOT_IMPLEMENTED(x)                                                                    \
	((x) ? Common::DbgNotImplementedHandler(#x, __FILE__, __LINE__) : (void)0)

#endif /* KYTY_COMMON_ASSERT_H_ */
