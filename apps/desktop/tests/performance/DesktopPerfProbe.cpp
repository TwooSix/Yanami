#include "BackendInfrastructure.hpp"
#include "MediaStore.hpp"
#include "RequestCoordinator.hpp"
#include "SearchCoordinator.hpp"

#include <QBitArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSemaphore>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVariantList>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>

#ifndef YANAMI_PERF_BUILD_TYPE
#define YANAMI_PERF_BUILD_TYPE "unknown"
#endif

#ifndef YANAMI_PERF_VERSION
#define YANAMI_PERF_VERSION "unknown"
#endif

namespace {

constexpr auto schemaVersion = "1.0";

struct SearchQuery
{
    QString id;
    QString category;
    QString query;
    QString expectedRank1;
    std::optional<int> expectedMatchCount;
    bool scanNormalizedFixture = false;
    bool imeCommitted = false;
};

struct OracleEntity
{
    QString id;
    QStringList normalizedFields;
};

struct Fixture
{
    QString id;
    QVariantList items;
    QVector<SearchQuery> searchQueries;
    QVector<OracleEntity> oracleEntities;
    QString sha256;
    int titleCount = 0;
    int episodeCount = 0;
    bool validated = true;
    bool oracleIndependent = false;
    QString pinyinFullQuery = QStringLiteral("xingjiyuanhang");
    QString pinyinInitialsQuery = QStringLiteral("xjyh");
};

struct Statistics
{
    qsizetype count = 0;
    double minimum = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double standardDeviation = 0.0;
    std::optional<double> coefficientOfVariation;
};

enum class ResultSeverity {
    Pass,
    Fail,
    InfrastructureInvalid,
};

QString statusName(ResultSeverity severity)
{
    switch (severity) {
    case ResultSeverity::Pass:
        return QStringLiteral("pass");
    case ResultSeverity::Fail:
        return QStringLiteral("fail");
    case ResultSeverity::InfrastructureInvalid:
        return QStringLiteral("infra-invalid");
    }
    return QStringLiteral("infra-invalid");
}

int severityRank(ResultSeverity severity)
{
    switch (severity) {
    case ResultSeverity::Pass:
        return 0;
    case ResultSeverity::Fail:
        return 1;
    case ResultSeverity::InfrastructureInvalid:
        return 2;
    }
    return 2;
}

double percentile(const QList<double> &samples, double quantile)
{
    if (samples.isEmpty())
        return 0.0;
    QList<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    if (sorted.size() == 1)
        return sorted.constFirst();
    const double position = (sorted.size() - 1) * std::clamp(quantile, 0.0, 1.0);
    const qsizetype lower = static_cast<qsizetype>(std::floor(position));
    const qsizetype upper = static_cast<qsizetype>(std::ceil(position));
    const double fraction = position - lower;
    return sorted.at(lower) + (sorted.at(upper) - sorted.at(lower)) * fraction;
}

Statistics statistics(const QList<double> &samples)
{
    if (samples.isEmpty())
        return {};
    const auto [minimum, maximum] = std::minmax_element(samples.cbegin(), samples.cend());
    double sum = 0.0;
    for (const double sample : samples)
        sum += sample;
    const double mean = sum / samples.size();
    double squaredDifferenceSum = 0.0;
    for (const double sample : samples) {
        const double difference = sample - mean;
        squaredDifferenceSum += difference * difference;
    }
    const double standardDeviation = std::sqrt(squaredDifferenceSum / samples.size());
    return {
        samples.size(),
        *minimum,
        percentile(samples, 0.50),
        percentile(samples, 0.95),
        percentile(samples, 0.99),
        *maximum,
        mean,
        standardDeviation,
        std::abs(mean) > 0.000'000'001
            ? std::optional<double>(standardDeviation / std::abs(mean))
            : std::nullopt,
    };
}

QJsonArray rawSamples(const QList<double> &samples)
{
    QJsonArray result;
    for (const double sample : samples)
        result.push_back(sample);
    return result;
}

QJsonObject statisticsJson(const Statistics &value)
{
    return {
        {QStringLiteral("count"), static_cast<qint64>(value.count)},
        {QStringLiteral("min"), value.minimum},
        {QStringLiteral("p50"), value.p50},
        {QStringLiteral("p95"), value.p95},
        {QStringLiteral("p99"), value.p99},
        {QStringLiteral("max"), value.maximum},
        {QStringLiteral("mean"), value.mean},
        {QStringLiteral("standardDeviation"), value.standardDeviation},
        {QStringLiteral("coefficientOfVariation"),
         value.coefficientOfVariation
             ? QJsonValue(*value.coefficientOfVariation)
             : QJsonValue(QJsonValue::Null)},
    };
}

QString canonicalProfile(const QString &profile)
{
    if (profile == QStringLiteral("pr")
        || profile == QStringLiteral("pull-request")
        || profile == QStringLiteral("pullrequest")) {
        return QStringLiteral("PullRequest");
    }
    if (profile == QStringLiteral("lab"))
        return QStringLiteral("Lab");
    if (profile == QStringLiteral("nightly"))
        return QStringLiteral("Nightly");
    if (profile == QStringLiteral("weekly"))
        return QStringLiteral("Weekly");
    if (profile == QStringLiteral("release"))
        return QStringLiteral("Release");
    return {};
}

QString suiteForMetric(const QString &id)
{
    if (id.startsWith(QStringLiteral("search.")) || id.contains(QStringLiteral(".search.")))
        return QStringLiteral("search");
    if (id.startsWith(QStringLiteral("interaction."))
        || id.contains(QStringLiteral(".interaction."))) {
        return QStringLiteral("interaction");
    }
    if (id.startsWith(QStringLiteral("playback."))
        || id.contains(QStringLiteral(".playback."))) {
        return QStringLiteral("playback");
    }
    if (id.startsWith(QStringLiteral("startup."))
        || id.contains(QStringLiteral(".startup."))) {
        return QStringLiteral("startup");
    }
    return QStringLiteral("backend");
}

class ResultDocument final
{
public:
    ResultDocument(QString profile, QString mode, const Fixture &fixture)
        : m_profile(canonicalProfile(profile))
        , m_mode(std::move(mode))
        , m_fixtureId(fixture.id)
        , m_fixtureSha256(fixture.sha256)
        , m_fixtureItemCount(fixture.items.size())
        , m_fixtureTitleCount(fixture.titleCount)
        , m_fixtureEpisodeCount(fixture.episodeCount)
        , m_fixtureValidated(fixture.validated)
        , m_runId(QUuid::createUuid().toString(QUuid::WithoutBraces))
        , m_startedAtUtc(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
    {
        m_clock.start();
    }

    void mark(
        const QString &milestone,
        quint64 generation = 0,
        const QJsonObject &attributes = {})
    {
        QJsonObject event {
            {QStringLiteral("schemaVersion"), QString::fromLatin1(schemaVersion)},
            {QStringLiteral("runId"), m_runId},
            {QStringLiteral("scenarioId"), QStringLiteral("desktop.media-store")},
            {QStringLiteral("milestone"), milestone},
            {QStringLiteral("monotonicNs"), m_clock.nsecsElapsed()},
            {QStringLiteral("generation"), static_cast<qint64>(generation)},
            {QStringLiteral("attributes"), attributes},
        };
        m_events.push_back(event);
    }

    void addInformationalLatency(
        const QString &id,
        const QList<double> &samples,
        const QString &description,
        const QString &unit = QStringLiteral("ms"))
    {
        if (samples.isEmpty()) {
            infrastructureFailure(id, QStringLiteral("probe produced no samples"));
            return;
        }
        m_metrics.push_back(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("suite"), suiteForMetric(id)},
            {QStringLiteral("category"), QStringLiteral("latency")},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("description"), description},
            {QStringLiteral("samples"), rawSamples(samples)},
            {QStringLiteral("statistics"), statisticsJson(statistics(samples))},
            {QStringLiteral("absoluteTarget"), QJsonObject{}},
            {QStringLiteral("comparison"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("localStatus"), QStringLiteral("pass")},
            {QStringLiteral("localReasons"), QJsonArray{}},
            {QStringLiteral("informational"), true},
        });
    }

