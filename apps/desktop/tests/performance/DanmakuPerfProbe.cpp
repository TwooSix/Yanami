#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QAbstractItemModel>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSValue>
#include <QMetaMethod>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSet>
#include <QSGRendererInterface>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVariantList>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

#ifndef YANAMI_PERF_BUILD_TYPE
#define YANAMI_PERF_BUILD_TYPE "unknown"
#endif

#ifndef YANAMI_PERF_VERSION
#define YANAMI_PERF_VERSION "unknown"
#endif

namespace {
constexpr int hostedCommentCount = 20'000;
constexpr int strictCommentCount = 100'000;
// R7 p95 needs 21 observations for one isolated cold sample to remain under
// the separately enforced max statistic instead of dominating the percentile.
// This yields one cold generation plus twenty repeated dense-seek generations.
constexpr int loadGenerationCount = 21;
constexpr int maximumTextureCommitFrames = 3;

struct Fixture
{
    QVariantList hostedComments;
    QVariantList strictComments;
    QHash<QString, QVariantMap> byId;
    QString sha256;
    int totalCount = 0;
};

struct TimelineInspection
{
    bool valid = false;
    int count = 0;
    QString fingerprint;
    QString error;
};

struct DelegateSnapshot
{
    int delegateCount = 0;
    QStringList visibleIds;
    bool unique = true;
    bool insideTimeWindow = true;
    bool filterValid = true;
};

struct OracleEntry
{
    QString id;
    double start = 0.0;
    QString mode;
    QString text;
    int lane = 0;
    int densityRank = 0;
    int inputOrdinal = 0;
};

struct OracleStyle
{
    double width = 1920.0;
    double height = 1080.0;
    double fontSize = 36.0;
    double laneLayoutFontSize = 36.0;
    double scrollDuration = 9.0;
    double laneLayoutScrollDuration = 9.0;
    double displayArea = 1.0;
    double topMargin = 0.0;
    double timeOffset = 0.0;
    int density = 30;
    QStringList blockedTerms;
    bool showScroll = true;
    bool showTop = true;
    bool showBottom = true;
};

struct ExactSetInspection
{
    bool frameReady = false;
    bool exact = false;
    bool unique = true;
    bool noEarlyOrStale = true;
    QStringList expectedIds;
    QStringList actualIds;
    QStringList missingIds;
    QStringList unexpectedIds;
};

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

QString fileSha256(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        *error = file.errorString();
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool loadFixture(
    const QString &directory,
    const QString &expectedSha256,
    Fixture *fixture,
    QString *error)
{
    if (directory.isEmpty() || expectedSha256.isEmpty()) {
        *error = QStringLiteral(
            "YANAMI_PERF_DANMAKU_FIXTURE_DIR and YANAMI_PERF_DANMAKU_SHA256 are required.");
        return false;
    }
    const QString path = QDir(directory).filePath(
        QStringLiteral("danmaku-comments-v1.jsonl"));
    const QString actualSha256 = fileSha256(path, error);
    if (actualSha256.isEmpty())
        return false;
    if (actualSha256.compare(expectedSha256, Qt::CaseInsensitive) != 0) {
        *error = QStringLiteral("fixture SHA-256 mismatch: expected %1, got %2")
                     .arg(expectedSha256, actualSha256);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = file.errorString();
        return false;
    }
    fixture->hostedComments.reserve(hostedCommentCount);
    fixture->strictComments.reserve(strictCommentCount);
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        ++fixture->totalCount;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            *error = QStringLiteral("invalid fixture JSON at record %1: %2")
                         .arg(fixture->totalCount)
                         .arg(parseError.errorString());
            return false;
        }
        const QVariantMap comment = document.object().toVariantMap();
        const QString id = comment.value(QStringLiteral("id")).toString();
        const QString mode = comment.value(QStringLiteral("mode")).toString();
        const QString text = comment.value(QStringLiteral("text")).toString();
        bool timeValid = false;
        const double time = comment.value(QStringLiteral("time")).toDouble(&timeValid);
        if (id.isEmpty() || fixture->byId.contains(id)
            || mode.isEmpty() || text.trimmed().isEmpty()
            || !timeValid || !std::isfinite(time) || time < 0) {
            *error = QStringLiteral("invalid fixture record '%1'").arg(id);
            return false;
        }
        if (fixture->hostedComments.size() < hostedCommentCount)
            fixture->hostedComments.push_back(comment);
        fixture->strictComments.push_back(comment);
        fixture->byId.insert(id, comment);
    }
    if (fixture->totalCount != strictCommentCount
        || fixture->hostedComments.size() != hostedCommentCount
        || fixture->strictComments.size() != strictCommentCount) {
        *error = QStringLiteral("fixture count mismatch: expected %1/%2, got %3/%4/%5")
                     .arg(strictCommentCount)
                     .arg(hostedCommentCount)
                     .arg(fixture->totalCount)
                     .arg(fixture->hostedComments.size())
                     .arg(fixture->strictComments.size());
        return false;
    }
    fixture->sha256 = actualSha256.toLower();
    return true;
}

QString rendererName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Software:
        return QStringLiteral("Software");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("OpenGL");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("Direct3D11");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("Direct3D12");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("Vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("Metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("Null");
    case QSGRendererInterface::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

bool waitForFrame(QQuickWindow *window, int timeoutMs, double *elapsedMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QElapsedTimer clock;
    bool swapped = false;
    const QMetaObject::Connection frameConnection = QObject::connect(
        window,
        &QQuickWindow::frameSwapped,
        &loop,
        [&] {
            swapped = true;
            loop.quit();
        },
        Qt::QueuedConnection);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    clock.start();
    timeout.start(timeoutMs);
    window->requestUpdate();
    loop.exec();
    QObject::disconnect(frameConnection);
    *elapsedMs = clock.nsecsElapsed() / 1'000'000.0;
    return swapped;
}

bool waitForCommittedTextures(
    QQuickWindow *window,
    QObject *renderer,
    int expectedCount,
    int timeoutMs,
    double *elapsedMs)
{
    QElapsedTimer clock;
    clock.start();
    bool frameReady = false;
    for (int frame = 0;
         frame < maximumTextureCommitFrames && clock.elapsed() < timeoutMs;
         ++frame) {
        double ignoredFrameMs = 0.0;
        frameReady = waitForFrame(
            window,
            std::max(1, timeoutMs - static_cast<int>(clock.elapsed())),
            &ignoredFrameMs);
        if (!frameReady)
            break;
        if (renderer
            && renderer->property("committedTextureCount").toInt() == expectedCount
            && renderer->property("uncommittedTextureCount").toInt() == 0) {
            *elapsedMs = clock.nsecsElapsed() / 1'000'000.0;
            return true;
        }
    }
    *elapsedMs = clock.nsecsElapsed() / 1'000'000.0;
    return false;
}

void processFramesFor(QQuickWindow *window, int durationMs)
{
    QEventLoop loop;
    QTimer updater;
    updater.setInterval(16);
    QObject::connect(&updater, &QTimer::timeout, window, [window] {
        window->requestUpdate();
    });
    QTimer::singleShot(durationMs, &loop, &QEventLoop::quit);
    updater.start();
    loop.exec();
}

bool hasProperty(QObject *object, const char *name)
{
    return object
        && (object->metaObject()->indexOfProperty(name) >= 0
            || object->dynamicPropertyNames().contains(name));
}

QObject *timelineProvider(QObject *overlay)
{
    for (const char *name : {"timelineModel", "timeline"}) {
        const QVariant value = overlay->property(name);
        if (QObject *provider = value.value<QObject *>())
            return provider;
    }
    return overlay;
}

int timelineCount(QObject *overlay)
{
    for (const char *name : {"timelineCount", "count"}) {
        if (hasProperty(overlay, name)) {
            bool valid = false;
            const int count = overlay->property(name).toInt(&valid);
            if (valid)
                return count;
        }
    }

    const QVariant timelineValue = overlay->property("timeline");
    const QJSValue timeline = timelineValue.value<QJSValue>();
    if (timeline.isArray())
        return timeline.property(QStringLiteral("length")).toInt();
    if (timelineValue.metaType().id() == QMetaType::QVariantList)
        return timelineValue.toList().size();

    QObject *provider = timelineProvider(overlay);
    if (auto *model = qobject_cast<QAbstractItemModel *>(provider))
        return model->rowCount();
    if (provider != overlay) {
        for (const char *name : {"timelineCount", "count"}) {
            if (hasProperty(provider, name))
                return provider->property(name).toInt();
        }
    }
    return -1;
}

QVariantMap modelEntryAt(QAbstractItemModel *model, int row)
{
    if (!model || row < 0 || row >= model->rowCount())
        return {};
    QVariantMap result;
    const QModelIndex index = model->index(row, 0);
    const QHash<int, QByteArray> roles = model->roleNames();
    for (auto iterator = roles.cbegin(); iterator != roles.cend(); ++iterator)
        result.insert(QString::fromUtf8(iterator.value()), model->data(index, iterator.key()));
    return result;
}

QVariantMap invokeEntryAt(QObject *provider, int index)
{
    if (!provider)
        return {};
    bool hasEntryAt = false;
    const QMetaObject *metaObject = provider->metaObject();
    for (int methodIndex = metaObject->methodOffset();
         methodIndex < metaObject->methodCount(); ++methodIndex) {
        if (metaObject->method(methodIndex).name() == QByteArrayLiteral("entryAt")) {
            hasEntryAt = true;
            break;
        }
    }
    if (!hasEntryAt)
        return {};

    QVariantMap mapResult;
    if (QMetaObject::invokeMethod(
            provider, "entryAt", Qt::DirectConnection,
            Q_RETURN_ARG(QVariantMap, mapResult), Q_ARG(int, index))) {
        return mapResult;
    }
    QVariant variantResult;
    if (QMetaObject::invokeMethod(
            provider, "entryAt", Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, variantResult), Q_ARG(int, index))) {
        return variantResult.toMap();
    }
    if (QMetaObject::invokeMethod(
            provider, "entryAt", Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, variantResult),
            Q_ARG(QVariant, QVariant(index)))) {
        return variantResult.toMap();
    }
    return {};
}

