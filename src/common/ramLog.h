#ifndef KYTY_COMMON_RAM_LOG_H_
#define KYTY_COMMON_RAM_LOG_H_

// gta6-prep: ram
// The "RAM" lines of a session (their text is in common/ramOptions.h): what the host has once the
// guest memory section is committed, the RAM profile, a page file warning, and a "RAM usage:" line
// every 30 s while a session log is open.

#include "common/ramOptions.h"

#include <cstdint>
#include <functional>

namespace Common::RamLog {

// Writes the "RAM:" line, the "RAM profile:" line under --ram-profile low and, on Windows, the
// "RAM warning:" of a small commit headroom. With a session log open (Log::SessionLogOpen()) it
// also starts the thread of the "RAM usage:" lines, which never stops. `guest`: the bytes of the
// guest memory section. Call it once the section exists and before the renderer starts; later
// calls do nothing.
void Start(uint64_t guest);

// The renderer's fields of the "RAM usage:" lines; empty: none. The thread of the lines calls it
// under a lock this function takes too, so once SetExtraFields({}) returns the old one is not
// called again. Any thread, before Start() too.
void SetExtraFields(std::function<Config::RendererMemory()> fields);

// One more "RAM usage:" line, without the renderer's fields, for the report of a fatal error
// (Common::DbgReport): sampled and formatted on the stack, without allocating. Nothing before
// Start().
void WriteExitLine();

} // namespace Common::RamLog

#endif /* KYTY_COMMON_RAM_LOG_H_ */