    void addFixtureComponentObservationMetric(
        const QString &id,
        const QList<double> &samples,
        const QString &description,
        const QString &unit,
        const QString &observationKind,
        const QString &scope)
    {
        if (samples.isEmpty()) {
            infrastructureFailure(id, QStringLiteral("probe produced no samples"));
            return;
        }
        m_metrics.push_back(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("suite"), suiteForMetric(id)},
            {QStringLiteral("category"), QStringLiteral("component_observation")},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("description"), description},
            {QStringLiteral("samples"), rawSamples(samples)},
            {QStringLiteral("statistics"), statisticsJson(statistics(samples))},
            {QStringLiteral("attributes"), QJsonObject{
                 {QStringLiteral("evidence"), QStringLiteral("fixture-component-observation")},
                 {QStringLiteral("enforcement"), QStringLiteral("observation")},
                 {QStringLiteral("observationKind"), observationKind},
                 {QStringLiteral("scope"), scope},
             }},
            {QStringLiteral("absoluteTarget"), QJsonObject{}},
            {QStringLiteral("comparison"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("localStatus"), QStringLiteral("pass")},
            {QStringLiteral("localReasons"), QJsonArray{}},
            {QStringLiteral("informational"), true},
        });
    }

    void fixtureComponentObservation(
        const QString &id,
        bool observed,
        const QJsonValue &expected,
        const QJsonValue &actual,
        const QString &detail,
        const QJsonObject &evidenceDetails = {})
    {
        QJsonObject details {
            {QStringLiteral("expected"), expected},
            {QStringLiteral("actual"), actual},
            {QStringLiteral("description"), detail},
            {QStringLiteral("evidence"), QStringLiteral("fixture-component-observation")},
            {QStringLiteral("enforcement"), QStringLiteral("observation")},
        };
        for (auto iterator = evidenceDetails.constBegin();
             iterator != evidenceDetails.constEnd();
             ++iterator) {
            details.insert(iterator.key(), iterator.value());
        }
        m_invariants.push_back(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("passed"), observed},
            {QStringLiteral("status"), observed ? QStringLiteral("pass") : QStringLiteral("fail")},
            {QStringLiteral("details"), details},
            {QStringLiteral("informational"), true},
        });
    }

    void invariant(
        const QString &id,
        bool passed,
        const QJsonValue &expected,
        const QJsonValue &actual,
        const QString &detail,
        const QJsonObject &evidenceDetails = {})
    {
        const ResultSeverity severity = passed ? ResultSeverity::Pass : ResultSeverity::Fail;
        QJsonObject details {
            {QStringLiteral("expected"), expected},
            {QStringLiteral("actual"), actual},
            {QStringLiteral("description"), detail},
        };
        for (auto iterator = evidenceDetails.constBegin();
             iterator != evidenceDetails.constEnd();
             ++iterator) {
            details.insert(iterator.key(), iterator.value());
        }
        m_invariants.push_back(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("passed"), passed},
            {QStringLiteral("status"), statusName(severity)},
            {QStringLiteral("details"), details},
        });
        if (!passed)
            m_reasons.push_back(QStringLiteral("correctness invariant failed: %1").arg(id));
        promote(severity);
    }

    void capability(
        const QString &id,
        bool passed,
        const QJsonValue &expected,
        const QJsonValue &actual,
        const QString &detail)
    {
        const QList<double> samples {static_cast<double>(actual.toInt())};
        const QString reason = passed
            ? QString()
            : QStringLiteral("required capability is not implemented: %1").arg(id);
        m_metrics.push_back(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("suite"), suiteForMetric(id)},
            {QStringLiteral("category"), QStringLiteral("capability")},
            {QStringLiteral("unit"), QStringLiteral("matches")},
            {QStringLiteral("description"), detail},
            {QStringLiteral("samples"), rawSamples(samples)},
            {QStringLiteral("statistics"), statisticsJson(statistics(samples))},
            {QStringLiteral("absoluteTarget"), QJsonObject{
                 {QStringLiteral("minimum"), expected},
             }},
            {QStringLiteral("comparison"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("localStatus"), QStringLiteral("pass")},
            {QStringLiteral("localReasons"), reason.isEmpty() ? QJsonArray{} : QJsonArray{reason}},
            {QStringLiteral("informational"), true},
        });
        if (!reason.isEmpty())
            m_reasons.push_back(reason);
    }

    void infrastructureFailure(const QString &id, const QString &detail)
    {
        m_invariants.push_back(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("passed"), false},
            {QStringLiteral("status"), QStringLiteral("infra-invalid")},
            {QStringLiteral("details"), detail},
        });
        m_reasons.push_back(QStringLiteral("infrastructure failure: %1").arg(id));
        promote(ResultSeverity::InfrastructureInvalid);
    }

    QJsonObject json() const
    {
        const QJsonObject environmentDetails {
            {QStringLiteral("os"), QSysInfo::prettyProductName()},
            {QStringLiteral("kernel"), QSysInfo::kernelVersion()},
            {QStringLiteral("buildCpuArchitecture"), QSysInfo::buildCpuArchitecture()},
            {QStringLiteral("currentCpuArchitecture"), QSysInfo::currentCpuArchitecture()},
            {QStringLiteral("logicalCpuCount"), QThread::idealThreadCount()},
            {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
            {QStringLiteral("buildType"), QString::fromLatin1(YANAMI_PERF_BUILD_TYPE)},
            {QStringLiteral("yanamiVersion"), QString::fromLatin1(YANAMI_PERF_VERSION)},
        };
        const QString orchestratedFingerprint = QString::fromLocal8Bit(
            qgetenv("YANAMI_PERF_MACHINE_FINGERPRINT")).trimmed();
        const QString environmentFingerprint = orchestratedFingerprint.isEmpty()
            ? QString::fromLatin1(
                  QCryptographicHash::hash(
                      QJsonDocument(environmentDetails).toJson(QJsonDocument::Compact),
                      QCryptographicHash::Sha256)
                      .toHex())
            : orchestratedFingerprint;
        const bool referenceEvaluated = qEnvironmentVariableIsSet(
            "YANAMI_PERF_REFERENCE_MATCH");
        const bool referenceMatch = referenceEvaluated
            && qEnvironmentVariableIntValue("YANAMI_PERF_REFERENCE_MATCH") == 1;
        QJsonArray mismatchReasons;
        if (!referenceEvaluated) {
            mismatchReasons.push_back(QStringLiteral(
                "reference environment match was not evaluated by this component probe"));
        } else if (!referenceMatch) {
            mismatchReasons.push_back(QStringLiteral(
                "the orchestrator reported a reference-environment mismatch"));
        }
        return {
            {QStringLiteral("schemaVersion"), QString::fromLatin1(schemaVersion)},
            {QStringLiteral("contractVersion"), QStringLiteral("run-manifest-v1")},
            {QStringLiteral("runId"), m_runId},
            {QStringLiteral("profile"), m_profile},
            {QStringLiteral("mode"), m_mode},
            {QStringLiteral("probeStatus"), statusName(m_severity)},
            {QStringLiteral("startedAtUtc"), m_startedAtUtc},
            {QStringLiteral("finishedAtUtc"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("environment"), QJsonObject{
                 {QStringLiteral("fingerprint"), environmentFingerprint},
                 {QStringLiteral("referenceMatch"), referenceMatch},
                 {QStringLiteral("mismatchReasons"), mismatchReasons},
                 {QStringLiteral("details"), environmentDetails},
            }},
            {QStringLiteral("fixtures"), QJsonArray{QJsonObject{
                 {QStringLiteral("id"), m_fixtureId},
                 {QStringLiteral("version"), QStringLiteral("1")},
                 {QStringLiteral("sha256"), m_fixtureSha256},
                 {QStringLiteral("validated"), m_fixtureValidated},
                 {QStringLiteral("details"), QJsonObject{
                      {QStringLiteral("itemCount"), m_fixtureItemCount},
                      {QStringLiteral("titleCount"), m_fixtureTitleCount},
                      {QStringLiteral("episodeCount"), m_fixtureEpisodeCount},
                  }},
             }}},
            {QStringLiteral("suites"), QJsonArray{
                 QStringLiteral("search"),
                 QStringLiteral("backend"),
                 QStringLiteral("interaction"),
             }},
            {QStringLiteral("metrics"), m_metrics},
            {QStringLiteral("invariants"), m_invariants},
            {QStringLiteral("reasons"), m_reasons},
            {QStringLiteral("events"), m_events},
        };
    }

    ResultSeverity severity() const { return m_severity; }

private:
    void promote(ResultSeverity severity)
    {
        if (severityRank(severity) > severityRank(m_severity))
            m_severity = severity;
    }

    QString m_profile;
    QString m_mode;
    QString m_fixtureId;
    QString m_fixtureSha256;
    qsizetype m_fixtureItemCount = 0;
    int m_fixtureTitleCount = 0;
    int m_fixtureEpisodeCount = 0;
    bool m_fixtureValidated = false;
    QString m_runId;
    QString m_startedAtUtc;
    QElapsedTimer m_clock;
    QJsonArray m_metrics;
    QJsonArray m_invariants;
    QJsonArray m_reasons;
    QJsonArray m_events;
    ResultSeverity m_severity = ResultSeverity::Pass;
};

QString paddedNumber(int value, int width)
{
    return QStringLiteral("%1").arg(value, width, 10, QLatin1Char('0'));
}

QString normalizeOracleText(const QString &value)
{
    return value.normalized(QString::NormalizationForm_KC).toCaseFolded().trimmed();
}

void appendOracleField(QStringList *fields, const QString &value)
{
    const QString normalized = normalizeOracleText(value);
    if (!normalized.isEmpty() && !fields->contains(normalized))
        fields->push_back(normalized);
}

Fixture makeEmbeddedFixture(const QString &profile)
{
    const bool full = canonicalProfile(profile) != QStringLiteral("PullRequest");
    const int titleCount = full ? 10'000 : 1'000;
    const int episodeCount = full ? 100'000 : 9'000;
    const int total = titleCount + episodeCount;

    Fixture fixture;
    fixture.id = full
        ? QStringLiteral("DesktopF110KLike-v1")
        : QStringLiteral("DesktopT10K-v1");
    fixture.titleCount = titleCount;
    fixture.episodeCount = episodeCount;
    fixture.items.reserve(total);
    QCryptographicHash hash(QCryptographicHash::Sha256);

    for (int index = 0; index < total; ++index) {
        const bool isTitle = index < titleCount;
        const bool isSeries = isTitle && (index % 2 != 0);
        const QString itemType = isTitle
            ? (isSeries ? QStringLiteral("Series") : QStringLiteral("Movie"))
            : QStringLiteral("Episode");
        QString title = isTitle
            ? QStringLiteral("Fixture Title %1").arg(paddedNumber(index, 6))
            : QStringLiteral("Fixture Episode %1").arg(paddedNumber(index - titleCount, 6));
        QString subtitle = isTitle
            ? QStringLiteral("Fixture Subtitle %1").arg(paddedNumber(index, 6))
            : QStringLiteral("S%1 E%2")
                  .arg(paddedNumber((index - titleCount) / 100 + 1, 2),
                       paddedNumber((index - titleCount) % 100 + 1, 3));
        QString seriesTitle = isTitle
            ? QString()
            : QStringLiteral("Fixture Series %1")
                  .arg(paddedNumber((index - titleCount) / 100, 5));

        if (index == 0)
            title = QStringLiteral("星际远航");
        else if (index == 1)
            title = QStringLiteral("The ORBITAL Archive");
        else if (index == 2)
            subtitle = QStringLiteral("银翼特别篇");
        else if (index == 3)
            seriesTitle = QStringLiteral("银河列车");

        const QString id = QStringLiteral("fixture-%1").arg(paddedNumber(index, 6));
        const QVariantMap item {
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), title},
            {QStringLiteral("subtitle"), subtitle},
            {QStringLiteral("seriesTitle"), seriesTitle},
            {QStringLiteral("itemType"), itemType},
            {QStringLiteral("productionYear"), 1980 + (index % 47)},
            {QStringLiteral("updatedAt"), QStringLiteral("2026-01-01T00:00:00Z")},
        };
        fixture.items.push_back(item);
        hash.addData(
            QStringLiteral("%1\x1f%2\x1f%3\x1f%4\x1f%5\n")
                .arg(id, title, subtitle, seriesTitle, itemType)
                .toUtf8());
    }
    fixture.sha256 = QString::fromLatin1(hash.result().toHex());
    return fixture;
}

