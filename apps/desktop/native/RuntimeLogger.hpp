#pragma once

#include <QString>

namespace RuntimeLogger {

// Installs the process-wide Qt message handler and opens the persistent log.
// Safe to call more than once; subsequent calls are no-ops while installed.
[[nodiscard]] bool install();

// Flushes and closes the log, then restores the message handler that was
// active before install().
void shutdown();

// Returns the absolute path of the active log file, or an empty string before
// install() has resolved a destination.
[[nodiscard]] QString currentLogPath();

} // namespace RuntimeLogger