QVariantMap timelineEntryAt(QObject *overlay, int index)
{
    QVariantMap entry = invokeEntryAt(overlay, index);
    if (!entry.isEmpty())
        return entry;

    const QVariant timelineValue = overlay->property("timeline");
    const QJSValue timeline = timelineValue.value<QJSValue>();
    if (timeline.isArray())
        return timeline.property(index).toVariant().toMap();
    if (timelineValue.metaType().id() == QMetaType::QVariantList)
        return timelineValue.toList().value(index).toMap();

    QObject *provider = timelineProvider(overlay);
    entry = invokeEntryAt(provider, index);
    if (!entry.isEmpty())
        return entry;
    return modelEntryAt(qobject_cast<QAbstractItemModel *>(provider), index);
}

bool timelineReady(QObject *overlay, int expectedCount)
{
    QObject *provider = timelineProvider(overlay);
    for (QObject *object : {overlay, provider}) {
        if (!object)
            continue;
        if (hasProperty(object, "preparing")
            && object->property("preparing").toBool()) {
            return false;
        }
        if (hasProperty(object, "ready")
            && !object->property("ready").toBool()) {
            return false;
        }
    }
    return timelineCount(overlay) == expectedCount;
}

bool waitForTimelineReady(
    QObject *overlay,
    int expectedCount,
    int timeoutMs,
    double *elapsedMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!timelineReady(overlay, expectedCount)
           && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    *elapsedMs = clock.nsecsElapsed() / 1'000'000.0;
    return timelineReady(overlay, expectedCount);
}

QString normalizedMode(const QVariant &value)
{
    const QString mode = value.toString().toLower();
    return mode == QStringLiteral("top") || mode == QStringLiteral("bottom")
        ? mode : QStringLiteral("scroll");
}

double estimatedWidth(const QString &text, double fontSize)
{
    double units = 0.0;
    for (const QChar character : text)
        units += character.unicode() > 255 ? 1.0 : 0.56;
    return std::max(fontSize, units * fontSize);
}

QList<OracleEntry> buildOracleTimeline(
    const QVariantList &comments,
    const OracleStyle &style)
{
    QList<OracleEntry> ordered;
    ordered.reserve(comments.size());
    for (int index = 0; index < comments.size(); ++index) {
        const QVariantMap comment = comments.at(index).toMap();
        const QString text = comment.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty())
            continue;
        OracleEntry entry;
        entry.id = comment.contains(QStringLiteral("id"))
            ? comment.value(QStringLiteral("id")).toString()
            : QString::number(index);
        entry.start = std::max(0.0, comment.value(QStringLiteral("time")).toDouble());
        entry.mode = normalizedMode(comment.value(QStringLiteral("mode")));
        entry.text = text;
        entry.inputOrdinal = index;
        ordered.push_back(std::move(entry));
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
        return left.start < right.start;
    });

    constexpr int maximumLanes = 64;
    std::array<double, maximumLanes> scrollAvailable {};
    std::array<double, maximumLanes> topAvailable {};
    std::array<double, maximumLanes> bottomAvailable {};
    QHash<QString, int> densitySecond {
        {QStringLiteral("scroll"), -1},
        {QStringLiteral("top"), -1},
        {QStringLiteral("bottom"), -1},
    };
    QHash<QString, int> densityRank {
        {QStringLiteral("scroll"), 0},
        {QStringLiteral("top"), 0},
        {QStringLiteral("bottom"), 0},
    };
    for (OracleEntry &entry : ordered) {
        const int second = static_cast<int>(std::floor(entry.start));
        if (second != densitySecond.value(entry.mode)) {
            densitySecond[entry.mode] = second;
            densityRank[entry.mode] = 0;
        }
        entry.densityRank = densityRank[entry.mode]++;
        auto &lanes = entry.mode == QStringLiteral("top") ? topAvailable
            : entry.mode == QStringLiteral("bottom") ? bottomAvailable
                                                       : scrollAvailable;
        int lane = -1;
        int earliestLane = 0;
        for (int candidate = 0; candidate < maximumLanes; ++candidate) {
            if (lanes[candidate] < lanes[earliestLane])
                earliestLane = candidate;
            if (lane < 0 && lanes[candidate] <= entry.start)
                lane = candidate;
        }
        if (lane < 0)
            lane = earliestLane;
        entry.lane = lane;
        if (entry.mode == QStringLiteral("scroll")) {
            const double speed = std::max(1.0, style.width)
                / std::max(3.0, style.laneLayoutScrollDuration);
            lanes[lane] = entry.start
                + estimatedWidth(entry.text, style.laneLayoutFontSize) / speed + 0.12;
        } else {
            lanes[lane] = entry.start + 4.5;
        }
    }
    return ordered;
}

bool oracleModeEnabled(const OracleEntry &entry, const OracleStyle &style)
{
    if (entry.mode == QStringLiteral("top"))
        return style.showTop;
    if (entry.mode == QStringLiteral("bottom"))
        return style.showBottom;
    return style.showScroll;
}

bool oracleBlocked(const QString &text, const OracleStyle &style)
{
    return std::any_of(
        style.blockedTerms.cbegin(), style.blockedTerms.cend(),
        [&text](const QString &term) { return text.contains(term); });
}

QStringList eligibleIdsAt(
    const QList<OracleEntry> &timeline,
    double position,
    const OracleStyle &style)
{
    const double effectiveTopMargin = std::max(
        0.0, std::min(style.topMargin,
                      style.height * style.displayArea - style.fontSize - 8.0));
    const int laneCount = std::max(
        1, static_cast<int>(std::floor(
               (style.height * style.displayArea - effectiveTopMargin)
               / std::max(1.0, style.fontSize + 8.0))));
    QStringList ids;
    for (const OracleEntry &entry : timeline) {
        const double effectiveStart = entry.start + style.timeOffset;
        const double duration = entry.mode == QStringLiteral("scroll")
            ? std::max(3.0, style.scrollDuration) : 4.5;
        if (effectiveStart > position)
            break;
        if (oracleModeEnabled(entry, style)
            && effectiveStart + duration >= position
            && entry.lane < laneCount
            && entry.densityRank < style.density
            && !oracleBlocked(entry.text, style)) {
            ids.push_back(entry.id);
        }
    }
    ids.sort();
    return ids;
}

TimelineInspection inspectTimeline(
    QObject *overlay,
    const Fixture &fixture,
    int expectedCount = hostedCommentCount)
{
    TimelineInspection result;
    result.count = timelineCount(overlay);
    if (result.count != expectedCount) {
        result.error = QStringLiteral("timeline count %1 does not match fixture count %2")
                           .arg(result.count)
                           .arg(expectedCount);
        return result;
    }

    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    QSet<QString> ids;
    double previousStart = -1.0;
    for (int index = 0; index < result.count; ++index) {
        const QVariantMap entry = timelineEntryAt(overlay, index);
        const QString id = entry.value(QStringLiteral("commentId"),
                                       entry.value(QStringLiteral("id"))).toString();
        const double start = entry.value(QStringLiteral("start"),
                                         entry.value(QStringLiteral("time"))).toDouble();
        const QString mode = entry.value(QStringLiteral("mode")).toString();
        const QString text = entry.value(QStringLiteral("commentText"),
                                         entry.value(QStringLiteral("text"))).toString();
        const int lane = entry.value(QStringLiteral("lane")).toInt();
        const int densityRank = entry.value(QStringLiteral("densityRank")).toInt();
        if (id.isEmpty() || ids.contains(id) || start < previousStart
            || !fixture.byId.contains(id)
            || mode != fixture.byId.value(id).value(QStringLiteral("mode")).toString()
            || text != fixture.byId.value(id).value(QStringLiteral("text")).toString()
            || lane < 0 || densityRank < 0) {
            result.error = QStringLiteral("timeline semantics differ at index %1/id '%2'")
                               .arg(index)
                               .arg(id);
            return result;
        }
        ids.insert(id);
        previousStart = start;
        const QByteArray row = QStringLiteral("%1|%2|%3|%4|%5\n")
                                   .arg(id)
                                   .arg(start, 0, 'f', 4)
                                   .arg(mode)
                                   .arg(lane)
                                   .arg(densityRank)
                                   .toUtf8();
        fingerprint.addData(row);
    }
    result.fingerprint = QString::fromLatin1(fingerprint.result().toHex());
    result.valid = true;
    return result;
}

DelegateSnapshot snapshotDelegates(
    QQuickWindow *window,
    double position,
    const QString &blockedTerm = {},
    bool topAllowed = true)
{
    DelegateSnapshot snapshot;
    QSet<QString> visibleIds;
    std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        const QVariant commentId = item->property("commentId");
        if (commentId.isValid()) {
            ++snapshot.delegateCount;
            if (item->isVisible()) {
                const QString id = commentId.toString();
                const QString mode = item->property("mode").toString();
                const QString text = item->property("commentText").toString();
                const double start = item->property("start").toDouble();
                const double duration = mode == QStringLiteral("scroll") ? 9.0 : 4.5;
                if (visibleIds.contains(id))
                    snapshot.unique = false;
                visibleIds.insert(id);
                if (start > position + 0.001 || start + duration < position - 0.121)
                    snapshot.insideTimeWindow = false;
                if ((!blockedTerm.isEmpty() && text.contains(blockedTerm))
                    || (!topAllowed && mode == QStringLiteral("top"))) {
                    snapshot.filterValid = false;
                }
            }
        }
        const QList<QQuickItem *> children = item->childItems();
        for (QQuickItem *child : children)
            visit(child);
    };
    visit(window->contentItem());
    snapshot.visibleIds = visibleIds.values();
    snapshot.visibleIds.sort();
    return snapshot;
}

