#include "PerformanceTrace.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdlib>
#include <thread>
#include <vector>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        qCritical().noquote() << message;
    return condition;
}

QByteArray readVisibleTrace(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return file.readAll();
}

QList<QByteArray> traceLines(const QByteArray &contents)
{
    QList<QByteArray> lines = contents.split('\n');
    lines.removeAll(QByteArray{});
    return lines;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--disabled-child"))) {
        const QString sentinel = QString::fromLocal8Bit(
            qgetenv("YANAMI_PERF_DISABLED_SENTINEL"));
        qunsetenv("YANAMI_PERF_TRACE");
        qunsetenv("YANAMI_PERF_RUN_ID");
        YanamiPerformance::PerformanceTrace::initialize(argc, argv);
        YanamiPerformance::PerformanceTrace::mark(
            QStringLiteral("must_remain_disabled"));
        YanamiPerformance::PerformanceTrace::flush();
        return require(
                   !YanamiPerformance::PerformanceTrace::enabled()
                       && YanamiPerformance::PerformanceTrace::outputPath().isEmpty()
                       && (sentinel.isEmpty() || !QFile::exists(sentinel)),
                   "disabled trace must not allocate an output or write an event")
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary trace directory is unavailable"))
        return EXIT_FAILURE;

    const QString disabledSentinel = directory.filePath(
        QStringLiteral("disabled-trace-must-not-exist.jsonl"));
    QProcess disabledChild;
    QProcessEnvironment disabledEnvironment = QProcessEnvironment::systemEnvironment();
    disabledEnvironment.remove(QStringLiteral("YANAMI_PERF_TRACE"));
    disabledEnvironment.remove(QStringLiteral("YANAMI_PERF_RUN_ID"));
    disabledEnvironment.insert(
        QStringLiteral("YANAMI_PERF_DISABLED_SENTINEL"), disabledSentinel);
    disabledChild.setProcessEnvironment(disabledEnvironment);
    disabledChild.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--disabled-child")});
    if (!require(disabledChild.waitForFinished(10'000)
                     && disabledChild.exitStatus() == QProcess::NormalExit
                     && disabledChild.exitCode() == EXIT_SUCCESS
                     && !QFile::exists(disabledSentinel),
                 "disabled trace child did not remain inert")) {
        return EXIT_FAILURE;
    }

    const QString outputPath = directory.filePath(QStringLiteral("runtime-events.jsonl"));
    qputenv("YANAMI_PERF_TRACE", outputPath.toLocal8Bit());
    qputenv("YANAMI_PERF_RUN_ID", QByteArrayLiteral("trace-contract-test"));

    YanamiPerformance::PerformanceTrace::initialize(argc, argv);
    if (!require(YanamiPerformance::PerformanceTrace::enabled(),
                 "trace did not become enabled")) {
        return EXIT_FAILURE;
    }
    if (!require(YanamiPerformance::PerformanceTrace::outputPath() == outputPath,
                 "trace output path differs from the configured path")) {
        return EXIT_FAILURE;
    }
    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("backend_services_ready"),
        {
            {QStringLiteral("generation"), 7},
            {QStringLiteral("ready"), true},
        });
    YanamiPerformance::PerformanceTrace::mark(QStringLiteral("empty_attributes"));

    if (!require(readVisibleTrace(outputPath).isEmpty(),
                 "marks below the batch limit must remain buffered until a boundary")) {
        return EXIT_FAILURE;
    }
    YanamiPerformance::PerformanceTrace::flush();
    QList<QByteArray> lines = traceLines(readVisibleTrace(outputPath));
    if (!require(lines.size() == 2,
                 "explicit flush did not persist every previously accepted event")) {
        return EXIT_FAILURE;
    }

    constexpr int producerCount = 6;
    constexpr int eventsPerProducer = 80;
    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([producer] {
            for (int ordinal = 0; ordinal < eventsPerProducer; ++ordinal) {
                YanamiPerformance::PerformanceTrace::mark(
                    QStringLiteral("interaction_concurrent"),
                    {
                        {QStringLiteral("generation"),
                         producer * eventsPerProducer + ordinal + 1},
                        {QStringLiteral("producer"), producer},
                        {QStringLiteral("ordinal"), ordinal},
                    });
            }
        });
    }
    for (std::thread &producer : producers)
        producer.join();

    YanamiPerformance::PerformanceTrace::flush();
    lines = traceLines(readVisibleTrace(outputPath));
    const int concurrentBoundaryCount =
        2 + producerCount * eventsPerProducer;
    if (!require(lines.size() == concurrentBoundaryCount,
                 "concurrent flush lost or duplicated an event")) {
        return EXIT_FAILURE;
    }

    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("playback_buffered_after_flush"));
    if (!require(
            traceLines(readVisibleTrace(outputPath)).size()
                == concurrentBoundaryCount,
            "a single mark unexpectedly forced a synchronous file flush")) {
        return EXIT_FAILURE;
    }
    YanamiPerformance::PerformanceTrace::flush();
    if (!require(
            traceLines(readVisibleTrace(outputPath)).size()
                == concurrentBoundaryCount + 1,
            "a second explicit flush did not establish a new persistence boundary")) {
        return EXIT_FAILURE;
    }

    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("shutdown_boundary"));
    YanamiPerformance::PerformanceTrace::shutdown();

    QFile file(outputPath);
    if (!require(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "trace result could not be read")) {
        return EXIT_FAILURE;
    }
    lines = traceLines(file.readAll());
    if (!require(lines.size() == concurrentBoundaryCount + 2
                     && !lines.constFirst().isEmpty(),
                 "shutdown did not persist the final buffered event")) {
        return EXIT_FAILURE;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(lines.constFirst(), &parseError);
    if (!require(parseError.error == QJsonParseError::NoError && document.isObject(),
                 "trace event is not valid JSON")) {
        return EXIT_FAILURE;
    }
    const QJsonObject event = document.object();
    const QJsonObject attributes = event.value(QStringLiteral("attributes")).toObject();
    const bool contractValid =
        event.value(QStringLiteral("schemaVersion")).toString() == QStringLiteral("1.0")
        && event.value(QStringLiteral("runId")).toString() == QStringLiteral("trace-contract-test")
        && event.value(QStringLiteral("suite")).toString() == QStringLiteral("startup")
        && event.value(QStringLiteral("scenarioId")).toString() == QStringLiteral("desktop.runtime")
        && event.value(QStringLiteral("milestone")).toString() == QStringLiteral("backend_services_ready")
        && event.value(QStringLiteral("monotonicNs")).toDouble(-1) >= 0
        && event.value(QStringLiteral("generation")).toInteger(-1) == 7
        && event.value(QStringLiteral("processId")).toDouble(-1) > 0
        && !event.value(QStringLiteral("wallClockUtc")).toString().isEmpty()
        && attributes.value(QStringLiteral("generation")).toInt() == 7
        && attributes.value(QStringLiteral("ready")).toBool();
    const QJsonDocument emptyDocument = QJsonDocument::fromJson(lines.at(1), &parseError);
    const QJsonObject emptyEvent = emptyDocument.object();
    const bool emptyAttributesValid = parseError.error == QJsonParseError::NoError
        && emptyEvent.value(QStringLiteral("generation")).toInteger(-1) == 0
        && emptyEvent.value(QStringLiteral("attributes")).isObject()
        && emptyEvent.value(QStringLiteral("attributes")).toObject().isEmpty();

    QHash<int, int> nextOrdinal;
    for (int producer = 0; producer < producerCount; ++producer)
        nextOrdinal.insert(producer, 0);
    qint64 previousMonotonicNs = -1;
    bool concurrentContractValid = true;
    for (int index = 0; index < lines.size(); ++index) {
        const QJsonDocument lineDocument =
            QJsonDocument::fromJson(lines.at(index), &parseError);
        if (parseError.error != QJsonParseError::NoError
            || !lineDocument.isObject()) {
            concurrentContractValid = false;
            break;
        }
        const QJsonObject lineEvent = lineDocument.object();
        const qint64 monotonicNs =
            lineEvent.value(QStringLiteral("monotonicNs")).toInteger(-1);
        if (monotonicNs < previousMonotonicNs) {
            concurrentContractValid = false;
            break;
        }
        previousMonotonicNs = monotonicNs;
        if (index < 2 || index >= concurrentBoundaryCount)
            continue;

        const QJsonObject lineAttributes =
            lineEvent.value(QStringLiteral("attributes")).toObject();
        const int producer =
            lineAttributes.value(QStringLiteral("producer")).toInt(-1);
        const int ordinal =
            lineAttributes.value(QStringLiteral("ordinal")).toInt(-1);
        if (lineEvent.value(QStringLiteral("milestone")).toString()
                != QStringLiteral("interaction_concurrent")
            || lineEvent.value(QStringLiteral("suite")).toString()
                != QStringLiteral("interaction")
            || !nextOrdinal.contains(producer)
            || ordinal != nextOrdinal.value(producer)) {
            concurrentContractValid = false;
            break;
        }
        nextOrdinal[producer] = ordinal + 1;
    }
    for (int producer = 0; producer < producerCount; ++producer) {
        concurrentContractValid = concurrentContractValid
            && nextOrdinal.value(producer) == eventsPerProducer;
    }
    const QJsonObject playbackEvent =
        QJsonDocument::fromJson(lines.at(concurrentBoundaryCount)).object();
    const QJsonObject shutdownEvent = QJsonDocument::fromJson(lines.constLast()).object();
    const bool boundaryEventsValid =
        playbackEvent.value(QStringLiteral("milestone")).toString()
            == QStringLiteral("playback_buffered_after_flush")
        && playbackEvent.value(QStringLiteral("suite")).toString()
            == QStringLiteral("playback")
        && shutdownEvent.value(QStringLiteral("milestone")).toString()
            == QStringLiteral("shutdown_boundary");
    return require(contractValid && emptyAttributesValid
                       && concurrentContractValid && boundaryEventsValid,
                   "trace ordering, flush, or PerfEvent v1 contract is invalid")
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