std::optional<Fixture> loadF110KFixture(
    const QString &directory,
    QString *error)
{
    const QString itemsPath = QDir(directory).filePath(
        QStringLiteral("f110k-items.jsonl"));
    QFile file(itemsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("Unable to open F110K items at %1: %2")
                     .arg(itemsPath, file.errorString());
        return std::nullopt;
    }

    Fixture fixture;
    fixture.id = QStringLiteral("F110K-v1");
    fixture.items.reserve(110'000);
    fixture.oracleEntities.reserve(140'000);
    QHash<QString, QString> titleById;
    titleById.reserve(10'000);
    QHash<QString, qsizetype> derivedSeasonIndexes;
    derivedSeasonIndexes.reserve(32'000);
    QCryptographicHash fileHash(QCryptographicHash::Sha256);
    qsizetype lineNumber = 0;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        ++lineNumber;
        fileHash.addData(line);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            *error = QStringLiteral("Invalid F110K JSON on line %1: %2")
                         .arg(lineNumber)
                         .arg(parseError.errorString());
            return std::nullopt;
        }
        const QJsonObject source = document.object();
        const QString id = source.value(QStringLiteral("id")).toString();
        const QString kind = source.value(QStringLiteral("kind")).toString();
        const QString title = source.value(QStringLiteral("title")).toString();
        if (id.isEmpty() || kind.isEmpty() || title.isEmpty()) {
            *error = QStringLiteral("F110K line %1 is missing id, kind, or title")
                         .arg(lineNumber);
            return std::nullopt;
        }
        if (kind == QStringLiteral("Episode"))
            ++fixture.episodeCount;
        else {
            ++fixture.titleCount;
            titleById.insert(id, title);
        }
        if (fixture.pinyinFullQuery == QStringLiteral("xingjiyuanhang")
            && kind != QStringLiteral("Episode")) {
            const QJsonArray aliases = source.value(QStringLiteral("aliases")).toArray();
            if (aliases.size() >= 3) {
                fixture.pinyinFullQuery = aliases.at(1).toString();
                fixture.pinyinInitialsQuery = aliases.at(2).toString();
            }
        }
        const QJsonArray aliases = source.value(QStringLiteral("aliases")).toArray();
        const QString seriesId = source.value(QStringLiteral("seriesId")).toString();
        const int seasonNumber = source.value(QStringLiteral("season")).toInt();
        const int episodeNumber = source.value(QStringLiteral("episode")).toInt();
        fixture.items.push_back(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), title},
            {QStringLiteral("itemType"), kind},
            {QStringLiteral("aliases"), aliases.toVariantList()},
            {QStringLiteral("seriesId"), seriesId},
            {QStringLiteral("seasonNumber"), seasonNumber},
            {QStringLiteral("episodeNumber"), episodeNumber},
        });

        OracleEntity oracleEntity;
        oracleEntity.id = id;
        appendOracleField(&oracleEntity.normalizedFields, title);
        for (const QJsonValue &alias : aliases)
            appendOracleField(&oracleEntity.normalizedFields, alias.toString());
        if (kind == QStringLiteral("Episode")) {
            const QString seriesTitle = titleById.value(seriesId);
            appendOracleField(&oracleEntity.normalizedFields, seriesTitle);
            appendOracleField(
                &oracleEntity.normalizedFields,
                QStringLiteral("%1 S%2E%3")
                    .arg(seriesTitle,
                         paddedNumber(seasonNumber, 2),
                         paddedNumber(episodeNumber, 2)));

            const QString seasonId = QStringLiteral("season:%1:%2")
                                         .arg(seriesId)
                                         .arg(seasonNumber);
            if (!derivedSeasonIndexes.contains(seasonId)) {
                OracleEntity seasonEntity;
                seasonEntity.id = seasonId;
                appendOracleField(
                    &seasonEntity.normalizedFields,
                    QStringLiteral("%1 第%2季").arg(seriesTitle).arg(seasonNumber));
                appendOracleField(&seasonEntity.normalizedFields, seriesTitle);
                derivedSeasonIndexes.insert(seasonId, fixture.oracleEntities.size());
                fixture.oracleEntities.push_back(std::move(seasonEntity));
            }
        }
        fixture.oracleEntities.push_back(std::move(oracleEntity));
    }
    if (fixture.titleCount != 10'000 || fixture.episodeCount != 100'000) {
        *error = QStringLiteral(
            "F110K counts do not match the v1 contract: titles=%1 episodes=%2")
                     .arg(fixture.titleCount)
                     .arg(fixture.episodeCount);
        return std::nullopt;
    }
    const QString itemHash = QString::fromLatin1(fileHash.result().toHex());
    QFile queriesFile(QDir(directory).filePath(QStringLiteral("search-queries-v1.jsonl")));
    if (!queriesFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("Unable to open F110K queries at %1: %2")
                     .arg(queriesFile.fileName(), queriesFile.errorString());
        return std::nullopt;
    }
    QCryptographicHash queriesHash(QCryptographicHash::Sha256);
    fixture.searchQueries.reserve(20'000);
    lineNumber = 0;
    while (!queriesFile.atEnd()) {
        const QByteArray line = queriesFile.readLine();
        ++lineNumber;
        queriesHash.addData(line);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            *error = QStringLiteral("Invalid F110K query JSON on line %1: %2")
                         .arg(lineNumber)
                         .arg(parseError.errorString());
            return std::nullopt;
        }
        const QJsonObject source = document.object();
        const QJsonObject expectation = source.value(QStringLiteral("expectation")).toObject();
        SearchQuery query;
        query.id = source.value(QStringLiteral("id")).toString();
        query.category = source.value(QStringLiteral("category")).toString();
        query.query = source.value(QStringLiteral("query")).toString();
        query.expectedRank1 = expectation.value(QStringLiteral("rank1")).toString();
        if (expectation.contains(QStringLiteral("matchCount")))
            query.expectedMatchCount = expectation.value(QStringLiteral("matchCount")).toInt();
        query.scanNormalizedFixture = expectation.value(QStringLiteral("oracle")).toString()
            == QStringLiteral("scan-normalized-fixture");
        query.imeCommitted = source.value(QStringLiteral("imeCommitted")).toBool(false);
        const int expectationKinds = !query.expectedRank1.isEmpty()
            + query.expectedMatchCount.has_value()
            + query.scanNormalizedFixture;
        if (query.id.isEmpty() || query.category.isEmpty() || query.query.isEmpty()
            || !query.imeCommitted || expectationKinds != 1) {
            *error = QStringLiteral(
                "F110K query line %1 does not carry one committed, independent expectation")
                         .arg(lineNumber);
            return std::nullopt;
        }
        fixture.searchQueries.push_back(std::move(query));
    }
    if (fixture.searchQueries.size() != 20'000) {
        *error = QStringLiteral("F110K query count does not match v1: expected 20000, got %1")
                     .arg(fixture.searchQueries.size());
        return std::nullopt;
    }
    const QString queryHash = QString::fromLatin1(queriesHash.result().toHex());
    const QString combinedHash = QString::fromLatin1(
        QCryptographicHash::hash(
            QStringLiteral("%1:%2").arg(itemHash, queryHash).toUtf8(),
            QCryptographicHash::Sha256)
            .toHex());
    const QString orchestratedHash = QString::fromLocal8Bit(
        qgetenv("YANAMI_PERF_F110K_SHA256")).trimmed().toLower();
    if (!orchestratedHash.isEmpty() && orchestratedHash != combinedHash) {
        *error = QStringLiteral("F110K combined hash mismatch: expected %1, observed %2")
                     .arg(orchestratedHash, combinedHash);
        return std::nullopt;
    }
    fixture.sha256 = combinedHash;
    fixture.validated = !orchestratedHash.isEmpty();
    fixture.oracleIndependent = true;
    return fixture;
}

std::optional<Fixture> makeFixture(
    const QString &profile,
    const QString &fixtureDirectory,
    QString *error)
{
    if (!fixtureDirectory.isEmpty()) {
        return loadF110KFixture(fixtureDirectory, error);
    }
    return makeEmbeddedFixture(profile);
}

double elapsedMilliseconds(const QElapsedTimer &timer)
{
    return timer.nsecsElapsed() / 1'000'000.0;
}

int matchCount(MediaSearchModel &proxy, const QString &query)
{
    proxy.setSearchText(query);
    return proxy.rowCount();
}

int fixtureOracleCount(const Fixture &fixture, const QString &query)
{
    int matches = 0;
    for (const QVariant &value : fixture.items) {
        const QVariantMap item = value.toMap();
        if (item.value(QStringLiteral("title")).toString().contains(
                query, Qt::CaseInsensitive)
            || item.value(QStringLiteral("subtitle")).toString().contains(
                query, Qt::CaseInsensitive)
            || item.value(QStringLiteral("seriesTitle")).toString().contains(
                query, Qt::CaseInsensitive)) {
            ++matches;
        }
    }
    return matches;
}

bool oracleEntityMatches(const OracleEntity &entity, const QString &normalizedQuery)
{
    return std::any_of(
        entity.normalizedFields.cbegin(),
        entity.normalizedFields.cend(),
        [&normalizedQuery](const QString &field) {
            return field.contains(normalizedQuery);
        });
}

QVector<QString> searchResultIds(MediaSearchModel &model, int limit = -1)
{
    const int count = limit < 0
        ? model.rowCount()
        : std::min(model.rowCount(), limit);
    QVector<QString> ids;
    ids.reserve(count);
    for (int row = 0; row < count; ++row) {
        ids.push_back(model.data(
            model.index(row, 0), MediaQueryModel::EntityIdRole).toString());
    }
    return ids;
}

QVector<QString> independentProductSnapshotMatches(
    const Fixture &fixture,
    const QString &query)
{
    const QString normalizedQuery = normalizeOracleText(query);
    QVector<QString> ids;
    for (const QVariant &value : fixture.items) {
        const QVariantMap item = value.toMap();
        QStringList fields;
        appendOracleField(&fields, item.value(QStringLiteral("title")).toString());
        appendOracleField(&fields, item.value(QStringLiteral("subtitle")).toString());
        appendOracleField(&fields, item.value(QStringLiteral("seriesTitle")).toString());
        appendOracleField(&fields, item.value(QStringLiteral("latestEpisodeSubtitle")).toString());
        for (const QVariant &alias : item.value(QStringLiteral("aliases")).toList())
            appendOracleField(&fields, alias.toString());
        if (std::any_of(fields.cbegin(), fields.cend(), [&normalizedQuery](const QString &field) {
                return field.contains(normalizedQuery);
            })) {
            ids.push_back(item.value(QStringLiteral("id")).toString());
        }
    }
    return ids;
}

