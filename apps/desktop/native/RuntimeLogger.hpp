#pragma once

#include <QString>

namespace RuntimeLogger {

enum class LogExportError
{
    None,
    LoggerUnavailable,
    InvalidDestination,
    NoLogsAvailable,
    DestinationUnavailable,
    ReadFailed,
    WriteFailed,
};

struct LogExportResult
{
    LogExportError error = LogExportError::None;
    QString destinationPath;
    QString detail;
    qsizetype exportedFileCount = 0;

    [[nodiscard]] bool succeeded() const
    {
        return error == LogExportError::None;
    }
};

// Installs the process-wide Qt message handler and opens the persistent log.
// Safe to call more than once; subsequent calls are no-ops while installed.
[[nodiscard]] bool install();

// Flushes and closes the log, then restores the message handler that was
// active before install().
void shutdown();

// Returns the absolute path of the active log file, or an empty string before
// install() has resolved a destination.
[[nodiscard]] QString currentLogPath();

// Flushes the active logger and writes a bounded, single-file bundle containing
// the most recent managed logs. The destination is replaced atomically.
[[nodiscard]] LogExportResult exportRecentLogs(
    const QString &destinationPath);

} // namespace RuntimeLogger
