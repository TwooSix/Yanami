#pragma once

#include <QString>
#include <QStringView>
#include <QVariantMap>

namespace YanamiPerformance {

// Production-safe, opt-in milestone trace used by the performance laboratory.
// The trace is inert unless --performance-trace or YANAMI_PERF_TRACE is set.
// It records only allow-listed measurements supplied by callers; URLs,
// credentials and media titles must never be passed as attributes.
class PerformanceTrace final
{
public:
    static void initialize(int argc, char *argv[]);
    static bool enabled();
    static qint64 monotonicNanoseconds();
    static QString outputPath();
    static void mark(QStringView milestone, const QVariantMap &attributes = {});
    // Persists every event accepted before this call. Normal marks are buffered
    // and written in bounded batches so instrumentation does not synchronously
    // flush the trace file for every milestone.
    static void flush();
    static void shutdown();
};

} // namespace YanamiPerformance