QStringList invokeRendererIdList(
    QObject *renderer,
    const char *methodName,
    bool *available,
    bool *invoked)
{
    *available = false;
    *invoked = false;
    if (!renderer)
        return {};
    const QMetaObject *metaObject = renderer->metaObject();
    for (int methodIndex = 0; methodIndex < metaObject->methodCount(); ++methodIndex) {
        const QMetaMethod method = metaObject->method(methodIndex);
        if (method.name() != QByteArrayView(methodName)
            || method.parameterCount() != 0) {
            continue;
        }
        *available = true;
        QStringList stringResult;
        if (QMetaObject::invokeMethod(
                renderer, methodName, Qt::DirectConnection,
                Q_RETURN_ARG(QStringList, stringResult))) {
            *invoked = true;
            return stringResult;
        }
        QVariant variantResult;
        if (QMetaObject::invokeMethod(
                renderer, methodName, Qt::DirectConnection,
                Q_RETURN_ARG(QVariant, variantResult))) {
            *invoked = true;
            if (variantResult.metaType().id() == QMetaType::QStringList)
                return variantResult.toStringList();
            QStringList result;
            for (const QVariant &entry : variantResult.toList())
                result.push_back(entry.toString());
            return result;
        }
        return {};
    }
    return {};
}

QStringList invokeEligibleCandidateIds(QObject *renderer, bool *valid)
{
    bool available = false;
    bool invoked = false;
    const QStringList result = invokeRendererIdList(
        renderer, "eligibleCandidateIds", &available, &invoked);
    *valid = available && invoked;
    return result;
}

DelegateSnapshot snapshotPresentedCandidates(
    QQuickWindow *window,
    QObject *overlay,
    double position,
    bool *committedHookValid = nullptr)
{
    QObject *renderer = overlay->property("rendererItem").value<QObject *>();
    bool rendererHookAvailable = false;
    bool rendererHookInvoked = false;
    QStringList rendererIds = invokeRendererIdList(
        renderer,
        "committedVisibleCandidateIds",
        &rendererHookAvailable,
        &rendererHookInvoked);
    const bool hookValid = rendererHookAvailable && rendererHookInvoked;
    if (committedHookValid)
        *committedHookValid = hookValid;
    if (hookValid) {
        DelegateSnapshot snapshot;
        snapshot.delegateCount = rendererIds.size();
        QSet<QString> uniqueIds(rendererIds.cbegin(), rendererIds.cend());
        snapshot.unique = uniqueIds.size() == rendererIds.size();
        snapshot.visibleIds = uniqueIds.values();
        snapshot.visibleIds.sort();
        return snapshot;
    }
    return snapshotDelegates(window, position);
}

QStringList setDifference(const QStringList &left, const QStringList &right)
{
    const QSet<QString> rightSet(right.cbegin(), right.cend());
    QStringList result;
    for (const QString &value : left) {
        if (!rightSet.contains(value))
            result.push_back(value);
    }
    result.sort();
    return result;
}

ExactSetInspection inspectExactSetAt(
    QQuickWindow *window,
    QObject *overlay,
    const QList<OracleEntry> &oracleTimeline,
    const OracleStyle &style,
    double position,
    int frameCount = 1)
{
    ExactSetInspection result;
    result.expectedIds = eligibleIdsAt(oracleTimeline, position, style);
    result.exact = true;
    for (int frame = 0; frame < frameCount; ++frame) {
        double ignoredFrameMs = 0.0;
        const bool frameReady = waitForFrame(window, 2'000, &ignoredFrameMs);
        result.frameReady = result.frameReady || frameReady;
        if (!frameReady) {
            result.exact = false;
            continue;
        }
        const DelegateSnapshot snapshot = snapshotPresentedCandidates(
            window, overlay, position);
        result.actualIds = snapshot.visibleIds;
        result.unique = result.unique && snapshot.unique;
        const QStringList missing = setDifference(result.expectedIds, result.actualIds);
        const QStringList unexpected = setDifference(result.actualIds, result.expectedIds);
        result.missingIds.append(missing);
        result.unexpectedIds.append(unexpected);
        result.noEarlyOrStale = result.noEarlyOrStale && unexpected.isEmpty();
        result.exact = result.exact
            && snapshot.unique && missing.isEmpty() && unexpected.isEmpty();
    }
    result.missingIds.removeDuplicates();
    result.unexpectedIds.removeDuplicates();
    result.missingIds.sort();
    result.unexpectedIds.sort();
    return result;
}

QVariantList generationFixture(
    const QVariantList &source,
    const QString &prefix,
    int count = 2'000)
{
    QVariantList result;
    result.reserve(std::min<qsizetype>(count, source.size()));
    for (int index = 0; index < count && index < source.size(); ++index) {
        QVariantMap comment = source.at(index).toMap();
        comment.insert(
            QStringLiteral("id"),
            prefix + comment.value(QStringLiteral("id")).toString());
        result.push_back(std::move(comment));
    }
    return result;
}

QVariantList longTextureFixture(int *textLength)
{
    constexpr int commentCount = 150;
    const QString repeatedText = QStringLiteral("长文本纹理预算安全验证").repeated(10);
    QVariantList result;
    result.reserve(commentCount);
    for (int index = 0; index < commentCount; ++index) {
        const QString text = repeatedText
            + QStringLiteral("%1").arg(index, 3, 10, QLatin1Char('0'));
        if (index == 0)
            *textLength = text.size();
        result.push_back(QVariantMap {
            {QStringLiteral("id"), QStringLiteral("texture-budget-%1").arg(index)},
            {QStringLiteral("time"), 605.0},
            {QStringLiteral("mode"), index % 3 == 0 ? QStringLiteral("scroll")
                 : index % 3 == 1 ? QStringLiteral("top")
                                  : QStringLiteral("bottom")},
            {QStringLiteral("color"), 0xffffff},
            {QStringLiteral("text"), text},
        });
    }
    return result;
}

QVariantList longTextureChurnFixture(
    QStringList *initialEligibleIds,
    QStringList *shiftedEligibleIds)
{
    constexpr int retainedCount = 125;
    constexpr int churnCount = 25;
    constexpr int totalCount = retainedCount + churnCount * 2;
    const QString repeatedText = QStringLiteral("长文本纹理预算安全验证").repeated(10);
    QVariantList result;
    result.reserve(totalCount);
    initialEligibleIds->reserve(retainedCount + churnCount);
    shiftedEligibleIds->reserve(retainedCount + churnCount);
    for (int index = 0; index < totalCount; ++index) {
        const QString id = QStringLiteral("texture-budget-%1").arg(index);
        const QString mode = index % 3 == 0 ? QStringLiteral("scroll")
            : index % 3 == 1 ? QStringLiteral("top")
                             : QStringLiteral("bottom");
        double start = 605.0;
        if (index < churnCount)
            start = mode == QStringLiteral("scroll") ? 596.0 : 600.5;
        else if (index >= retainedCount + churnCount)
            start = 605.5;
        if (index < retainedCount + churnCount)
            initialEligibleIds->push_back(id);
        if (index >= churnCount)
            shiftedEligibleIds->push_back(id);
        result.push_back(QVariantMap {
            {QStringLiteral("id"), id},
            {QStringLiteral("time"), start},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("color"), 0xffffff},
            {QStringLiteral("text"), repeatedText
                 + QStringLiteral("%1").arg(index, 3, 10, QLatin1Char('0'))},
        });
    }
    initialEligibleIds->sort();
    shiftedEligibleIds->sort();
    return result;
}

QJsonArray limitedIds(const QStringList &ids, int limit = 12)
{
    QJsonArray result;
    for (int index = 0; index < ids.size() && index < limit; ++index)
        result.push_back(ids.at(index));
    return result;
}

bool timelineIdsUsePrefix(QObject *overlay, int count, const QString &prefix)
{
    if (timelineCount(overlay) != count)
        return false;
    for (int index = 0; index < count; ++index) {
        const QVariantMap entry = timelineEntryAt(overlay, index);
        const QString id = entry.value(QStringLiteral("commentId"),
                                       entry.value(QStringLiteral("id"))).toString();
        if (!id.startsWith(prefix))
            return false;
    }
    return true;
}

QJsonObject metric(
    const QString &id,
    const QString &unit,
    const QList<double> &samples,
    const QJsonObject &attributes)
{
    QJsonArray encodedSamples;
    for (const double sample : samples)
        encodedSamples.push_back(sample);
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("unit"), unit},
        {QStringLiteral("samples"), encodedSamples},
        {QStringLiteral("attributes"), attributes},
    };
}

QJsonObject invariant(
    const QString &id,
    bool passed,
    const QJsonObject &details = {})
{
    QJsonObject evidenceDetails = details;
    evidenceDetails.insert(
        QStringLiteral("evidence"), QStringLiteral("qt-offscreen-software"));
    evidenceDetails.insert(
        QStringLiteral("strictAbsoluteEvidence"), false);
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("passed"), passed},
        {QStringLiteral("details"), evidenceDetails},
    };
}