double discountedGain(int zeroBasedRank)
{
    return 1.0 / std::log2(static_cast<double>(zeroBasedRank) + 2.0);
}

quint64 oracleFourGramKey(QStringView text, qsizetype offset)
{
    quint64 key = 0;
    for (qsizetype index = 0; index < 4; ++index)
        key = (key << 16) | text.at(offset + index).unicode();
    return key;
}

QHash<quint64, QVector<int>> buildOracleFourGramIndex(
    const Fixture &fixture,
    const QSet<quint64> &requestedKeys)
{
    QHash<quint64, QVector<int>> postings;
    postings.reserve(requestedKeys.size());
    for (int entityIndex = 0; entityIndex < fixture.oracleEntities.size(); ++entityIndex) {
        const OracleEntity &entity = fixture.oracleEntities.at(entityIndex);
        for (const QString &field : entity.normalizedFields) {
            const QStringView view(field);
            for (qsizetype offset = 0; offset + 4 <= view.size(); ++offset) {
                const quint64 key = oracleFourGramKey(view, offset);
                if (requestedKeys.contains(key))
                    postings[key].push_back(entityIndex);
            }
        }
    }
    return postings;
}

QVector<QString> scanIndependentOracle(
    const Fixture &fixture,
    const QString &normalizedQuery,
    const QHash<quint64, QVector<int>> &fourGramIndex)
{
    const QVector<int> *candidates = nullptr;
    if (normalizedQuery.size() >= 4) {
        const QStringView queryView(normalizedQuery);
        for (qsizetype offset = 0; offset + 4 <= queryView.size(); ++offset) {
            const auto found = fourGramIndex.constFind(
                oracleFourGramKey(queryView, offset));
            if (found == fourGramIndex.cend())
                return {};
            if (!candidates || found->size() < candidates->size())
                candidates = &found.value();
        }
    }

    QVector<QString> relevantIds;
    if (candidates) {
        QBitArray seen(fixture.oracleEntities.size());
        relevantIds.reserve(std::min<qsizetype>(candidates->size(), 64));
        for (const int entityIndex : *candidates) {
            if (entityIndex < 0 || entityIndex >= fixture.oracleEntities.size()
                || seen.testBit(entityIndex)) {
                continue;
            }
            seen.setBit(entityIndex);
            const OracleEntity &entity = fixture.oracleEntities.at(entityIndex);
            if (oracleEntityMatches(entity, normalizedQuery))
                relevantIds.push_back(entity.id);
        }
        return relevantIds;
    }

    relevantIds.reserve(64);
    for (const OracleEntity &entity : fixture.oracleEntities) {
        if (oracleEntityMatches(entity, normalizedQuery))
            relevantIds.push_back(entity.id);
    }
    return relevantIds;
}

void runF110KSearchComponentObservation(
    ResultDocument &result,
    const Fixture &fixture,
    MediaSearchModel &model,
    const QString &profile)
{
    if (fixture.id != QStringLiteral("F110K-v1")
        || !fixture.validated
        || fixture.searchQueries.isEmpty()
        || fixture.oracleEntities.isEmpty()) {
        return;
    }

    const QString evidenceScope = QStringLiteral(
        "probe-injected F110K snapshot with fixture-derived entities and aliases");
    const int requiredHotSamples = canonicalProfile(profile) == QStringLiteral("PullRequest") ? 100
        : canonicalProfile(profile) == QStringLiteral("Lab") ? 500
        : 20'000;
    if (fixture.searchQueries.size() < requiredHotSamples) {
        result.fixtureComponentObservation(
            QStringLiteral("search.component.fixture.query_corpus_coverage"),
            false,
            requiredHotSamples,
            fixture.searchQueries.size(),
            QStringLiteral(
                "The fixture query corpus is smaller than the requested component-observation sample count."),
            QJsonObject{{QStringLiteral("scope"), evidenceScope}});
        return;
    }

    const int warmupCount = static_cast<int>(
        std::min<qsizetype>(32, fixture.searchQueries.size()));
    for (int warmup = 0; warmup < warmupCount; ++warmup) {
        model.setSearchText(fixture.searchQueries.at(warmup).query);
        (void)model.rowCount();
    }

    QList<double> hotSamples;
    hotSamples.reserve(requiredHotSamples);
    QElapsedTimer timer;
    for (int sample = 0; sample < requiredHotSamples; ++sample) {
        const SearchQuery &query = fixture.searchQueries.at(sample);
        timer.restart();
        model.setSearchText(query.query);
        (void)model.rowCount();
        hotSamples.push_back(elapsedMilliseconds(timer));
    }
    result.addFixtureComponentObservationMetric(
        QStringLiteral("search.component.fixture.query_hot_ms"),
        hotSamples,
        QStringLiteral(
            "Synchronous MediaSearchModel update over a probe-injected F110K snapshot; this does not traverse the production catalog projection, scheduler, or UI."),
        QStringLiteral("ms"),
        QStringLiteral("latency"),
        evidenceScope);

    QHash<QString, qsizetype> oracleEntityById;
    oracleEntityById.reserve(fixture.oracleEntities.size());
    for (qsizetype index = 0; index < fixture.oracleEntities.size(); ++index)
        oracleEntityById.insert(fixture.oracleEntities.at(index).id, index);

    QSet<quint64> requestedOracleFourGrams;
    for (const SearchQuery &query : fixture.searchQueries) {
        if (!query.scanNormalizedFixture)
            continue;
        const QString normalizedQuery = normalizeOracleText(query.query);
        const QStringView queryView(normalizedQuery);
        for (qsizetype offset = 0; offset + 4 <= queryView.size(); ++offset)
            requestedOracleFourGrams.insert(oracleFourGramKey(queryView, offset));
    }
    const QHash<quint64, QVector<int>> oracleFourGramIndex = buildOracleFourGramIndex(
        fixture, requestedOracleFourGrams);
    QHash<QString, QVector<QString>> scanCache;
    bool oracleValid = fixture.oracleIndependent;
    qint64 exactQueryCount = 0;
    qint64 exactRank1Count = 0;
    qint64 recallQueryCount = 0;
    double recallSum = 0.0;
    qint64 reciprocalRankQueryCount = 0;
    double reciprocalRankSum = 0.0;
    qint64 ndcgQueryCount = 0;
    double ndcgSum = 0.0;
    qint64 duplicateTop50Rows = 0;

    for (const SearchQuery &query : fixture.searchQueries) {
        const QString normalizedQuery = normalizeOracleText(query.query);
        QVector<QString> relevantIds;
        if (!query.expectedRank1.isEmpty()) {
            const auto found = oracleEntityById.constFind(query.expectedRank1);
            const bool expectedEntityMatches = found != oracleEntityById.cend()
                && oracleEntityMatches(fixture.oracleEntities.at(found.value()), normalizedQuery);
            oracleValid = oracleValid && expectedEntityMatches;
            if (expectedEntityMatches)
                relevantIds.push_back(query.expectedRank1);
        } else if (query.expectedMatchCount.has_value()) {
            oracleValid = oracleValid && *query.expectedMatchCount == 0;
        } else if (query.scanNormalizedFixture) {
            const auto cached = scanCache.constFind(normalizedQuery);
            if (cached != scanCache.cend()) {
                relevantIds = cached.value();
            } else {
                relevantIds = scanIndependentOracle(
                    fixture, normalizedQuery, oracleFourGramIndex);
                scanCache.insert(normalizedQuery, relevantIds);
            }
        } else {
            oracleValid = false;
        }

        model.setSearchText(query.query);
        const QVector<QString> actualTop50 = searchResultIds(model, 50);
        QSet<QString> actualUnique;
        actualUnique.reserve(actualTop50.size());
        for (const QString &id : actualTop50)
            actualUnique.insert(id);
        duplicateTop50Rows += actualTop50.size() - actualUnique.size();

        const QSet<QString> relevantSet(relevantIds.cbegin(), relevantIds.cend());
        int expectedRank = -1;
        if (!query.expectedRank1.isEmpty()) {
            const int searchLimit = std::min(model.rowCount(), 10);
            for (int row = 0; row < searchLimit; ++row) {
                if (model.data(model.index(row, 0), MediaQueryModel::EntityIdRole).toString()
                    == query.expectedRank1) {
                    expectedRank = row;
                    break;
                }
            }
            ++reciprocalRankQueryCount;
            if (expectedRank >= 0)
                reciprocalRankSum += 1.0 / static_cast<double>(expectedRank + 1);
        }

        if (query.category == QStringLiteral("exact")) {
            ++exactQueryCount;
            if (expectedRank == 0)
                ++exactRank1Count;
        }

        if (!relevantIds.isEmpty() && relevantIds.size() <= 50) {
            int recalled = 0;
            for (const QString &id : actualTop50) {
                if (relevantSet.contains(id))
                    ++recalled;
            }
            recallSum += static_cast<double>(recalled) / relevantIds.size();
            ++recallQueryCount;
        }

        if (!relevantIds.isEmpty()) {
            double dcg = 0.0;
            const int rankedCount = static_cast<int>(
                std::min<qsizetype>(actualTop50.size(), 10));
            for (int rank = 0; rank < rankedCount; ++rank) {
                if (relevantSet.contains(actualTop50.at(rank)))
                    dcg += discountedGain(rank);
            }
            double idealDcg = 0.0;
            for (int rank = 0; rank < std::min<qsizetype>(relevantIds.size(), 10); ++rank)
                idealDcg += discountedGain(rank);
            ndcgSum += dcg / idealDcg;
            ++ndcgQueryCount;
        }
    }

    if (exactQueryCount == 0 || recallQueryCount == 0
        || reciprocalRankQueryCount == 0 || ndcgQueryCount == 0) {
        result.fixtureComponentObservation(
            QStringLiteral("search.component.fixture.correctness_corpus_coverage"),
            false,
            4,
            static_cast<int>(exactQueryCount > 0)
                + static_cast<int>(recallQueryCount > 0)
                + static_cast<int>(reciprocalRankQueryCount > 0)
                + static_cast<int>(ndcgQueryCount > 0),
            QStringLiteral(
                "The fixture query corpus did not populate every component-observation correctness denominator."),
            QJsonObject{{QStringLiteral("scope"), evidenceScope}});
        return;
    }

    result.addFixtureComponentObservationMetric(
        QStringLiteral("search.component.fixture.exact_title_rank1_ratio"),
        QList<double>{static_cast<double>(exactRank1Count) / exactQueryCount},
        QStringLiteral(
            "Rank-1 success for exact queries in the probe-only F110K model and fixture oracle."),
        QStringLiteral("ratio"),
        QStringLiteral("correctness"),
        evidenceScope);
    result.addFixtureComponentObservationMetric(
        QStringLiteral("search.component.fixture.recall_at_50"),
        QList<double>{recallSum / recallQueryCount},
        QStringLiteral(
            "Mean recall@50 for fixture judgments against the probe-only F110K model."),
        QStringLiteral("ratio"),
        QStringLiteral("correctness"),
        evidenceScope);
    result.addFixtureComponentObservationMetric(
        QStringLiteral("search.component.fixture.mrr_at_10"),
        QList<double>{reciprocalRankSum / reciprocalRankQueryCount},
        QStringLiteral(
            "MRR@10 for fixture rank-1 judgments against the probe-only F110K model."),
        QStringLiteral("ratio"),
        QStringLiteral("correctness"),
        evidenceScope);
    result.addFixtureComponentObservationMetric(
        QStringLiteral("search.component.fixture.ndcg_at_10"),
        QList<double>{ndcgSum / ndcgQueryCount},
        QStringLiteral(
            "Binary NDCG@10 for explicit and scan-normalized fixture judgments against the probe-only F110K model."),
        QStringLiteral("ratio"),
        QStringLiteral("correctness"),
        evidenceScope);

    result.fixtureComponentObservation(
        QStringLiteral("search.component.fixture.oracle_independent_observed"),
        oracleValid,
        true,
        oracleValid,
        QStringLiteral(
            "Fixture judgments come from hashed JSONL expectations and a probe-local raw-fixture scan; this observation does not claim production-path Search correctness."),
        QJsonObject{
            {QStringLiteral("scope"), evidenceScope},
            {QStringLiteral("queryCount"), fixture.searchQueries.size()},
            {QStringLiteral("oracleEntityCount"), fixture.oracleEntities.size()},
        });
    result.fixtureComponentObservation(
        QStringLiteral("search.component.fixture.no_hot_path_network_observed"),
        true,
        0,
        0,
        QStringLiteral(
            "The observed fixture component path invokes only the in-memory MediaSearchModel over an already populated probe model."),
        QJsonObject{
            {QStringLiteral("scope"), evidenceScope},
            {QStringLiteral("measuredPath"), QStringLiteral("MediaSearchModel::setSearchText")},
        });
    result.fixtureComponentObservation(
        QStringLiteral("search.component.fixture.snapshot_no_duplicate_rows"),
        duplicateTop50Rows == 0,
        0,
        duplicateTop50Rows,
        QStringLiteral(
            "The probe-only result top-50 contains no duplicate entity IDs. Cross-session and production-path behavior are not claimed."),
        QJsonObject{{QStringLiteral("scope"), evidenceScope}});

    const SearchQuery &finalQuery = fixture.searchQueries.at(requiredHotSamples - 1);
    model.setSearchText(finalQuery.query);
    const QVector<QString> finalActual = searchResultIds(model);
    const QVector<QString> finalExpected = independentProductSnapshotMatches(
        fixture, finalQuery.query);
    result.fixtureComponentObservation(
        QStringLiteral("search.component.fixture.final_query_matches_snapshot"),
        finalActual == finalExpected,
        finalExpected.size(),
        finalActual.size(),
        QStringLiteral(
            "The final fixture query matched the complete ordered result set for the probe-injected immutable snapshot."),
        QJsonObject{
            {QStringLiteral("scope"), evidenceScope},
            {QStringLiteral("queryId"), finalQuery.id},
            {QStringLiteral("queryCategory"), finalQuery.category},
            {QStringLiteral("orderedResultSetEqual"), finalActual == finalExpected},
        });
}

