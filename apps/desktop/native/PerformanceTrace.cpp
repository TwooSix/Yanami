#include "PerformanceTrace.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <memory>

namespace YanamiPerformance {
namespace {

using MonotonicClock = std::chrono::steady_clock;
constexpr qsizetype maxBufferedBytes = 64 * 1024;
constexpr qsizetype maxBufferedEvents = 128;

struct TraceState {
    QMutex mutex;
    std::unique_ptr<QFile> file;
    QByteArray pending;
    QString path;
    QString runId;
    std::atomic_bool enabled {false};
    qsizetype pendingEvents = 0;
    bool initialized = false;
};

TraceState &state()
{
    static TraceState value;
    return value;
}

const MonotonicClock::time_point processEpoch = MonotonicClock::now();

QString tracePathFromArguments(int argc, char *argv[])
{
    const QString prefix = QStringLiteral("--performance-trace=");
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument.startsWith(prefix))
            return argument.sliced(prefix.size()).trimmed();
        if (argument == QStringLiteral("--performance-trace")
            && index + 1 < argc) {
            return QString::fromLocal8Bit(argv[index + 1]).trimmed();
        }
    }
    return QString::fromLocal8Bit(qgetenv("YANAMI_PERF_TRACE")).trimmed();
}

QString runIdFromEnvironment()
{
    const QString supplied =
        QString::fromLocal8Bit(qgetenv("YANAMI_PERF_RUN_ID")).trimmed();
    return supplied.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : supplied;
}

bool writePending(TraceState &trace, bool flushFile)
{
    if (!trace.file || !trace.file->isOpen())
        return false;

    qsizetype written = 0;
    while (written < trace.pending.size()) {
        const qint64 chunk = trace.file->write(
            trace.pending.constData() + written,
            trace.pending.size() - written);
        if (chunk <= 0)
            break;
        written += chunk;
    }
    if (written > 0)
        trace.pending.remove(0, written);
    if (trace.pending.isEmpty())
        trace.pendingEvents = 0;

    const bool writeSucceeded = trace.pending.isEmpty();
    const bool flushSucceeded = !flushFile || trace.file->flush();
    if (writeSucceeded && flushSucceeded)
        return true;

    // A broken/full trace destination must not turn optional diagnostics into
    // unbounded process memory. Stop accepting events after the first write
    // failure; normal product logging remains independent of this channel.
    trace.pending.clear();
    trace.pendingEvents = 0;
    trace.enabled.store(false, std::memory_order_release);
    trace.file->close();
    trace.file.reset();
    return false;
}

} // namespace

void PerformanceTrace::initialize(int argc, char *argv[])
{
    TraceState &trace = state();
    QMutexLocker locker(&trace.mutex);
    if (trace.initialized)
        return;
    trace.initialized = true;
    trace.path = tracePathFromArguments(argc, argv);
    if (trace.path.isEmpty())
        return;
    trace.runId = runIdFromEnvironment();

    auto file = std::make_unique<QFile>(trace.path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        trace.path.clear();
        return;
    }
    trace.file = std::move(file);
    trace.pending.reserve(maxBufferedBytes);
    trace.enabled.store(true, std::memory_order_release);
}

bool PerformanceTrace::enabled()
{
    return state().enabled.load(std::memory_order_acquire);
}

qint64 PerformanceTrace::monotonicNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               MonotonicClock::now() - processEpoch)
        .count();
}

QString PerformanceTrace::outputPath()
{
    TraceState &trace = state();
    QMutexLocker locker(&trace.mutex);
    return trace.path;
}

void PerformanceTrace::mark(
    QStringView milestone,
    const QVariantMap &attributes)
{
    if (milestone.isEmpty())
        return;

    TraceState &trace = state();
    if (!trace.enabled.load(std::memory_order_acquire))
        return;
    QMutexLocker locker(&trace.mutex);
    if (!trace.file || !trace.file->isOpen())
        return;

    QString suite = QStringLiteral("startup");
    if (milestone.startsWith(QStringLiteral("playback_")))
        suite = QStringLiteral("playback");
    else if (milestone.startsWith(QStringLiteral("interaction_")))
        suite = QStringLiteral("interaction");
    else if (milestone.startsWith(QStringLiteral("search_")))
        suite = QStringLiteral("search");
    else if (milestone.startsWith(QStringLiteral("backend_"))
             && milestone != QStringLiteral("backend_services_ready"))
        suite = QStringLiteral("backend");
    const quint64 generation =
        attributes.value(QStringLiteral("generation"), 0).toULongLong();
    QJsonObject event {
        {QStringLiteral("schemaVersion"), QStringLiteral("1.0")},
        {QStringLiteral("runId"), trace.runId},
        {QStringLiteral("suite"), suite},
        {QStringLiteral("scenarioId"), QStringLiteral("desktop.runtime")},
        {QStringLiteral("milestone"), milestone.toString()},
        {QStringLiteral("monotonicNs"), monotonicNanoseconds()},
        {QStringLiteral("generation"), static_cast<qint64>(generation)},
        {QStringLiteral("processId"), QCoreApplication::applicationPid()},
        {QStringLiteral("wallClockUtc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("attributes"), QJsonObject::fromVariantMap(attributes)},
    };
    trace.pending.append(QJsonDocument(event).toJson(QJsonDocument::Compact));
    trace.pending.append('\n');
    ++trace.pendingEvents;
    if (trace.pendingEvents >= maxBufferedEvents
        || trace.pending.size() >= maxBufferedBytes) {
        // A bounded batch write keeps long-running traces observable without
        // imposing QFile::flush() on the render/input path for every event.
        writePending(trace, false);
    }
}

void PerformanceTrace::flush()
{
    TraceState &trace = state();
    if (!trace.enabled.load(std::memory_order_acquire))
        return;
    QMutexLocker locker(&trace.mutex);
    if (!trace.enabled.load(std::memory_order_relaxed))
        return;
    writePending(trace, true);
}

void PerformanceTrace::shutdown()
{
    TraceState &trace = state();
    if (!trace.enabled.exchange(false, std::memory_order_acq_rel))
        return;
    QMutexLocker locker(&trace.mutex);
    if (trace.file) {
        writePending(trace, true);
        if (trace.file) {
            trace.file->close();
            trace.file.reset();
        }
    }
}

} // namespace YanamiPerformance