bool writeManifest(const QString &path, const QJsonObject &manifest, QString *error)
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
    const QByteArray encoded = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("yanami-danmaku-perf-probe"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Collect hosted-only Yanami danmaku rendering smoke evidence."));
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(
        {QStringLiteral("p"), QStringLiteral("profile")},
        QStringLiteral("Only pull-request/pr is accepted by this hosted probe."),
        QStringLiteral("profile"),
        QStringLiteral("pr")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("m"), QStringLiteral("mode")},
        QStringLiteral("Gate mode: collect, debt, or enforce."),
        QStringLiteral("mode"),
        QStringLiteral("collect")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("o"), QStringLiteral("output")},
        QStringLiteral("Path for the run manifest."),
        QStringLiteral("path"),
        QStringLiteral("danmaku-run-manifest.json")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("fixture-dir")},
        QStringLiteral("Validated DanmakuDensity-v1 directory."),
        QStringLiteral("directory")));
    parser.process(app);

    const QString profile = canonicalProfile(
        parser.value(QStringLiteral("profile")).trimmed().toLower());
    const QString mode = parser.value(QStringLiteral("mode")).trimmed().toLower();
    const QSet<QString> allowedModes {
        QStringLiteral("collect"), QStringLiteral("debt"), QStringLiteral("enforce"),
    };
    if (profile != QStringLiteral("PullRequest")
        || !allowedModes.contains(mode)) {
        qCritical("The danmaku component probe is hosted PullRequest evidence only.");
        return 2;
    }

    QString fixtureDirectory = parser.value(QStringLiteral("fixture-dir")).trimmed();
    if (fixtureDirectory.isEmpty()) {
        fixtureDirectory = QString::fromLocal8Bit(
            qgetenv("YANAMI_PERF_DANMAKU_FIXTURE_DIR")).trimmed();
    }
    const QString expectedFixtureSha = QString::fromLocal8Bit(
        qgetenv("YANAMI_PERF_DANMAKU_SHA256")).trimmed().toLower();
    Fixture fixture;
    QString fixtureError;
    if (!loadFixture(
            fixtureDirectory,
            expectedFixtureSha,
            &fixture,
            &fixtureError)) {
        qCritical().noquote() << "Invalid DanmakuDensity-v1 fixture:" << fixtureError;
        return 2;
    }
    const QString startedAtUtc = QDateTime::currentDateTimeUtc().toString(
        Qt::ISODateWithMs);

    QStringList qmlWarnings;
    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlEngine::warnings,
        &app,
        [&qmlWarnings](const QList<QQmlError> &warnings) {
            for (const QQmlError &warning : warnings)
                qmlWarnings.push_back(warning.toString());
        });
    engine.loadFromModule("Yanami.DanmakuPerfProbe", "DanmakuPerfHarness");
    if (engine.rootObjects().size() != 1) {
        qCritical("Unable to create the DanmakuPerfHarness QML root.");
        return 2;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QObject *overlay = window
        ? window->findChild<QObject *>(QStringLiteral("danmakuPerfOverlay"))
        : nullptr;
    if (!window || !overlay) {
        qCritical("DanmakuPerfHarness did not expose its window and production overlay.");
        return 2;
    }
    // Keep the hosted workload inside the production menu's supported range.
    overlay->setProperty("density", 30);

    double initialFrameMs = 0.0;
    const bool initialFrameReady = waitForFrame(window, 5'000, &initialFrameMs);
    const auto graphicsApi = window->rendererInterface()->graphicsApi();
    const bool rendererValid = QGuiApplication::platformName() == QStringLiteral("offscreen")
        && graphicsApi == QSGRendererInterface::Software;

    QList<double> prepareSamples;
    QList<double> seekFrameSamples;
    bool timelineValid = true;
    bool latestSeekValid = true;
    QString canonicalTimelineFingerprint;
    QString canonicalVisibleFingerprint;
    int canonicalDelegateCount = -1;

    overlay->setProperty("paused", true);
    for (int generation = 0; generation < loadGenerationCount; ++generation) {
        overlay->setProperty("comments", QVariantList{});
        overlay->setProperty("mediaPosition", 0.0);
        QCoreApplication::processEvents();

        QElapsedTimer prepareClock;
        prepareClock.start();
        overlay->setProperty("comments", fixture.hostedComments);
        double readinessWaitMs = 0.0;
        const bool ready = waitForTimelineReady(
            overlay, hostedCommentCount, 10'000, &readinessWaitMs);
        prepareSamples.push_back(prepareClock.nsecsElapsed() / 1'000'000.0);
        const TimelineInspection timeline = ready
            ? inspectTimeline(overlay, fixture) : TimelineInspection {};
        timelineValid = timelineValid && ready && timeline.valid;
        if (canonicalTimelineFingerprint.isEmpty())
            canonicalTimelineFingerprint = timeline.fingerprint;
        else
            timelineValid = timelineValid
                && timeline.fingerprint == canonicalTimelineFingerprint;

        overlay->setProperty("mediaPosition", 1'200.0);
        overlay->setProperty("mediaPosition", 900.5);
        QElapsedTimer seekClock;
        seekClock.start();
        overlay->setProperty("mediaPosition", 605.5);
        double ignoredFrameMs = 0.0;
        const bool frameReady = waitForFrame(window, 2'000, &ignoredFrameMs);
        seekFrameSamples.push_back(seekClock.nsecsElapsed() / 1'000'000.0);
        const DelegateSnapshot first = snapshotPresentedCandidates(window, overlay, 605.5);
        const QString firstFingerprint = first.visibleIds.join(QLatin1Char('|'));
        latestSeekValid = latestSeekValid && frameReady && first.unique
            && first.insideTimeWindow && first.filterValid
            && !first.visibleIds.isEmpty();

        overlay->setProperty("mediaPosition", 900.5);
        overlay->setProperty("mediaPosition", 605.5);
        waitForFrame(window, 2'000, &ignoredFrameMs);
        const DelegateSnapshot repeated = snapshotPresentedCandidates(window, overlay, 605.5);
        latestSeekValid = latestSeekValid
            && repeated.visibleIds == first.visibleIds
            && repeated.delegateCount == first.delegateCount;
        if (canonicalVisibleFingerprint.isEmpty()) {
            canonicalVisibleFingerprint = firstFingerprint;
            canonicalDelegateCount = first.delegateCount;
        } else {
            latestSeekValid = latestSeekValid
                && firstFingerprint == canonicalVisibleFingerprint
                && first.delegateCount == canonicalDelegateCount;
        }
    }

    OracleStyle oracleStyle;
    oracleStyle.width = window->width();
    oracleStyle.height = window->height();
    const QList<OracleEntry> strictOracle = buildOracleTimeline(
        fixture.strictComments, oracleStyle);

    overlay->setProperty("comments", QVariantList {});
    overlay->setProperty("paused", true);
    overlay->setProperty("buffering", false);
    overlay->setProperty("playbackRate", 1.0);
    overlay->setProperty("mediaPosition", 605.5);
    QCoreApplication::processEvents();
    QElapsedTimer strictLoadClock;
    strictLoadClock.start();
    overlay->setProperty("comments", fixture.strictComments);
    double strictReadyWaitMs = 0.0;
    const bool strictReady = waitForTimelineReady(
        overlay, strictCommentCount, 15'000, &strictReadyWaitMs);
    double strictFirstFrameWaitMs = 0.0;
    const bool strictFirstFrameReady = strictReady
        && waitForFrame(window, 5'000, &strictFirstFrameWaitMs);
    const double strictLoadToFirstEligibleFrameMs =
        strictLoadClock.nsecsElapsed() / 1'000'000.0;
    bool committedPresentationHookValid = false;
    const DelegateSnapshot strictFirstFrame = strictFirstFrameReady
        ? snapshotPresentedCandidates(
              window, overlay, 605.5, &committedPresentationHookValid)
        : DelegateSnapshot {};
    const QStringList strictExpected605 = eligibleIdsAt(
        strictOracle, 605.5, oracleStyle);
    const bool firstEligibleFrameValid = strictFirstFrameReady
        && !strictFirstFrame.visibleIds.isEmpty()
        && setDifference(strictFirstFrame.visibleIds, strictExpected605).isEmpty();
    const QImage strictFirstEligibleFrame = strictFirstFrameReady
        ? window->grabWindow().convertToFormat(QImage::Format_ARGB32)
        : QImage {};
    qsizetype nonBackgroundPixelCount = 0;
    constexpr QRgb hostedBackground = qRgb(5, 5, 5);
    for (int y = 0; y < strictFirstEligibleFrame.height(); ++y) {
        const auto *pixels = reinterpret_cast<const QRgb *>(
            strictFirstEligibleFrame.constScanLine(y));
        for (int x = 0; x < strictFirstEligibleFrame.width(); ++x) {
            if ((pixels[x] & 0x00ffffffU)
                != (hostedBackground & 0x00ffffffU)) {
                ++nonBackgroundPixelCount;
            }
        }
    }
    const QString screenshotPath = QString::fromLocal8Bit(
        qgetenv("YANAMI_PERF_DANMAKU_SCREENSHOT")).trimmed();
    bool screenshotSaved = false;
    if (!screenshotPath.isEmpty() && !strictFirstEligibleFrame.isNull()) {
        const QFileInfo screenshot(screenshotPath);
        screenshotSaved = QDir().mkpath(screenshot.absolutePath())
            && strictFirstEligibleFrame.save(screenshot.absoluteFilePath(), "PNG");
        if (!screenshotSaved) {
            qWarning().noquote()
                << "Unable to save hosted danmaku screenshot:" << screenshotPath;
        }
    }

    const TimelineInspection strictTimeline = strictReady
        ? inspectTimeline(overlay, fixture, strictCommentCount)
        : TimelineInspection {};
    timelineValid = timelineValid && strictTimeline.valid;
    overlay->setProperty("mediaPosition", 900.5);
    overlay->setProperty("mediaPosition", 605.5);
    const ExactSetInspection exact605 = inspectExactSetAt(
        window, overlay, strictOracle, oracleStyle, 605.5);
    overlay->setProperty("mediaPosition", 900.5);
    const ExactSetInspection exact900 = inspectExactSetAt(
        window, overlay, strictOracle, oracleStyle, 900.5);
    overlay->setProperty("mediaPosition", 1'200.0);
    overlay->setProperty("mediaPosition", 900.5);
    overlay->setProperty("mediaPosition", 605.5);
    const ExactSetInspection reverseSeekExact = inspectExactSetAt(
        window, overlay, strictOracle, oracleStyle, 605.5);
    const bool exactSetValid = exact605.exact && exact900.exact
        && reverseSeekExact.exact;
    const bool noDuplicateEarlyOrStale = exact605.unique && exact900.unique
        && reverseSeekExact.unique && exact605.noEarlyOrStale
        && exact900.noEarlyOrStale && reverseSeekExact.noEarlyOrStale;

    overlay->setProperty("mediaPosition", 605.5);
    overlay->setProperty("paused", true);
    const double pausedStart = overlay->property("renderTime").toDouble();
    processFramesFor(window, 180);
    const double pausedEnd = overlay->property("renderTime").toDouble();
    overlay->setProperty("paused", false);
    overlay->setProperty("buffering", true);
    const double bufferingStart = overlay->property("renderTime").toDouble();
    processFramesFor(window, 180);
    const double bufferingEnd = overlay->property("renderTime").toDouble();
    overlay->setProperty("buffering", false);

    QJsonArray rateChecks;
    bool allRatesValid = true;
    for (const double rate : {0.5, 1.0, 1.5, 2.0}) {
        overlay->setProperty("paused", true);
        overlay->setProperty("mediaPosition", 605.5);
        overlay->setProperty("playbackRate", rate);
        const double rateStart = overlay->property("renderTime").toDouble();
        QElapsedTimer rateClock;
        rateClock.start();
        overlay->setProperty("paused", false);
        processFramesFor(window, 220);
        const double elapsedSeconds = rateClock.nsecsElapsed() / 1'000'000'000.0;
        const double rateAdvance = overlay->property("renderTime").toDouble() - rateStart;
        const double expectedAdvance = elapsedSeconds * rate;
        const double tolerance = std::max(0.08, expectedAdvance * 0.45);
        const bool valid = std::abs(rateAdvance - expectedAdvance) <= tolerance;
        allRatesValid = allRatesValid && valid;
        rateChecks.push_back(QJsonObject {
            {QStringLiteral("rate"), rate},
            {QStringLiteral("elapsedSeconds"), elapsedSeconds},
            {QStringLiteral("advanceSeconds"), rateAdvance},
            {QStringLiteral("expectedAdvanceSeconds"), expectedAdvance},
            {QStringLiteral("toleranceSeconds"), tolerance},
            {QStringLiteral("passed"), valid},
        });
    }
    const bool pauseBufferRateValid = std::abs(pausedEnd - pausedStart) <= 0.02
        && std::abs(bufferingEnd - bufferingStart) <= 0.02
        && allRatesValid;
    overlay->setProperty("paused", true);
    overlay->setProperty("playbackRate", 1.0);
    overlay->setProperty("mediaPosition", 605.5);

    const TimelineInspection timelineBeforeStyle = inspectTimeline(
        overlay, fixture, strictCommentCount);
    overlay->setProperty("fontSize", 30.0);
    overlay->setProperty("commentOpacity", 0.72);
    overlay->setProperty("blockedTerms", QStringLiteral("屏蔽"));
    overlay->setProperty("showTop", false);
    overlay->setProperty("density", 14);
    overlay->setProperty("danmakuEnabled", false);
    processFramesFor(window, 50);
    const bool disabled = !overlay->property("visible").toBool();
    overlay->setProperty("danmakuEnabled", true);
    double styleFrameMs = 0.0;
    const bool styleFrameReady = waitForFrame(window, 2'000, &styleFrameMs);
    OracleStyle filteredStyle = oracleStyle;
    filteredStyle.fontSize = 30.0;
    filteredStyle.blockedTerms = {QStringLiteral("屏蔽")};
    filteredStyle.showTop = false;
    filteredStyle.density = 14;
    overlay->setProperty("mediaPosition", 900.5);
    overlay->setProperty("mediaPosition", 605.5);
    const ExactSetInspection filteredExact = inspectExactSetAt(
        window, overlay, strictOracle, filteredStyle, 605.5);
    const TimelineInspection timelineAfterStyle = inspectTimeline(
        overlay, fixture, strictCommentCount);
    const bool styleFilterValid = disabled && styleFrameReady
        && filteredExact.exact && timelineBeforeStyle.valid && timelineAfterStyle.valid
        && timelineBeforeStyle.fingerprint == timelineAfterStyle.fingerprint;
    overlay->setProperty("blockedTerms", QString {});
    overlay->setProperty("showTop", true);
    overlay->setProperty("fontSize", 36.0);
    overlay->setProperty("commentOpacity", 0.9);
    overlay->setProperty("density", 30);

    for (int cycle = 0; cycle < 100; ++cycle) {
        overlay->setProperty("mediaPosition", cycle % 2 == 0 ? 900.5 : 1'200.0);
        overlay->setProperty("danmakuEnabled", cycle % 3 != 0);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    overlay->setProperty("danmakuEnabled", true);
    overlay->setProperty("mediaPosition", 605.5);
    double boundedFrameMs = 0.0;
    const bool boundedFrameReady = waitForFrame(window, 2'000, &boundedFrameMs);
    const DelegateSnapshot bounded = snapshotPresentedCandidates(window, overlay, 605.5);
    const bool boundedGrowth = boundedFrameReady
        && bounded.delegateCount == strictFirstFrame.delegateCount
        && bounded.visibleIds == strictFirstFrame.visibleIds;

    overlay->setProperty("paused", false);
    QList<double> frameIntervals;
    QElapsedTimer frameClock;
    qint64 previousFrameNs = 0;
    QEventLoop frameLoop;
    QTimer updater;
    updater.setInterval(16);
    QObject::connect(&updater, &QTimer::timeout, window, [window] {
        window->requestUpdate();
    });
    const QMetaObject::Connection intervalConnection = QObject::connect(
        window,
        &QQuickWindow::frameSwapped,
        &frameLoop,
        [&] {
            const qint64 now = frameClock.nsecsElapsed();
            if (previousFrameNs != 0)
                frameIntervals.push_back((now - previousFrameNs) / 1'000'000.0);
            previousFrameNs = now;
            if (frameIntervals.size() >= 120)
                frameLoop.quit();
        },
        Qt::QueuedConnection);
    QTimer frameWatchdog;
    frameWatchdog.setSingleShot(true);
    QObject::connect(&frameWatchdog, &QTimer::timeout, &frameLoop, &QEventLoop::quit);
    frameClock.start();
    updater.start();
    frameWatchdog.start(5'000);
    window->requestUpdate();
    frameLoop.exec();
    QObject::disconnect(intervalConnection);
    updater.stop();

    const QVariantList generationA = generationFixture(
        fixture.strictComments, QStringLiteral("episode-a-"));
    const QVariantList generationB = generationFixture(
        fixture.strictComments, QStringLiteral("episode-b-"));
    overlay->setProperty("paused", true);
    overlay->setProperty("mediaPosition", 605.5);
    overlay->setProperty("comments", generationA);
    overlay->setProperty("comments", generationB);
    double generationReadyMs = 0.0;
    const bool generationReady = waitForTimelineReady(
        overlay, generationB.size(), 10'000, &generationReadyMs);
    double generationFrameMs = 0.0;
    const bool generationFrameReady = generationReady
        && waitForFrame(window, 2'000, &generationFrameMs);
    const DelegateSnapshot generationSnapshot = generationFrameReady
        ? snapshotPresentedCandidates(window, overlay, 605.5)
        : DelegateSnapshot {};
    const bool generationTimelineCurrent = generationReady
        && timelineIdsUsePrefix(
            overlay, generationB.size(), QStringLiteral("episode-b-"));
    const bool generationPixelsCurrent = generationFrameReady
        && !generationSnapshot.visibleIds.isEmpty()
        && std::all_of(
            generationSnapshot.visibleIds.cbegin(), generationSnapshot.visibleIds.cend(),
            [](const QString &id) { return id.startsWith(QStringLiteral("episode-b-")); });
    const bool generationValid = generationTimelineCurrent && generationPixelsCurrent;
    latestSeekValid = latestSeekValid && reverseSeekExact.frameReady && generationValid;

    int longTextureTextLength = 0;
    const QVariantList longTextureComments = longTextureFixture(&longTextureTextLength);
    overlay->setProperty("comments", QVariantList {});
    double clearedBeforeTextureMs = 0.0;
    waitForTimelineReady(overlay, 0, 5'000, &clearedBeforeTextureMs);
    window->setWidth(1920);
    window->setHeight(3000);
    overlay->setProperty("fontSize", 42.0);
    overlay->setProperty("commentOpacity", 0.9);
    overlay->setProperty("scrollDuration", 9.0);
    overlay->setProperty("displayArea", 1.0);
    overlay->setProperty("density", 50);
    overlay->setProperty("timeOffset", 0.0);
    overlay->setProperty("blockedTerms", QString {});
    overlay->setProperty("showScroll", true);
    overlay->setProperty("showTop", true);
    overlay->setProperty("showBottom", true);
    overlay->setProperty("topMargin", 0.0);
    overlay->setProperty("danmakuEnabled", true);
    overlay->setProperty("paused", true);
    overlay->setProperty("buffering", false);
    overlay->setProperty("playbackRate", 1.0);
    overlay->setProperty("mediaPosition", 605.5);
    overlay->setProperty("comments", longTextureComments);
    double longTextureReadyMs = 0.0;
    const bool longTextureReady = waitForTimelineReady(
        overlay, longTextureComments.size(), 10'000, &longTextureReadyMs);
    if (longTextureReady) {
        // Re-anchor after the asynchronous timeline swap so the deliberately
        // expensive software raster pass cannot leave the active window on a
        // pre-swap frame.
        overlay->setProperty("mediaPosition", 605.0);
        overlay->setProperty("mediaPosition", 605.5);
    }
    QObject *const textureRenderer = overlay->property("rendererItem").value<QObject *>();
    double longTextureFrameMs = 0.0;
    const bool longTextureFrameReady = longTextureReady
        && waitForCommittedTextures(
            window, textureRenderer, longTextureComments.size(),
            10'000, &longTextureFrameMs);
    bool longCommittedHookValid = false;
    const DelegateSnapshot longTextureSnapshot = longTextureFrameReady
        ? snapshotPresentedCandidates(
              window, overlay, 605.5, &longCommittedHookValid)
        : DelegateSnapshot {};
    bool longEligibleHookValid = false;
    const QStringList longEligibleIds = invokeEligibleCandidateIds(
        textureRenderer, &longEligibleHookValid);
    const qint64 estimatedCachedRasterBytes = textureRenderer
        ? textureRenderer->property("estimatedCachedRasterBytes").toLongLong() : 0;
    const qint64 texturePayloadBudgetBytes = textureRenderer
        ? textureRenderer->property("texturePayloadBudgetBytes").toLongLong() : 0;
    const int committedTextureCount = textureRenderer
        ? textureRenderer->property("committedTextureCount").toInt() : 0;
    const int budgetScaledTextureCount = textureRenderer
        ? textureRenderer->property("budgetScaledTextureCount").toInt() : 0;
    const int uncommittedTextureCount = textureRenderer
        ? textureRenderer->property("uncommittedTextureCount").toInt() : 0;
    const int emptyTextureCount = textureRenderer
        ? textureRenderer->property("emptyTextureCount").toInt() : -1;
    const int budgetDeferredTextureCount = textureRenderer
        ? textureRenderer->property("budgetDeferredTextureCount").toInt() : -1;
    const int transientFailureTextureCount = textureRenderer
        ? textureRenderer->property("transientFailureTextureCount").toInt() : -1;
    const int permanentRejectedTextureCount = textureRenderer
        ? textureRenderer->property("permanentRejectedTextureCount").toInt() : -1;
    const bool activeRasterStatusAccountingValid = committedTextureCount
            + emptyTextureCount + budgetDeferredTextureCount
            + transientFailureTextureCount + permanentRejectedTextureCount
        == longTextureComments.size();
    const bool activeTextureBudgetBounded = longTextureFrameReady
        && longCommittedHookValid && longEligibleHookValid
        && longEligibleIds.size() == longTextureComments.size()
        && longTextureSnapshot.visibleIds.size() == longTextureComments.size()
        && longTextureSnapshot.visibleIds == longEligibleIds
        && committedTextureCount == longTextureComments.size()
        && uncommittedTextureCount == 0
        && activeRasterStatusAccountingValid
        && budgetScaledTextureCount > 0
        && estimatedCachedRasterBytes > 0
        && texturePayloadBudgetBytes > 0
        && estimatedCachedRasterBytes <= texturePayloadBudgetBytes;

    QStringList churnInitialExpectedIds;
    QStringList churnShiftedExpectedIds;
    const QVariantList longTextureChurnComments = longTextureChurnFixture(
        &churnInitialExpectedIds, &churnShiftedExpectedIds);
    overlay->setProperty("mediaPosition", 605.0);
    overlay->setProperty("comments", longTextureChurnComments);
    double churnReadyMs = 0.0;
    const bool churnReady = waitForTimelineReady(
        overlay, longTextureChurnComments.size(), 10'000, &churnReadyMs);
    if (churnReady) {
        overlay->setProperty("mediaPosition", 604.9);
        overlay->setProperty("mediaPosition", 605.0);
    }
    double churnInitialFrameMs = 0.0;
    const bool churnInitialFrameReady = churnReady
        && waitForCommittedTextures(
            window, textureRenderer, churnInitialExpectedIds.size(),
            10'000, &churnInitialFrameMs);
    bool churnInitialCommittedHookValid = false;
    const DelegateSnapshot churnInitialSnapshot = churnInitialFrameReady
        ? snapshotPresentedCandidates(
              window, overlay, 605.0, &churnInitialCommittedHookValid)
        : DelegateSnapshot {};
    bool churnInitialEligibleHookValid = false;
    const QStringList churnInitialEligibleIds = invokeEligibleCandidateIds(
        textureRenderer, &churnInitialEligibleHookValid);
    const bool churnInitialExact = churnInitialFrameReady
        && churnInitialCommittedHookValid && churnInitialEligibleHookValid
        && churnInitialSnapshot.unique
        && churnInitialSnapshot.visibleIds == churnInitialExpectedIds
        && churnInitialEligibleIds == churnInitialExpectedIds;

    overlay->setProperty("mediaPosition", 605.5);
    double churnShiftedFrameMs = 0.0;
    const bool churnShiftedFrameReady = churnInitialExact
        && waitForCommittedTextures(
            window, textureRenderer, churnShiftedExpectedIds.size(),
            10'000, &churnShiftedFrameMs);
    bool churnShiftedCommittedHookValid = false;
    const DelegateSnapshot churnShiftedSnapshot = churnShiftedFrameReady
        ? snapshotPresentedCandidates(
              window, overlay, 605.5, &churnShiftedCommittedHookValid)
        : DelegateSnapshot {};
    bool churnShiftedEligibleHookValid = false;
    const QStringList churnShiftedEligibleIds = invokeEligibleCandidateIds(
        textureRenderer, &churnShiftedEligibleHookValid);
    const int churnCommittedTextureCount = textureRenderer
        ? textureRenderer->property("committedTextureCount").toInt() : -1;
    const int churnUncommittedTextureCount = textureRenderer
        ? textureRenderer->property("uncommittedTextureCount").toInt() : -1;
    const int churnBudgetScaledTextureCount = textureRenderer
        ? textureRenderer->property("budgetScaledTextureCount").toInt() : -1;
    const int churnEmptyTextureCount = textureRenderer
        ? textureRenderer->property("emptyTextureCount").toInt() : -1;
    const int churnBudgetDeferredTextureCount = textureRenderer
        ? textureRenderer->property("budgetDeferredTextureCount").toInt() : -1;
    const int churnTransientFailureTextureCount = textureRenderer
        ? textureRenderer->property("transientFailureTextureCount").toInt() : -1;
    const int churnPermanentRejectedTextureCount = textureRenderer
        ? textureRenderer->property("permanentRejectedTextureCount").toInt() : -1;
    const bool longTextureChurnValid = churnShiftedFrameReady
        && churnShiftedCommittedHookValid && churnShiftedEligibleHookValid
        && churnShiftedSnapshot.unique
        && churnShiftedSnapshot.visibleIds == churnShiftedExpectedIds
        && churnShiftedEligibleIds == churnShiftedExpectedIds
        && churnShiftedSnapshot.visibleIds == churnShiftedEligibleIds
        && churnCommittedTextureCount == churnShiftedEligibleIds.size()
        && churnUncommittedTextureCount == 0;

    overlay->setProperty("comments", QVariantList {});
    double clearedAfterTextureMs = 0.0;
    const bool clearedAfterTexture = waitForTimelineReady(
        overlay, 0, 5'000, &clearedAfterTextureMs);
    double clearedTextureFrameMs = 0.0;
    const bool clearedTextureFrameReady = clearedAfterTexture
        && waitForFrame(window, 5'000, &clearedTextureFrameMs);
    const qint64 clearedEstimatedCachedRasterBytes = textureRenderer
        ? textureRenderer->property("estimatedCachedRasterBytes").toLongLong() : -1;
    const int clearedCommittedTextureCount = textureRenderer
        ? textureRenderer->property("committedTextureCount").toInt() : -1;
    const int clearedUncommittedTextureCount = textureRenderer
        ? textureRenderer->property("uncommittedTextureCount").toInt() : -1;
    const bool clearedTextureStatsZero = clearedTextureFrameReady
        && clearedEstimatedCachedRasterBytes == 0
        && clearedCommittedTextureCount == 0
        && clearedUncommittedTextureCount == 0;
    window->setWidth(1920);
    window->setHeight(1080);
    overlay->setProperty("fontSize", 36.0);
    overlay->setProperty("commentOpacity", 0.9);
    overlay->setProperty("scrollDuration", 9.0);
    overlay->setProperty("displayArea", 1.0);
    overlay->setProperty("density", 30);
    overlay->setProperty("timeOffset", 0.0);
    overlay->setProperty("blockedTerms", QString {});
    overlay->setProperty("showScroll", true);
    overlay->setProperty("showTop", true);
    overlay->setProperty("showBottom", true);
    overlay->setProperty("topMargin", 0.0);
    overlay->setProperty("danmakuEnabled", true);
    overlay->setProperty("paused", true);
    overlay->setProperty("buffering", false);
    overlay->setProperty("playbackRate", 1.0);
    overlay->setProperty("mediaPosition", 605.5);
    overlay->setProperty("comments", fixture.strictComments);
    double textureRestoreReadyMs = 0.0;
    const bool textureRestoreReady = waitForTimelineReady(
        overlay, strictCommentCount, 15'000, &textureRestoreReadyMs);
    double textureRestoreFrameMs = 0.0;
    const bool textureRestoreFrameReady = textureRestoreReady
        && waitForFrame(window, 5'000, &textureRestoreFrameMs);
    const bool textureScenarioStateRestored = textureRestoreFrameReady
        && window->width() == 1920 && window->height() == 1080
        && timelineCount(overlay) == strictCommentCount;
    const bool textureBudgetBounded = activeTextureBudgetBounded
        && longTextureChurnValid && clearedTextureStatsZero
        && textureScenarioStateRestored;

    const int over100 = static_cast<int>(std::count_if(
        frameIntervals.cbegin(), frameIntervals.cend(),
        [](double value) { return value > 100.0; }));
    const int over250 = static_cast<int>(std::count_if(
        frameIntervals.cbegin(), frameIntervals.cend(),
        [](double value) { return value > 250.0; }));
    const double over100Ratio = frameIntervals.isEmpty()
        ? 1.0
        : static_cast<double>(over100) / frameIntervals.size();
    const bool hooksValid = initialFrameReady && rendererValid
        && committedPresentationHookValid
        && prepareSamples.size() == loadGenerationCount
        && seekFrameSamples.size() == loadGenerationCount
        && strictReady && strictFirstFrameReady && firstEligibleFrameValid
        && frameIntervals.size() >= 20 && qmlWarnings.isEmpty();

    const QJsonObject hostedAttributes {
        {QStringLiteral("evidence"), QStringLiteral("qt-offscreen-software")},
        {QStringLiteral("enforcement"), QStringLiteral("hosted-catastrophe-only")},
        {QStringLiteral("strictAbsoluteEvidence"), false},
        {QStringLiteral("fixtureProfile"), QStringLiteral("hosted-20k")},
        {QStringLiteral("resolution"), QStringLiteral("1920x1080")},
    };
    const QJsonObject hostedStrictFixtureAttributes {
        {QStringLiteral("evidence"), QStringLiteral("qt-offscreen-software")},
        {QStringLiteral("enforcement"), QStringLiteral("hosted-observation-only")},
        {QStringLiteral("strictAbsoluteEvidence"), false},
        {QStringLiteral("fixtureProfile"), QStringLiteral("strict-100k")},
        {QStringLiteral("resolution"), QStringLiteral("1920x1080")},
        {QStringLiteral("notEquivalentTo"), QStringLiteral("D3D11-4K60-external-present")},
    };
    QJsonArray metrics;
    metrics.push_back(metric(
        QStringLiteral("danmaku.hosted_smoke.timeline_prepare_20k_ms"),
        QStringLiteral("ms"), prepareSamples, hostedAttributes));
    metrics.push_back(metric(
        QStringLiteral("danmaku.hosted_smoke.dense_seek_to_frame_candidate_ms"),
        QStringLiteral("ms"), seekFrameSamples, hostedAttributes));
    metrics.push_back(metric(
        QStringLiteral("danmaku.hosted_smoke.frame_interval_candidate_ms"),
        QStringLiteral("ms"), frameIntervals, hostedAttributes));
    metrics.push_back(metric(
        QStringLiteral("danmaku.hosted_smoke.frame_over_100_ratio"),
        QStringLiteral("ratio"), {over100Ratio}, hostedAttributes));
    metrics.push_back(metric(
        QStringLiteral("danmaku.hosted_smoke.frame_gap_over_250_count"),
        QStringLiteral("count"), {static_cast<double>(over250)}, hostedAttributes));
    metrics.push_back(metric(
        QStringLiteral("danmaku.hosted_observation.timeline_prepare_100k_to_first_eligible_frame_candidate_ms"),
        QStringLiteral("ms"), {strictLoadToFirstEligibleFrameMs},
        hostedStrictFixtureAttributes));

    QJsonArray invariants;
    invariants.push_back(invariant(
        QStringLiteral("danmaku.fixture_hash_and_oracle_valid"),
        fixture.totalCount == strictCommentCount
            && fixture.hostedComments.size() == hostedCommentCount
            && fixture.strictComments.size() == strictCommentCount
            && fixture.sha256 == expectedFixtureSha,
        {{QStringLiteral("fixtureSha256"), fixture.sha256},
         {QStringLiteral("strictCount"), fixture.totalCount},
         {QStringLiteral("hostedCount"), fixture.hostedComments.size()}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.measurement_hooks_valid"), hooksValid,
        {{QStringLiteral("qpaPlatform"), QGuiApplication::platformName()},
         {QStringLiteral("rendererApi"), rendererName(graphicsApi)},
         {QStringLiteral("initialFrameMs"), initialFrameMs},
         {QStringLiteral("strict100kReadyWaitMs"), strictReadyWaitMs},
         {QStringLiteral("strict100kFirstFrameWaitMs"), strictFirstFrameWaitMs},
         {QStringLiteral("strict100kLoadToFirstEligibleFrameCandidateMs"),
          strictLoadToFirstEligibleFrameMs},
         {QStringLiteral("strict100kFirstEligibleFrameValid"), firstEligibleFrameValid},
         {QStringLiteral("committedPresentationHookValid"),
          committedPresentationHookValid},
         {QStringLiteral("strictAbsoluteEvidence"), false},
         {QStringLiteral("evidenceBoundary"),
          QStringLiteral("Hosted offscreen software is not D3D11 4K60 external Present/pixel evidence.")},
         {QStringLiteral("frameSampleCount"), frameIntervals.size()},
         {QStringLiteral("qmlWarnings"), QJsonArray::fromStringList(qmlWarnings)}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.hosted_non_background_pixels_present"),
        strictFirstFrameReady && !strictFirstEligibleFrame.isNull()
            && nonBackgroundPixelCount > 0,
        {{QStringLiteral("nonBackgroundPixelCount"),
          static_cast<double>(nonBackgroundPixelCount)},
         {QStringLiteral("imageWidth"), strictFirstEligibleFrame.width()},
         {QStringLiteral("imageHeight"), strictFirstEligibleFrame.height()},
         {QStringLiteral("backgroundColor"), QStringLiteral("#050505")},
         {QStringLiteral("screenshotRequested"), !screenshotPath.isEmpty()},
         {QStringLiteral("screenshotSaved"), screenshotSaved},
         {QStringLiteral("evidenceBoundary"),
          QStringLiteral("Hosted grabWindow offscreen-software smoke only; not D3D11 4K60 external pixel/Present evidence.")}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.hosted_texture_budget_bounded"),
        textureBudgetBounded,
        {{QStringLiteral("sourceCommentCount"), static_cast<int>(longTextureComments.size())},
         {QStringLiteral("eligibleCandidateCount"), longEligibleIds.size()},
         {QStringLiteral("committedVisibleCandidateCount"),
          longTextureSnapshot.visibleIds.size()},
         {QStringLiteral("textLength"), longTextureTextLength},
         {QStringLiteral("estimatedCachedRasterBytes"),
          static_cast<double>(estimatedCachedRasterBytes)},
         {QStringLiteral("texturePayloadBudgetBytes"),
          static_cast<double>(texturePayloadBudgetBytes)},
         {QStringLiteral("committedTextureCount"), committedTextureCount},
         {QStringLiteral("budgetScaledTextureCount"), budgetScaledTextureCount},
         {QStringLiteral("uncommittedTextureCount"), uncommittedTextureCount},
         {QStringLiteral("emptyTextureCount"), emptyTextureCount},
         {QStringLiteral("budgetDeferredTextureCount"), budgetDeferredTextureCount},
         {QStringLiteral("transientFailureTextureCount"), transientFailureTextureCount},
         {QStringLiteral("permanentRejectedTextureCount"), permanentRejectedTextureCount},
         {QStringLiteral("rasterStatusAccountingValid"),
          activeRasterStatusAccountingValid},
         {QStringLiteral("clearedEstimatedCachedRasterBytes"),
          static_cast<double>(clearedEstimatedCachedRasterBytes)},
         {QStringLiteral("clearedCommittedTextureCount"), clearedCommittedTextureCount},
         {QStringLiteral("clearedUncommittedTextureCount"), clearedUncommittedTextureCount},
         {QStringLiteral("timelineReadyMs"), longTextureReadyMs},
         {QStringLiteral("frameMs"), longTextureFrameMs},
         {QStringLiteral("clearedFrameMs"), clearedTextureFrameMs},
         {QStringLiteral("normalStateRestored"), textureScenarioStateRestored},
         {QStringLiteral("restoreReadyMs"), textureRestoreReadyMs},
         {QStringLiteral("restoreFrameMs"), textureRestoreFrameMs},
         {QStringLiteral("evidenceBoundary"),
          QStringLiteral("Hosted offscreen-software texture accounting smoke only; not D3D11 4K60 GPU residency evidence.")}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.hosted_texture_timeline_churn_exact"),
        longTextureChurnValid,
        {{QStringLiteral("sourceTimelineCount"),
          static_cast<int>(longTextureChurnComments.size())},
         {QStringLiteral("initialExpectedCount"), churnInitialExpectedIds.size()},
         {QStringLiteral("initialEligibleCandidateCount"),
          churnInitialEligibleIds.size()},
         {QStringLiteral("initialCommittedVisibleCandidateCount"),
          churnInitialSnapshot.visibleIds.size()},
         {QStringLiteral("shiftedExpectedCount"), churnShiftedExpectedIds.size()},
         {QStringLiteral("shiftedEligibleCandidateCount"),
          churnShiftedEligibleIds.size()},
         {QStringLiteral("shiftedCommittedVisibleCandidateCount"),
          churnShiftedSnapshot.visibleIds.size()},
         {QStringLiteral("exitedCount"), 25},
         {QStringLiteral("enteredCount"), 25},
         {QStringLiteral("committedTextureCount"), churnCommittedTextureCount},
         {QStringLiteral("uncommittedTextureCount"), churnUncommittedTextureCount},
         {QStringLiteral("budgetScaledTextureCount"), churnBudgetScaledTextureCount},
         {QStringLiteral("emptyTextureCount"), churnEmptyTextureCount},
         {QStringLiteral("budgetDeferredTextureCount"),
          churnBudgetDeferredTextureCount},
         {QStringLiteral("transientFailureTextureCount"),
          churnTransientFailureTextureCount},
         {QStringLiteral("permanentRejectedTextureCount"),
          churnPermanentRejectedTextureCount},
         {QStringLiteral("initialExact"), churnInitialExact},
         {QStringLiteral("shiftedMissingSample"), limitedIds(setDifference(
              churnShiftedExpectedIds, churnShiftedSnapshot.visibleIds))},
         {QStringLiteral("shiftedUnexpectedSample"), limitedIds(setDifference(
              churnShiftedSnapshot.visibleIds, churnShiftedExpectedIds))},
         {QStringLiteral("timelineReadyMs"), churnReadyMs},
         {QStringLiteral("initialFrameMs"), churnInitialFrameMs},
         {QStringLiteral("shiftedFrameMs"), churnShiftedFrameMs},
         {QStringLiteral("evidenceBoundary"),
          QStringLiteral("Hosted offscreen-software committed-texture churn evidence only; not D3D11 4K60 GPU residency evidence.")}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.timeline_semantics_valid"),
        timelineValid && exactSetValid,
        {{QStringLiteral("hostedTimelineFingerprint"), canonicalTimelineFingerprint},
         {QStringLiteral("strictTimelineFingerprint"), strictTimeline.fingerprint},
         {QStringLiteral("eligibleExactSetValid"), exactSetValid}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.no_stale_generation"),
        latestSeekValid && noDuplicateEarlyOrStale,
        {{QStringLiteral("finalVisibleFingerprint"), canonicalVisibleFingerprint},
         {QStringLiteral("generationReadyMs"), generationReadyMs},
         {QStringLiteral("generationFrameMs"), generationFrameMs},
         {QStringLiteral("generationTimelineCurrent"), generationTimelineCurrent},
         {QStringLiteral("generationPixelsCurrent"), generationPixelsCurrent}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.pause_buffer_rate_sync_valid"), pauseBufferRateValid,
        {{QStringLiteral("pausedAdvanceSeconds"), pausedEnd - pausedStart},
         {QStringLiteral("bufferingAdvanceSeconds"), bufferingEnd - bufferingStart},
         {QStringLiteral("rateChecks"), rateChecks}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.style_filter_updates_stable"), styleFilterValid,
        {{QStringLiteral("styleFrameCandidateMs"), styleFrameMs},
         {QStringLiteral("expectedVisibleCount"), filteredExact.expectedIds.size()},
         {QStringLiteral("actualVisibleCount"), filteredExact.actualIds.size()},
         {QStringLiteral("missingCount"), filteredExact.missingIds.size()},
         {QStringLiteral("unexpectedCount"), filteredExact.unexpectedIds.size()},
         {QStringLiteral("missingSample"), limitedIds(filteredExact.missingIds)},
         {QStringLiteral("unexpectedSample"), limitedIds(filteredExact.unexpectedIds)}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.no_unbounded_queue_or_delegate_growth"),
        boundedGrowth && textureBudgetBounded,
        {{QStringLiteral("expectedDelegateCount"), strictFirstFrame.delegateCount},
         {QStringLiteral("actualDelegateCount"), bounded.delegateCount},
         {QStringLiteral("eligibleCandidateCount"), longEligibleIds.size()},
         {QStringLiteral("committedVisibleCandidateCount"),
          longTextureSnapshot.visibleIds.size()},
         {QStringLiteral("estimatedCachedRasterBytes"),
          static_cast<double>(estimatedCachedRasterBytes)},
         {QStringLiteral("texturePayloadBudgetBytes"),
          static_cast<double>(texturePayloadBudgetBytes)},
         {QStringLiteral("committedTextureCount"), committedTextureCount},
         {QStringLiteral("budgetScaledTextureCount"), budgetScaledTextureCount},
         {QStringLiteral("uncommittedTextureCount"), uncommittedTextureCount},
         {QStringLiteral("emptyTextureCount"), emptyTextureCount},
         {QStringLiteral("budgetDeferredTextureCount"), budgetDeferredTextureCount},
         {QStringLiteral("transientFailureTextureCount"), transientFailureTextureCount},
         {QStringLiteral("permanentRejectedTextureCount"), permanentRejectedTextureCount},
         {QStringLiteral("rasterStatusAccountingValid"),
          activeRasterStatusAccountingValid},
         {QStringLiteral("clearedEstimatedCachedRasterBytes"),
          static_cast<double>(clearedEstimatedCachedRasterBytes)},
         {QStringLiteral("clearedCommittedTextureCount"), clearedCommittedTextureCount},
         {QStringLiteral("clearedUncommittedTextureCount"), clearedUncommittedTextureCount},
         {QStringLiteral("clearedTextureStatsZero"), clearedTextureStatsZero},
         {QStringLiteral("longTextureChurnValid"), longTextureChurnValid},
         {QStringLiteral("churnCommittedTextureCount"), churnCommittedTextureCount},
         {QStringLiteral("churnUncommittedTextureCount"), churnUncommittedTextureCount},
         {QStringLiteral("churnEmptyTextureCount"), churnEmptyTextureCount},
         {QStringLiteral("churnBudgetDeferredTextureCount"),
          churnBudgetDeferredTextureCount},
         {QStringLiteral("churnTransientFailureTextureCount"),
          churnTransientFailureTextureCount},
         {QStringLiteral("churnPermanentRejectedTextureCount"),
          churnPermanentRejectedTextureCount},
         {QStringLiteral("textureBudgetBounded"), textureBudgetBounded}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.hosted_eligible_exact_set_valid"), exactSetValid,
        {{QStringLiteral("positions"), QJsonArray {605.5, 900.5}},
         {QStringLiteral("expected605Count"), exact605.expectedIds.size()},
         {QStringLiteral("actual605Count"), exact605.actualIds.size()},
         {QStringLiteral("missing605Count"), exact605.missingIds.size()},
         {QStringLiteral("unexpected605Count"), exact605.unexpectedIds.size()},
         {QStringLiteral("missing605Sample"), limitedIds(exact605.missingIds)},
         {QStringLiteral("unexpected605Sample"), limitedIds(exact605.unexpectedIds)},
         {QStringLiteral("expected900Count"), exact900.expectedIds.size()},
         {QStringLiteral("actual900Count"), exact900.actualIds.size()},
         {QStringLiteral("missing900Count"), exact900.missingIds.size()},
         {QStringLiteral("unexpected900Count"), exact900.unexpectedIds.size()},
         {QStringLiteral("reverseSeekExact"), reverseSeekExact.exact},
         {QStringLiteral("oracleIndependentFromProduction"), true},
         {QStringLiteral("preTruncationApplied"), false}}));
    invariants.push_back(invariant(
        QStringLiteral("danmaku.hosted_no_duplicate_early_or_stale_candidates"),
        noDuplicateEarlyOrStale,
        {{QStringLiteral("605Unique"), exact605.unique},
         {QStringLiteral("900Unique"), exact900.unique},
         {QStringLiteral("reverseUnique"), reverseSeekExact.unique},
         {QStringLiteral("unexpected605Count"), exact605.unexpectedIds.size()},
         {QStringLiteral("unexpected900Count"), exact900.unexpectedIds.size()},
         {QStringLiteral("unexpectedReverseCount"), reverseSeekExact.unexpectedIds.size()}}));

    const QJsonObject environmentDetails {
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
        {QStringLiteral("buildType"), QString::fromLatin1(YANAMI_PERF_BUILD_TYPE)},
        {QStringLiteral("yanamiVersion"), QString::fromLatin1(YANAMI_PERF_VERSION)},
        {QStringLiteral("qpaPlatform"), QGuiApplication::platformName()},
        {QStringLiteral("rendererApi"), rendererName(graphicsApi)},
    };
    const QString fingerprint = QString::fromLocal8Bit(
        qgetenv("YANAMI_PERF_MACHINE_FINGERPRINT")).trimmed();
    const bool referenceMatch = qEnvironmentVariableIntValue(
        "YANAMI_PERF_REFERENCE_MATCH") == 1;
    const QString runId = QStringLiteral("danmaku-hosted-%1")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString finishedAtUtc = QDateTime::currentDateTimeUtc().toString(
        Qt::ISODateWithMs);
    const QJsonObject manifest {
        {QStringLiteral("schemaVersion"), QStringLiteral("1.0")},
        {QStringLiteral("runId"), runId},
        {QStringLiteral("profile"), profile},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("startedAtUtc"), startedAtUtc},
        {QStringLiteral("finishedAtUtc"), finishedAtUtc},
        {QStringLiteral("environment"), QJsonObject{
             {QStringLiteral("fingerprint"), fingerprint.isEmpty()
                     ? QStringLiteral("unorchestrated-danmaku-hosted")
                     : fingerprint},
             {QStringLiteral("referenceMatch"), referenceMatch},
             {QStringLiteral("mismatchReasons"), QJsonArray{}},
             {QStringLiteral("details"), environmentDetails},
         }},
        {QStringLiteral("fixtures"), QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("DanmakuDensity-v1")},
             {QStringLiteral("version"), QStringLiteral("1")},
             {QStringLiteral("sha256"), fixture.sha256},
             {QStringLiteral("validated"), true},
             {QStringLiteral("details"), QJsonObject{
                  {QStringLiteral("strictCommentCount"), fixture.totalCount},
                  {QStringLiteral("hostedCommentCount"), fixture.hostedComments.size()},
              }},
         }}},
        {QStringLiteral("suites"), QJsonArray{QStringLiteral("danmaku")}},
        {QStringLiteral("metrics"), metrics},
        {QStringLiteral("invariants"), invariants},
        {QStringLiteral("reasons"), QJsonArray{}},
    };

    QString writeError;
    const QString outputPath = parser.value(QStringLiteral("output"));
    if (!writeManifest(outputPath, manifest, &writeError)) {
        qCritical().noquote() << "Unable to write danmaku run manifest:" << writeError;
        return 2;
    }
    qInfo().noquote()
        << "danmaku hosted manifest" << outputPath
        << "frames=" << frameIntervals.size()
        << "invariantsPassed=" << (timelineValid && exactSetValid
                                      && noDuplicateEarlyOrStale && latestSeekValid
                                      && pauseBufferRateValid && styleFilterValid
                                      && boundedGrowth && textureBudgetBounded
                                      && hooksValid);
    return 0;
}