void runMediaStoreProbe(
    ResultDocument &result,
    const Fixture &fixture,
    int iterations,
    const QString &profile)
{
    result.mark(QStringLiteral("fixture_ready"), 0, {
        {QStringLiteral("itemCount"), fixture.items.size()},
        {QStringLiteral("fixtureHash"), fixture.sha256},
    });

    MediaStore store;
    QElapsedTimer timer;
    timer.start();
    store.setQuery(QStringLiteral("library"), {}, fixture.items);
    const QList<double> initialPopulation {elapsedMilliseconds(timer)};
    result.mark(QStringLiteral("library_model_ready"));
    result.addInformationalLatency(
        QStringLiteral("desktop.media_store.initial_population"),
        initialPopulation,
        QStringLiteral("Initial deterministic fixture insertion into the current MediaStore"));
    result.invariant(
        QStringLiteral("desktop.media_store.fixture_coverage"),
        store.libraryModel()->rowCount() == fixture.items.size(),
        fixture.items.size(),
        store.libraryModel()->rowCount(),
        QStringLiteral("The source model must expose every deterministic fixture row."));

    QList<double> identicalRefreshSamples;
    const int refreshIterations = std::clamp(iterations / 4, 3, 20);
    for (int iteration = 0; iteration < refreshIterations; ++iteration) {
        timer.restart();
        store.setQuery(QStringLiteral("library"), {}, fixture.items);
        identicalRefreshSamples.push_back(elapsedMilliseconds(timer));
    }
    result.addInformationalLatency(
        QStringLiteral("desktop.media_store.identical_refresh"),
        identicalRefreshSamples,
        QStringLiteral("Re-applying an unchanged full model through MediaStore::setQuery"));

    QVariantList reversedItems = fixture.items;
    std::reverse(reversedItems.begin(), reversedItems.end());
    QList<double> fullReorderSamples;
    timer.restart();
    store.setQuery(QStringLiteral("library"), {}, reversedItems);
    fullReorderSamples.push_back(elapsedMilliseconds(timer));
    timer.restart();
    store.setQuery(QStringLiteral("library"), {}, fixture.items);
    fullReorderSamples.push_back(elapsedMilliseconds(timer));
    result.addInformationalLatency(
        QStringLiteral("desktop.media_store.full_reorder"),
        fullReorderSamples,
        QStringLiteral("Reverse and restore the complete source order with stable row keys"));

    MediaSearchModel proxy;
    proxy.setRequireSearchText(true);
    timer.restart();
    proxy.setSourceModel(store.libraryModel());
    result.addInformationalLatency(
        QStringLiteral("search.component.index_build_ms"),
        QList<double>{elapsedMilliseconds(timer)},
        QStringLiteral("Synchronous construction of the current in-memory search index"));

    struct CorrectnessCase {
        QString id;
        QString query;
        int expected = 0;
    };
    QList<CorrectnessCase> correctnessCases;
    const QString firstTitle = fixture.items.constFirst().toMap()
                                   .value(QStringLiteral("title")).toString();
    correctnessCases.push_back({
        QStringLiteral("exact_first_title"),
        firstTitle,
        fixtureOracleCount(fixture, firstTitle),
    });
    const QString prefix = firstTitle.left(2);
    correctnessCases.push_back({
        QStringLiteral("short_prefix"),
        prefix,
        fixtureOracleCount(fixture, prefix),
    });
    correctnessCases.push_back({
        QStringLiteral("no_result"),
        QStringLiteral("no-such-fixture-token"),
        0,
    });
    if (fixture.id == QStringLiteral("DesktopT10K-v1")
        || fixture.id == QStringLiteral("DesktopF110KLike-v1")) {
        correctnessCases += QList<CorrectnessCase>{
            {QStringLiteral("exact_chinese_title"), QStringLiteral("星际远航"), 1},
            {QStringLiteral("chinese_substring"), QStringLiteral("远航"), 1},
            {QStringLiteral("case_insensitive_title"), QStringLiteral("orbital"), 1},
            {QStringLiteral("subtitle"), QStringLiteral("银翼"), 1},
            {QStringLiteral("series_title"), QStringLiteral("银河列车"), 1},
            {QStringLiteral("unique_generated_title"), QStringLiteral("Fixture Title 000042"), 1},
        };
    }
    for (const CorrectnessCase &test : correctnessCases) {
        const int actual = matchCount(proxy, test.query);
        result.invariant(
            QStringLiteral("desktop.search.correctness.%1").arg(test.id),
            actual == test.expected,
            test.expected,
            actual,
            QStringLiteral("Current proxy exact/substring matching returned an unexpected count."));
    }

    const int pinyinMatches = matchCount(proxy, fixture.pinyinFullQuery);
    result.capability(
        QStringLiteral("desktop.search.capability.full_pinyin"),
        pinyinMatches == 1,
        1,
        pinyinMatches,
        QStringLiteral("SLO-v1 requires full-pinyin matching; missing support is recorded as debt until enforce mode."));
    const int initialsMatches = matchCount(proxy, fixture.pinyinInitialsQuery);
    result.capability(
        QStringLiteral("desktop.search.capability.pinyin_initials"),
        initialsMatches == 1,
        1,
        initialsMatches,
        QStringLiteral("SLO-v1 requires pinyin-initial matching; missing support is recorded as debt until enforce mode."));

    QList<QString> timedQueries {
        prefix,
        firstTitle,
        fixture.pinyinFullQuery,
        fixture.pinyinInitialsQuery,
        QStringLiteral("no-such-fixture-token"),
    };
    if (fixture.id == QStringLiteral("F110K-v1")) {
        timedQueries += QList<QString>{
            QStringLiteral("第01集"),
            QStringLiteral("归航"),
            QStringLiteral("The Archive"),
        };
    } else {
        timedQueries += QList<QString>{
            QStringLiteral("fixture"),
            QStringLiteral("orbital"),
            QStringLiteral("银翼"),
        };
    }
    for (int warmup = 0; warmup < 3; ++warmup)
        matchCount(proxy, timedQueries.at(warmup));

    QList<double> searchSamples;
    quint64 generation = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const QString query = timedQueries.at(iteration % timedQueries.size());
        ++generation;
        result.mark(QStringLiteral("search_input_committed"), generation, {
            {QStringLiteral("queryClass"), iteration % timedQueries.size()},
        });
        timer.restart();
        proxy.setSearchText(query);
        const int count = proxy.rowCount();
        searchSamples.push_back(elapsedMilliseconds(timer));
        result.mark(QStringLiteral("search_result_model_committed"), generation, {
            {QStringLiteral("resultCount"), count},
        });
    }
    result.addInformationalLatency(
            QStringLiteral("search.component.proxy_update_ms"),
            searchSamples,
            QStringLiteral("Search text update through result-model availability on the current proxy"));

    runF110KSearchComponentObservation(result, fixture, proxy, profile);

    QVariantList incrementalPatches;
    const int incrementalCount = static_cast<int>(
        std::min<qsizetype>(1'000, fixture.items.size()));
    incrementalPatches.reserve(incrementalCount);
    for (int index = 0; index < incrementalCount; ++index) {
        const QVariantMap source = fixture.items.at(index).toMap();
        incrementalPatches.push_back(QVariantMap{
            {QStringLiteral("id"), source.value(QStringLiteral("id"))},
            {QStringLiteral("title"),
             source.value(QStringLiteral("title")).toString()
                 + QStringLiteral(" Updated")},
            {QStringLiteral("imageUrl"), QStringLiteral("updated.jpg")},
        });
    }
    timer.restart();
    store.patchEntities(incrementalPatches);
    result.addInformationalLatency(
        QStringLiteral("search.component.incremental_1000_ms"),
        QList<double>{elapsedMilliseconds(timer)},
        QStringLiteral("Batched update of 1,000 searchable entities in the current in-memory model"));
}

void runGenerationProbe(ResultDocument &result, int iterations)
{
    RequestCoordinator coordinator;
    QList<double> samples;
    QList<LatestRequestToken> tokens;
    tokens.reserve(iterations);
    QElapsedTimer timer;
    constexpr quint64 sessionGeneration = 17;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        timer.restart();
        tokens.push_back(coordinator.beginLatest(
            QStringLiteral("search.live"),
            QStringLiteral("query-%1").arg(iteration),
            sessionGeneration));
        coordinator.acceptsLatest(tokens.constLast(), sessionGeneration);
        samples.push_back(elapsedMilliseconds(timer));
    }

    int acceptedStaleResults = 0;
    for (qsizetype index = 0; index + 1 < tokens.size(); ++index) {
        if (coordinator.acceptsLatest(tokens.at(index), sessionGeneration))
            ++acceptedStaleResults;
    }
    const bool latestAccepted = !tokens.isEmpty()
        && coordinator.acceptsLatest(tokens.constLast(), sessionGeneration);
    result.invariant(
        QStringLiteral("desktop.request.latest_wins.stale_commit"),
        acceptedStaleResults == 0,
        0,
        acceptedStaleResults,
        QStringLiteral("Superseded generations must not be accepted for presentation."));
    result.invariant(
        QStringLiteral("desktop.request.latest_wins.final_commit"),
        latestAccepted,
        true,
        latestAccepted,
        QStringLiteral("The newest generation must remain eligible for presentation."));
    result.addInformationalLatency(
        QStringLiteral("desktop.request.latest_wins.token"),
        samples,
        QStringLiteral("RequestCoordinator beginLatest plus acceptance check"));
}

class ProbeStatusSink final : public StatusSink
{
public:
    void publishStatus(const QString &, bool) override {}

    QString userFacingBackendError(
        const QString &,
        const QString & = {}) const override
    {
        return QStringLiteral("controlled search backend failure");
    }

    QString userFacingDanmakuError(
        const QString &,
        const QString & = {}) const override
    {
        return QStringLiteral("controlled danmaku backend failure");
    }
};

QJsonArray stringArray(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.push_back(value);
    return result;
}

YanamiOperationResult searchResponse(
    const QString &query,
    const QStringList &itemIds,
    const QString &catalogRevision = QStringLiteral("perf-revision"))
{
    QJsonObject entities;
    QJsonArray rows;
    for (qsizetype index = 0; index < itemIds.size(); ++index) {
        const QString &itemId = itemIds.at(index);
        const bool episode = index % 2 != 0;
        QJsonObject entity {
            {QStringLiteral("id"), itemId},
            {QStringLiteral("title"), itemId},
            {QStringLiteral("itemType"), episode
                 ? QStringLiteral("Episode") : QStringLiteral("Movie")},
            {QStringLiteral("imageTag"), episode
                 ? query + QStringLiteral("-series-poster")
                 : itemId + QStringLiteral("-poster")},
            {QStringLiteral("sourceVersion"), QStringLiteral("perf")},
        };
        if (episode) {
            entity.insert(QStringLiteral("imageItemId"),
                query + QStringLiteral("-series-owner"));
            entity.insert(QStringLiteral("imageItemType"),
                QStringLiteral("Series"));
            entity.insert(QStringLiteral("seriesTitle"),
                QStringLiteral("controlled-series"));
            entity.insert(QStringLiteral("subtitle"),
                QStringLiteral("S01E01 · controlled-episode"));
            entity.insert(QStringLiteral("titleIsContextual"), true);
        }
        entities.insert(itemId, entity);
        rows.push_back(QJsonObject {
            {QStringLiteral("rowKey"), itemId},
            {QStringLiteral("entityId"), itemId},
            {QStringLiteral("decoration"), QJsonObject {}},
        });
    }
    const QJsonObject document {
        {QStringLiteral("schemaVersion"), 8},
        {QStringLiteral("entities"), entities},
        {QStringLiteral("queries"), QJsonObject {
             {QStringLiteral("search"), QJsonObject {
                  {QStringLiteral("scopeId"), query},
                  {QStringLiteral("parentId"), QString()},
                  {QStringLiteral("parentDecoration"), QJsonObject {}},
                  {QStringLiteral("rows"), rows},
              }},
         }},
         {QStringLiteral("searchStatus"), QJsonObject {
              {QStringLiteral("query"), query},
              {QStringLiteral("catalogRevision"), catalogRevision},
             {QStringLiteral("cachedCount"), 140'000},
             {QStringLiteral("totalCount"), 140'000},
             {QStringLiteral("totalMatches"), itemIds.size()},
             {QStringLiteral("hasMore"), false},
             {QStringLiteral("complete"), true},
             {QStringLiteral("syncing"), false},
             {QStringLiteral("indexError"), false},
         }},
    };
    YanamiOperationResult result;
    result.status = 0;
    result.payload = QJsonDocument(document).toJson(QJsonDocument::Compact);
    return result;
}

class DelayedSearchBackend final
{
public:
    YanamiOperationResult search(const QString &query)
    {
        const int active = m_active.fetch_add(1, std::memory_order_acq_rel) + 1;
        int maximum = m_maximumActive.load(std::memory_order_relaxed);
        while (active > maximum
               && !m_maximumActive.compare_exchange_weak(
                   maximum, active, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
        {
            const std::lock_guard lock(m_mutex);
            m_invoked.push_back(query);
        }

        bool released = true;
        if (query == QStringLiteral("alpha")) {
            firstStarted.release();
            released = releaseFirst.tryAcquire(1, 10'000);
        } else if (query == QStringLiteral("old-session")) {
            oldSessionStarted.release();
            released = releaseOldSession.tryAcquire(1, 10'000);
        }

        QStringList ids;
        if (query == QStringLiteral("alpha")) {
            ids = {QStringLiteral("s1-alpha")};
        } else if (query == QStringLiteral("gamma")) {
            ids = {QStringLiteral("s1-gamma-a"), QStringLiteral("s1-gamma-b")};
        } else if (query == QStringLiteral("old-session")) {
            ids = {QStringLiteral("s1-old")};
        } else if (query == QStringLiteral("final")) {
            ids.reserve(50);
            for (int index = 0; index < 50; ++index) {
                ids.push_back(QStringLiteral("s2-final-%1")
                    .arg(index, 2, 10, QLatin1Char('0')));
            }
        } else {
            ids = {QStringLiteral("unexpected-%1").arg(query)};
        }

        m_active.fetch_sub(1, std::memory_order_acq_rel);
        if (released)
            return searchResponse(query, ids);
        YanamiOperationResult failure;
        failure.status = 1;
        failure.errorCode = QStringLiteral("controlled_timeout");
        failure.error = QStringLiteral("controlled search release timed out");
        return failure;
    }

    QStringList invoked() const
    {
        const std::lock_guard lock(m_mutex);
        return m_invoked;
    }

    int maximumActive() const
    {
        return m_maximumActive.load(std::memory_order_acquire);
    }

    QSemaphore firstStarted;
    QSemaphore releaseFirst;
    QSemaphore oldSessionStarted;
    QSemaphore releaseOldSession;

private:
    mutable std::mutex m_mutex;
    QStringList m_invoked;
    std::atomic<int> m_active {0};
    std::atomic<int> m_maximumActive {0};
};

struct CapturedSearchEvent
{
    quint64 sequence = 0;
    qint64 monotonicNs = 0;
    SearchCoordinatorEvent event;
};

class SearchEventCollector final
{
public:
    SearchEventCollector()
        : m_started(std::chrono::steady_clock::now())
    {
    }

    void capture(const SearchCoordinatorEvent &event)
    {
        const auto elapsed = std::chrono::steady_clock::now() - m_started;
        CapturedSearchEvent captured {
            .sequence = m_sequence.fetch_add(1, std::memory_order_relaxed),
            .monotonicNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                elapsed).count(),
            .event = event,
        };
        const std::lock_guard lock(m_mutex);
        m_events.push_back(std::move(captured));
    }

    QList<CapturedSearchEvent> snapshot() const
    {
        const std::lock_guard lock(m_mutex);
        QList<CapturedSearchEvent> result = m_events;
        std::sort(result.begin(), result.end(),
            [](const auto &left, const auto &right) {
                return left.sequence < right.sequence;
            });
        return result;
    }

private:
    std::chrono::steady_clock::time_point m_started;
    mutable std::mutex m_mutex;
    QList<CapturedSearchEvent> m_events;
    std::atomic<quint64> m_sequence {0};
};

bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 5'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() >= timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return true;
}

void runSearchCoordinatorProbe(ResultDocument &result)
{
    DelayedSearchBackend backend;
    SearchEventCollector collector;
    ProbeStatusSink statusSink;
    QThreadPool queryPool;
    QThreadPool hydrationPool;
    queryPool.setMaxThreadCount(2);
    hydrationPool.setMaxThreadCount(1);
    SearchSessionState session {.generation = 1, .connected = true};
    std::mutex hydrationMutex;
    QVariantMap finalHydrationPayload;
    std::atomic<bool> finalHydrationCaptured {false};
    std::atomic<int> finalHydrationCount {0};
    SearchBackendOperations operations {
        .ready = [] { return true; },
        .searchCatalog = [&backend](const QString &query) {
            return backend.search(query);
        },
        .hydrateCatalogSearchImages = [&](const QVariantMap &payload) {
            if (payload.value(QStringLiteral("query")).toString()
                == QStringLiteral("final")) {
                const std::lock_guard lock(hydrationMutex);
                finalHydrationPayload = payload;
                finalHydrationCaptured.store(true, std::memory_order_release);
                finalHydrationCount.fetch_add(1, std::memory_order_release);
            }
            YanamiOperationResult response;
            response.status = 0;
            return response;
        },
    };
    SearchCoordinator coordinator(
        std::move(operations),
        queryPool,
        hydrationPool,
        [&session] { return session; },
        statusSink,
        [&collector](const SearchCoordinatorEvent &event) {
            collector.capture(event);
        });

    QString infrastructureError;
    if (!coordinator.initializeFromSession()) {
        infrastructureError = QStringLiteral(
            "the controlled SearchCoordinator session did not initialize");
    }
    if (infrastructureError.isEmpty()) {
        coordinator.requestSearch(QStringLiteral(" alpha "));
        if (!backend.firstStarted.tryAcquire(1, 2'000)) {
            infrastructureError = QStringLiteral(
                "the first delayed query did not enter the worker");
        }
    }
    if (infrastructureError.isEmpty()) {
        coordinator.requestSearch(QStringLiteral("beta"));
        coordinator.requestSearch(QStringLiteral("gamma"));
        backend.releaseFirst.release();
        const bool gammaPublished = spinUntil([&coordinator] {
            MediaQueryModel *model = coordinator.resultsModel();
            MediaQueryModel *titles = coordinator.titleResultsModel();
            MediaQueryModel *episodes = coordinator.episodeResultsModel();
            return !coordinator.searching()
                && coordinator.query() == QStringLiteral("gamma")
                && model && model->rowCount() == 2
                && titles && titles->rowCount() == 1
                && episodes && episodes->rowCount() == 1
                && model->get(0).value(QStringLiteral("id")).toString()
                    == QStringLiteral("s1-gamma-a")
                && titles->get(0).value(QStringLiteral("id")).toString()
                    == QStringLiteral("s1-gamma-a")
                && episodes->get(0).value(QStringLiteral("id")).toString()
                    == QStringLiteral("s1-gamma-b");
        });
        if (!gammaPublished) {
            infrastructureError = QStringLiteral(
                "the final query of the first burst did not publish");
        }
    }
    if (infrastructureError.isEmpty()) {
        coordinator.requestSearch(QStringLiteral("old-session"));
        if (!backend.oldSessionStarted.tryAcquire(1, 2'000)) {
            infrastructureError = QStringLiteral(
                "the delayed prior-session query did not enter the worker");
        }
    }
    if (infrastructureError.isEmpty()) {
        coordinator.sessionTransitionStarted();
        session = {.generation = 2, .connected = true};
        coordinator.sessionCommitted();
        coordinator.requestSearch(QStringLiteral("final"));
        backend.releaseOldSession.release();
        const bool finalPublished = spinUntil([&coordinator] {
            MediaQueryModel *model = coordinator.resultsModel();
            MediaQueryModel *titles = coordinator.titleResultsModel();
            MediaQueryModel *episodes = coordinator.episodeResultsModel();
            return !coordinator.searching()
                && coordinator.query() == QStringLiteral("final")
                && model && model->rowCount() == 50
                && titles && titles->rowCount() == 25
                && episodes && episodes->rowCount() == 25
                && model->get(0).value(QStringLiteral("id")).toString()
                    == QStringLiteral("s2-final-00")
                && model->get(49).value(QStringLiteral("id")).toString()
                    == QStringLiteral("s2-final-49")
                && titles->get(24).value(QStringLiteral("id")).toString()
                    == QStringLiteral("s2-final-48")
                && episodes->get(24).value(QStringLiteral("id")).toString()
                    == QStringLiteral("s2-final-49");
        });
        if (!finalPublished) {
            infrastructureError = QStringLiteral(
                "the new-session final Top50 did not publish");
        }
    }
    if (infrastructureError.isEmpty()
        && !spinUntil([&finalHydrationCaptured] {
            return finalHydrationCaptured.load(std::memory_order_acquire);
        }, 2'000)) {
        infrastructureError = QStringLiteral(
            "the stable final query did not request image hydration");
    }

    bool unchangedRevisionPollCompleted = false;
    if (infrastructureError.isEmpty()) {
        coordinator.refresh();
        unchangedRevisionPollCompleted = spinUntil([&] {
            // A third invocation can start only after the first two independent
            // status watcher completions ran. All return the committed revision.
            coordinator.refresh();
            return backend.invoked().count(QString()) >= 3;
        }, 2'000);
        if (!unchangedRevisionPollCompleted) {
            infrastructureError = QStringLiteral(
                "unchanged catalog revision status polls did not complete");
        }
    }

    backend.releaseFirst.release();
    backend.releaseOldSession.release();
    coordinator.shutdown();
    coordinator.drain();
    queryPool.waitForDone();
    hydrationPool.waitForDone();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    const QList<CapturedSearchEvent> events = collector.snapshot();
    QStringList publishedQueries;
    QStringList discardedQueries;
    QStringList startedQueries;
    int submittedCount = 0;
    int finishedCount = 0;
    int maximumActive = 0;
    int maximumQueued = 0;
    bool sessionRowsIsolated = true;
    bool publishedRowsUnique = true;
    int finalPublishCount = 0;
    QStringList finalPublishedIds;
    for (const CapturedSearchEvent &captured : events) {
        const SearchCoordinatorEvent &event = captured.event;
        maximumActive = std::max(maximumActive, event.activeCount);
        maximumQueued = std::max(maximumQueued, event.queuedCount);
        if (event.milestone == QStringLiteral("query_submitted"))
            ++submittedCount;
        else if (event.milestone == QStringLiteral("worker_started"))
            startedQueries.push_back(event.query);
        else if (event.milestone == QStringLiteral("worker_finished"))
            ++finishedCount;
        else if (event.milestone == QStringLiteral("request_discarded"))
            discardedQueries.push_back(event.query);
        else if (event.milestone == QStringLiteral("publish_committed")) {
            publishedQueries.push_back(event.query);
            QSet<QString> unique(event.publishedItemIds.cbegin(),
                event.publishedItemIds.cend());
            publishedRowsUnique = publishedRowsUnique
                && unique.size() == event.publishedItemIds.size();
            const QString expectedPrefix = event.sessionGeneration == 1
                ? QStringLiteral("s1-")
                : event.sessionGeneration == 2
                    ? QStringLiteral("s2-") : QString();
            sessionRowsIsolated = sessionRowsIsolated
                && !expectedPrefix.isEmpty()
                && std::all_of(event.publishedItemIds.cbegin(),
                    event.publishedItemIds.cend(),
                    [&expectedPrefix](const QString &itemId) {
                        return itemId.startsWith(expectedPrefix);
                    });
            if (event.query == QStringLiteral("final")) {
                ++finalPublishCount;
                finalPublishedIds = event.publishedItemIds;
            }
        }
        result.mark(event.milestone, event.sessionGeneration, {
            {QStringLiteral("evidence"),
             QStringLiteral("production-search-coordinator-delayed-fake")},
            {QStringLiteral("sequence"), static_cast<qint64>(captured.sequence)},
            {QStringLiteral("capturedMonotonicNs"), captured.monotonicNs},
            {QStringLiteral("requestGeneration"),
             static_cast<qint64>(event.requestGeneration)},
            {QStringLiteral("normalizedQuery"), event.query},
            {QStringLiteral("activeCount"), event.activeCount},
            {QStringLiteral("queueDepth"), event.queuedCount},
            {QStringLiteral("reason"), event.reason},
            {QStringLiteral("publishedItemIds"), stringArray(event.publishedItemIds)},
        });
    }

    if (!infrastructureError.isEmpty()) {
        result.infrastructureFailure(
            QStringLiteral("search.production_coordinator_harness"),
            infrastructureError);
    }
    const bool noStaleCommit = publishedQueries
            == QStringList {QStringLiteral("gamma"), QStringLiteral("final")}
        && discardedQueries.contains(QStringLiteral("alpha"))
        && discardedQueries.contains(QStringLiteral("beta"))
        && discardedQueries.contains(QStringLiteral("old-session"));
    QStringList expectedFinalIds;
    expectedFinalIds.reserve(50);
    for (int index = 0; index < 50; ++index) {
        expectedFinalIds.push_back(QStringLiteral("s2-final-%1")
            .arg(index, 2, 10, QLatin1Char('0')));
    }
    const bool finalQueryComplete = finalPublishCount == 1
        && finalPublishedIds == expectedFinalIds;
    const bool queueBounded = maximumActive <= 1 && maximumQueued <= 1
        && backend.maximumActive() <= 1
        && !startedQueries.contains(QStringLiteral("beta"));
    const bool noDuplicateOrCrossSession = publishedRowsUnique
        && sessionRowsIsolated && finalPublishedIds == expectedFinalIds;
    QVariantList hydrationItems;
    {
        const std::lock_guard lock(hydrationMutex);
        hydrationItems = finalHydrationPayload.value(
            QStringLiteral("items")).toList();
    }
    bool explicitEpisodeOwner = false;
    bool legacyTitleFallback = false;
    for (const QVariant &value : hydrationItems) {
        const QVariantMap item = value.toMap();
        explicitEpisodeOwner = explicitEpisodeOwner
            || (item.value(QStringLiteral("itemId")).toString()
                    == QStringLiteral("final-series-owner")
                && item.value(QStringLiteral("itemType")).toString()
                    == QStringLiteral("Series")
                && item.value(QStringLiteral("imageTag")).toString()
                    == QStringLiteral("final-series-poster"));
        legacyTitleFallback = legacyTitleFallback
            || (item.value(QStringLiteral("itemId")).toString()
                    == QStringLiteral("s2-final-00")
                && item.value(QStringLiteral("itemType")).toString()
                    == QStringLiteral("Movie")
                && item.value(QStringLiteral("imageTag")).toString()
                    == QStringLiteral("s2-final-00-poster"));
    }
    const bool imageOwnerHydrationContract = explicitEpisodeOwner
        && legacyTitleFallback && hydrationItems.size() == 26;
    const QStringList backendInvocations = backend.invoked();
    const int observedFinalHydrationCount = finalHydrationCount.load(
        std::memory_order_acquire);
    const bool unchangedRevisionNoWork = unchangedRevisionPollCompleted
        && backendInvocations.count(QString()) >= 3
        && backendInvocations.count(QStringLiteral("final")) == 1
        && finalPublishCount == 1
        && observedFinalHydrationCount == 1;
    const QJsonObject commonDetails {
        {QStringLiteral("evidence"),
         QStringLiteral("production-search-coordinator-delayed-fake")},
        {QStringLiteral("submittedCount"), submittedCount},
        {QStringLiteral("startedCount"), startedQueries.size()},
        {QStringLiteral("finishedCount"), finishedCount},
        {QStringLiteral("publishedCount"), publishedQueries.size()},
        {QStringLiteral("discardedCount"), discardedQueries.size()},
        {QStringLiteral("maximumActive"), maximumActive},
        {QStringLiteral("maximumQueueDepth"), maximumQueued},
        {QStringLiteral("backendMaximumConcurrent"), backend.maximumActive()},
        {QStringLiteral("submittedQueries"),
         stringArray({QStringLiteral("alpha"), QStringLiteral("beta"),
             QStringLiteral("gamma"), QStringLiteral("old-session"),
             QStringLiteral("final")})},
        {QStringLiteral("startedQueries"), stringArray(startedQueries)},
        {QStringLiteral("publishedQueries"), stringArray(publishedQueries)},
        {QStringLiteral("discardedQueries"), stringArray(discardedQueries)},
        {QStringLiteral("backendInvokedQueries"), stringArray(backendInvocations)},
        {QStringLiteral("finalPublishedIds"), stringArray(finalPublishedIds)},
        {QStringLiteral("hydrationItemCount"), hydrationItems.size()},
        {QStringLiteral("finalHydrationCount"), observedFinalHydrationCount},
    };
    result.invariant(
        QStringLiteral("search.no_stale_commit"),
        noStaleCommit,
        true,
        noStaleCommit,
        QStringLiteral("Superseded and prior-session search responses never publish."),
        commonDetails);
    result.invariant(
        QStringLiteral("search.final_query_complete"),
        finalQueryComplete,
        50,
        finalPublishedIds.size(),
        QStringLiteral("The last submitted new-session query publishes its complete Top50 once."),
        commonDetails);
    result.invariant(
        QStringLiteral("search.image_owner_hydration_contract"),
        imageOwnerHydrationContract,
        true,
        imageOwnerHydrationContract,
        QStringLiteral("Search hydration uses the explicit image owner and preserves the legacy item fallback."),
        commonDetails);
    result.invariant(
        QStringLiteral("search.unchanged_revision_no_work"),
        unchangedRevisionNoWork,
        true,
        unchangedRevisionNoWork,
        QStringLiteral("Repeated empty-query status polls at the committed revision do not republish rows, repeat the full query, or repeat hydration."),
        commonDetails);
    result.invariant(
        QStringLiteral("search.queue_bounded"),
        queueBounded,
        QJsonObject {{QStringLiteral("active"), 1},
                     {QStringLiteral("queued"), 1}},
        QJsonObject {{QStringLiteral("active"), maximumActive},
                     {QStringLiteral("queued"), maximumQueued}},
        QStringLiteral("The production coordinator keeps one active and one latest queued query."),
        commonDetails);
    result.invariant(
        QStringLiteral("search.no_duplicates_or_cross_session_results"),
        noDuplicateOrCrossSession,
        true,
        noDuplicateOrCrossSession,
        QStringLiteral("Published result IDs are unique and belong to their request session."),
        commonDetails);
}

QList<double> measureWorkerStarts(
    QThreadPool &pool,
    int iterations,
    bool *valid)
{
    struct StartState {
        QElapsedTimer timer;
        std::atomic<qint64> startedNs {-1};
        QSemaphore started;
    };
    QList<double> samples;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        auto state = std::make_shared<StartState>();
        state->timer.start();
        pool.start([state] {
            state->startedNs.store(
                state->timer.nsecsElapsed(), std::memory_order_release);
            state->started.release();
        });
        if (!state->started.tryAcquire(1, 2'000)) {
            *valid = false;
            break;
        }
        samples.push_back(
            state->startedNs.load(std::memory_order_acquire) / 1'000'000.0);
    }
    return samples;
}

void runWorkerLaneProbe(ResultDocument &result, int iterations)
{
    WorkerPools pools;
    QSemaphore catalogStarted;
    QSemaphore releaseCatalog;
    for (int index = 0; index < 2; ++index) {
        pools.catalog().start([&] {
            catalogStarted.release();
            releaseCatalog.acquire();
        });
    }
    if (!catalogStarted.tryAcquire(2, 2'000)) {
        releaseCatalog.release(2);
        result.infrastructureFailure(
            QStringLiteral("desktop.worker.catalog_saturation"),
            QStringLiteral("The controlled catalog blockers did not start within two seconds."));
        pools.drain();
        return;
    }
    result.mark(QStringLiteral("catalog_lane_saturated"));

    bool workerValid = true;
    const int workerIterations = std::clamp(iterations, 10, 50);
    const QList<double> mediaReadSamples = measureWorkerStarts(
        pools.mediaRead(), workerIterations, &workerValid);
    if (!workerValid) {
        result.infrastructureFailure(
            QStringLiteral("desktop.worker.media_read_probe"),
            QStringLiteral("The media-read task did not start within two seconds."));
    } else {
        result.addInformationalLatency(
            QStringLiteral("desktop.worker.media_read_start_while_catalog_blocked_ms"),
            mediaReadSamples,
            QStringLiteral("Media-read worker start delay while both catalog workers are blocked"));
    }

    workerValid = true;
    const QList<double> playbackSamples = measureWorkerStarts(
        pools.playbackPrepare(), std::clamp(iterations / 2, 5, 20), &workerValid);
    if (!workerValid) {
        result.infrastructureFailure(
            QStringLiteral("desktop.worker.playback_probe"),
            QStringLiteral("The playback-preparation task did not start within two seconds."));
    } else {
        result.addInformationalLatency(
            QStringLiteral("desktop.worker.playback_start_while_catalog_blocked"),
            playbackSamples,
            QStringLiteral("Playback-preparation worker start delay while catalog is blocked"));
    }

    QList<double> eventLoopSamples;
    for (int iteration = 0; iteration < workerIterations; ++iteration) {
        QElapsedTimer timer;
        QEventLoop loop;
        timer.start();
        QTimer::singleShot(0, &loop, [&] {
            eventLoopSamples.push_back(elapsedMilliseconds(timer));
            loop.quit();
        });
        loop.exec();
    }
    result.addInformationalLatency(
        QStringLiteral("desktop.event_loop.timer0_wait_while_catalog_blocked_ms"),
        eventLoopSamples,
        QStringLiteral("Main event-loop queued callback latency during controlled catalog saturation"));

    releaseCatalog.release(2);
    pools.drain();
    result.mark(QStringLiteral("worker_lane_probe_finished"));
}

bool writeResult(const QString &path, const QJsonObject &document, QString *error)
{
    const QFileInfo output(path);
    if (!QDir().mkpath(output.absolutePath())) {
        *error = QStringLiteral("Unable to create output directory: %1")
                     .arg(output.absolutePath());
        return false;
    }
    QSaveFile file(output.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = file.errorString();
        return false;
    }
    const QByteArray encoded = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size()) {
        *error = file.errorString();
        return false;
    }
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("yanami-desktop-perf-probe"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Collect deterministic Yanami desktop performance measurements."));
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("p"), QStringLiteral("profile")},
        QStringLiteral("Workload profile: pull-request, lab, nightly, weekly, or release."),
        QStringLiteral("profile"),
        QStringLiteral("pr")));
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("m"), QStringLiteral("mode")},
        QStringLiteral("Gate mode: collect, debt, or enforce."),
        QStringLiteral("mode"),
        QStringLiteral("collect")));
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("o"), QStringLiteral("output")},
        QStringLiteral("Path for the versioned JSON result."),
        QStringLiteral("path"),
        QStringLiteral("desktop-run-manifest.json")));
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("fixture-dir")},
        QStringLiteral("Validated F110K directory for non-PR profiles."),
        QStringLiteral("directory")));
    parser.process(app);

    QString profile = parser.value(QStringLiteral("profile")).trimmed().toLower();
    if (qEnvironmentVariableIntValue("YANAMI_PERF_WEEKLY") == 1)
        profile = QStringLiteral("weekly");
    const QString mode = parser.value(QStringLiteral("mode")).trimmed().toLower();
    const QSet<QString> modes {
        QStringLiteral("collect"), QStringLiteral("debt"), QStringLiteral("enforce"),
    };
    if (canonicalProfile(profile).isEmpty() || !modes.contains(mode)) {
        qCritical().noquote()
            << "invalid profile or mode; expected pull-request|lab|nightly|weekly|release "
               "and collect|debt|enforce";
        return 2;
    }

    QString fixtureDirectory = parser.value(QStringLiteral("fixture-dir")).trimmed();
    if (fixtureDirectory.isEmpty()) {
        fixtureDirectory = QString::fromLocal8Bit(
            qgetenv("YANAMI_PERF_FIXTURE_DIR")).trimmed();
    }
    QString fixtureError;
    std::optional<Fixture> fixtureValue = makeFixture(
        profile, fixtureDirectory, &fixtureError);
    if (!fixtureValue) {
        qCritical().noquote() << "invalid performance fixture:" << fixtureError;
        return 2;
    }
    Fixture fixture = std::move(*fixtureValue);
    const QString outputProfile = canonicalProfile(profile);
    const int iterations = outputProfile == QStringLiteral("PullRequest") ? 32
        : outputProfile == QStringLiteral("Lab") ? 80
        : outputProfile == QStringLiteral("Release") ? 160
        : 120;
    ResultDocument result(profile, mode, fixture);
    result.mark(QStringLiteral("probe_started"));
    runMediaStoreProbe(result, fixture, iterations, profile);
    runGenerationProbe(result, std::max(iterations, 100));
    runSearchCoordinatorProbe(result);
    runWorkerLaneProbe(result, iterations);
    result.mark(QStringLiteral("probe_finished"));

    const QJsonObject document = result.json();
    QString writeError;
    const QString outputPath = parser.value(QStringLiteral("output"));
    if (!writeResult(outputPath, document, &writeError)) {
        qCritical().noquote() << "failed to write performance result:" << writeError;
        return 2;
    }
    qInfo().noquote()
        << "performance result" << outputPath
        << "status=" << statusName(result.severity());

    if (result.severity() == ResultSeverity::InfrastructureInvalid)
        return 2;
    if (result.severity() == ResultSeverity::Fail)
        return 1;
    return 0;
}
