#include "PlaybackCoordinator.hpp"

#include "BackendInfrastructure.hpp"
#include "PlaybackDescriptor.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace {

constexpr int desktopSchemaVersion = 8;
const QString playbackPrepareLaneKey = QStringLiteral("playback.prepare");

struct PlaybackFailureSummary
{
    QString stage = QStringLiteral("unknown");
    QString kind = QStringLiteral("backend");
    qint64 stageElapsedMs = -1;
};

PlaybackFailureSummary summarizePlaybackFailure(const QString &error)
{
    PlaybackFailureSummary summary;
    static const QRegularExpression stagePattern(QStringLiteral(
        R"(playback preparation stage ([a-z-]+) failed after (\d+) ms:)"));
    static const QRegularExpression authenticationStatusPattern(
        QStringLiteral(R"((?:http|status(?: code)?)\D{0,4}(?:401|403)\b)"));
    static const QRegularExpression rateLimitStatusPattern(
        QStringLiteral(R"((?:http|status(?: code)?)\D{0,4}429\b)"));
    static const QRegularExpression serverStatusPattern(
        QStringLiteral(R"((?:http|status(?: code)?)\D{0,4}5\d\d\b)"));
    const QRegularExpressionMatch stageMatch = stagePattern.match(error);
    if (stageMatch.hasMatch()) {
        summary.stage = stageMatch.captured(1);
        summary.stageElapsedMs = stageMatch.captured(2).toLongLong();
    }

    const QString normalized = error.toLower();
    if (normalized.contains(QStringLiteral("timeout"))
        || normalized.contains(QStringLiteral("timed out"))) {
        summary.kind = QStringLiteral("timeout");
    } else if (normalized.contains(QStringLiteral("unauthorized"))
               || normalized.contains(QStringLiteral("forbidden"))
               || authenticationStatusPattern.match(normalized).hasMatch()) {
        summary.kind = QStringLiteral("authentication");
    } else if (rateLimitStatusPattern.match(normalized).hasMatch()) {
        summary.kind = QStringLiteral("rate_limit");
    } else if (serverStatusPattern.match(normalized).hasMatch()) {
        summary.kind = QStringLiteral("server");
    } else if (normalized.contains(QStringLiteral("connect"))
               || normalized.contains(QStringLiteral("network"))
               || normalized.contains(QStringLiteral("dns"))
               || normalized.contains(QStringLiteral("socket"))
               || normalized.contains(QStringLiteral("tls"))
               || normalized.contains(QStringLiteral("certificate"))
               || normalized.contains(QStringLiteral("reset by peer"))) {
        summary.kind = QStringLiteral("network");
    } else if (normalized.contains(QStringLiteral("http"))) {
        summary.kind = QStringLiteral("http");
    }
    return summary;
}

QString schemaVersionLabel(const QJsonValue &value)
{
    if (value.isUndefined())
        return QStringLiteral("missing");
    if (!value.isDouble())
        return QStringLiteral("invalid");
    return QString::number(value.toDouble(), 'g', 16);
}

} // namespace

PlaybackCoordinator::PlaybackCoordinator(
    RustBridgeRuntime &runtime,
    QThreadPool &preparePool,
    QThreadPool &reportPool,
    SessionStateProvider sessionStateProvider,
    StatusSink &statusSink,
    QObject *parent)
    : PlaybackPort(parent)
    , m_runtime(runtime)
    , m_preparePool(preparePool)
    , m_reportPool(reportPool)
    , m_sessionStateProvider(std::move(sessionStateProvider))
    , m_statusSink(statusSink)
{
    connect(
        &m_prepareWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this,
        &PlaybackCoordinator::finishPrepare);
    connect(
        &m_reportWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this,
        &PlaybackCoordinator::finishReport);
}

PlaybackCoordinator::~PlaybackCoordinator()
{
    shutdown();
    drain();
}

void PlaybackCoordinator::prepare(
    const QString &requestId,
    const QString &itemId)
{
    prepareInContext(requestId, itemId, {});
}

void PlaybackCoordinator::prepareInContext(
    const QString &requestId,
    const QString &itemId,
    const QVariantMap &context)
{
    // A direct prepare is newer than any deferred switch waiting for a stop
    // report. Advance the serial before it can re-enter and supersede us.
    std::optional<AfterStopRequest> supersededAfterStop =
        std::move(m_afterStop);
    m_afterStop.reset();
    invalidateAfterStop();
    const auto settleSupersededAfterStop = [this, &supersededAfterStop] {
        if (supersededAfterStop.has_value()) {
            emit failed(
                supersededAfterStop->clientRequestId,
                supersededAfterStop->itemId,
                tr("Playback preparation was canceled."));
        }
    };
    const quint64 generation = ++m_prepareGeneration;
    const SessionState session = sessionState();
    qInfo().noquote()
        << "prepare_playback"
        << "phase=requested"
        << "itemId=" << itemId
        << "generation=" << generation;
    if (!available(session)) {
        qWarning().noquote()
            << "prepare_playback"
            << "phase=dropped"
            << "itemId=" << itemId
            << "generation=" << generation
            << "reason="
            << (!m_runtime.ready() ? "backend_not_ready" : "session_unavailable");
        fail(
            requestId,
            itemId,
            tr("Playback is unavailable while the server connection is not ready."));
        settleSupersededAfterStop();
        return;
    }
    if (itemId.isEmpty()) {
        qWarning().noquote()
            << "prepare_playback"
            << "phase=dropped"
            << "itemId=" << itemId
            << "generation=" << generation
            << "reason=invalid_item_id";
        fail(requestId, itemId, tr("The selected media item is invalid."));
        settleSupersededAfterStop();
        return;
    }
    submitPrepare(requestId, itemId, context, generation, session);
    settleSupersededAfterStop();
}

void PlaybackCoordinator::cancelPreparation()
{
    resetPreparation("ui_cancelled", false);
}

void PlaybackCoordinator::switchTo(
    const QString &requestId,
    const QString &itemId,
    double positionSeconds,
    bool paused)
{
    switchToInContext(
        requestId,
        itemId,
        m_currentContext,
        positionSeconds,
        paused);
}

void PlaybackCoordinator::switchToInContext(
    const QString &requestId,
    const QString &itemId,
    const QVariantMap &context,
    double positionSeconds,
    bool paused)
{
    Q_UNUSED(positionSeconds);
    Q_UNUSED(paused);
    const SessionState session = sessionState();
    if (!available(session)) {
        fail(
            requestId,
            itemId,
            tr("Playback is unavailable while the server connection is not ready."));
        return;
    }
    if (itemId.isEmpty()) {
        fail(requestId, itemId, tr("The selected media item is invalid."));
        return;
    }

    std::optional<AfterStopRequest> supersededAfterStop =
        std::move(m_afterStop);
    m_afterStop.reset();
    invalidateAfterStop();
    AfterStopRequest request;
    request.serial = ++m_afterStopSerial;
    request.clientRequestId = requestId;
    request.itemId = itemId;
    request.context = context;
    const std::optional<YanamiPlaybackReportRequest> activeReport =
        m_reportInFlight
        ? std::optional<YanamiPlaybackReportRequest>(m_activeReport)
        : std::nullopt;
    request.reportSessionId = YanamiPlayback::latestStoppedReportSessionId(
        activeReport, m_reportQueue);
    m_afterStop = std::move(request);
    if (m_afterStop->reportSessionId.isEmpty()) {
        AfterStopRequest immediate = std::move(*m_afterStop);
        m_afterStop.reset();
        scheduleAfterStop(std::move(immediate));
    }
    if (supersededAfterStop.has_value()) {
        emit failed(
            supersededAfterStop->clientRequestId,
            supersededAfterStop->itemId,
            tr("Playback preparation was canceled."));
    }
}

void PlaybackCoordinator::report(
    Event event,
    const Snapshot &snapshot)
{
    const SessionState session = sessionState();
    if (!available(session)
        || snapshot.reportSessionId.trimmed().isEmpty()) {
        return;
    }
    YanamiPlayback::enqueueReport(
        m_reportQueue,
        YanamiPlaybackReportRequest {event, snapshot, session.generation});
    startNextReport();
}

void PlaybackCoordinator::fenceSessionTransition(const char *reason)
{
    m_sessionFenced = true;
    resetPreparation(reason, true);
    m_currentContext.clear();
}

void PlaybackCoordinator::resumeAfterSessionTransition()
{
    if (m_shuttingDown)
        return;
    m_sessionFenced = false;
    startNextReport();
    pumpPrepare();
}

void PlaybackCoordinator::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_sessionFenced = true;
    resetPreparation("shutdown", true);
    m_currentContext.clear();
}

void PlaybackCoordinator::drain()
{
    if (m_prepareWatcher.isRunning())
        m_prepareWatcher.waitForFinished();
    if (m_reportWatcher.isRunning())
        m_reportWatcher.waitForFinished();
}

PlaybackCoordinator::SessionState PlaybackCoordinator::sessionState() const
{
    return m_sessionStateProvider
        ? m_sessionStateProvider()
        : SessionState {};
}

bool PlaybackCoordinator::available(const SessionState &session) const
{
    return !m_shuttingDown
        && !m_sessionFenced
        && m_runtime.ready()
        && session.connected;
}

void PlaybackCoordinator::fail(
    const QString &requestId,
    const QString &itemId,
    const QString &message)
{
    m_statusSink.publishStatus(message, true);
    emit failed(requestId, itemId, message);
}

void PlaybackCoordinator::submitPrepare(
    const QString &requestId,
    const QString &itemId,
    const QVariantMap &context,
    quint64 generation,
    const SessionState &session)
{
    const QByteArray contextJson = QJsonDocument::fromVariant(context).toJson(
        QJsonDocument::Compact);
    const QString requestKey = itemId
        + QLatin1Char(':')
        + QString::fromLatin1(
            QCryptographicHash::hash(contextJson, QCryptographicHash::Sha256)
                .toHex().left(16));
    PrepareRequest request {
        requestId,
        itemId,
        context,
        m_requests.beginLatest(
            playbackPrepareLaneKey,
            requestKey,
            session.generation),
        generation,
        QDateTime::currentMSecsSinceEpoch(),
    };

    if (m_activePrepare.has_value() || m_prepareWatcher.isRunning()) {
        std::optional<PrepareRequest> supersededActive =
            std::move(m_activePrepare);
        std::optional<PrepareRequest> supersededQueued =
            std::move(m_queuedPrepare);
        m_activePrepare.reset();
        m_queuedPrepare = std::move(request);
        if (supersededQueued.has_value()) {
            const PrepareRequest &superseded = *supersededQueued;
            qInfo().noquote()
                << "prepare_playback"
                << "phase=dropped"
                << "itemId=" << superseded.itemId
                << "generation=" << superseded.generation
                << "requestId=" << superseded.token.requestId
                << "resourceKey=" << superseded.token.requestKey
                << "lane=" << superseded.token.laneKey
                << "reason=superseded";
        }
        qInfo().noquote()
            << "prepare_playback"
            << "phase=queued"
            << "itemId=" << itemId
            << "generation=" << generation
            << "requestId=" << m_queuedPrepare->token.requestId
            << "resourceKey=" << m_queuedPrepare->token.requestKey
            << "lane=" << m_queuedPrepare->token.laneKey
            << "reason=playback_lane_busy";
        if (supersededActive.has_value()) {
            emit failed(
                supersededActive->clientRequestId,
                supersededActive->itemId,
                tr("Playback preparation was superseded."));
        }
        if (supersededQueued.has_value()) {
            emit failed(
                supersededQueued->clientRequestId,
                supersededQueued->itemId,
                tr("Playback preparation was superseded."));
        }
        return;
    }
    startPrepare(std::move(request));
}

void PlaybackCoordinator::startPrepare(PrepareRequest request)
{
    const SessionState session = sessionState();
    if (!available(session)
        || !m_requests.acceptsLatest(request.token, session.generation)) {
        qInfo().noquote()
            << "prepare_playback"
            << "phase=dropped"
            << "itemId=" << request.itemId
            << "generation=" << request.generation
            << "requestId=" << request.token.requestId
            << "resourceKey=" << request.token.requestKey
            << "lane=" << request.token.laneKey
            << "reason=stale_before_start";
        return;
    }

    const qint64 queueWaitMs = std::max<qint64>(
        0,
        QDateTime::currentMSecsSinceEpoch() - request.enqueuedAtMs);
    const QString itemId = request.itemId;
    const QVariantMap context = request.context;
    m_activePrepare = std::move(request);
    m_prepareTimer.start();
    qInfo().noquote()
        << "prepare_playback"
        << "phase=start"
        << "itemId=" << m_activePrepare->itemId
        << "generation=" << m_activePrepare->generation
        << "requestId=" << m_activePrepare->token.requestId
        << "resourceKey=" << m_activePrepare->token.requestKey
        << "lane=" << m_activePrepare->token.laneKey
        << "queueWaitMs=" << queueWaitMs;
    m_prepareWatcher.setFuture(QtConcurrent::run(
        &m_preparePool,
        [this, itemId, context] {
            return m_runtime.playbackRequest(itemId, context);
        }));
}

void PlaybackCoordinator::finishPrepare()
{
    const YanamiOperationResult operationResult = m_prepareWatcher.result();
    const qint64 elapsedMs = m_prepareTimer.isValid()
        ? m_prepareTimer.elapsed()
        : -1;
    m_prepareTimer.invalidate();
    if (!m_activePrepare.has_value()) {
        qWarning().noquote()
            << "prepare_playback"
            << "phase=dropped"
            << "reason=missing_active_request";
        pumpPrepare();
        return;
    }

    const PrepareRequest request = *m_activePrepare;
    m_activePrepare.reset();
    const SessionState session = sessionState();
    if (!available(session)
        || !m_requests.acceptsLatest(request.token, session.generation)) {
        qInfo().noquote()
            << "prepare_playback"
            << "phase=dropped"
            << "itemId=" << request.itemId
            << "generation=" << request.generation
            << "requestId=" << request.token.requestId
            << "resourceKey=" << request.token.requestKey
            << "lane=" << request.token.laneKey
            << "elapsedMs=" << elapsedMs
            << "reason=stale_result";
    } else {
        applyPrepareResult(request, operationResult, elapsedMs);
    }
    pumpPrepare();
}

void PlaybackCoordinator::pumpPrepare()
{
    if (m_shuttingDown || m_sessionFenced
        || m_prepareWatcher.isRunning()
        || !m_queuedPrepare.has_value()) {
        return;
    }
    PrepareRequest request = std::move(*m_queuedPrepare);
    m_queuedPrepare.reset();
    startPrepare(std::move(request));
}

void PlaybackCoordinator::resetPreparation(
    const char *reason,
    bool clearReports)
{
    m_requests.invalidateLatestLane(playbackPrepareLaneKey);
    const QString cancellationMessage = tr("Playback preparation was canceled.");
    std::optional<AfterStopRequest> canceledAfterStop =
        std::move(m_afterStop);
    std::optional<PrepareRequest> canceledQueued =
        std::move(m_queuedPrepare);
    std::optional<PrepareRequest> canceledActive =
        std::move(m_activePrepare);
    m_afterStop.reset();
    m_queuedPrepare.reset();
    m_activePrepare.reset();
    invalidateAfterStop();
    if (clearReports)
        m_reportQueue.clear();
    if (canceledAfterStop.has_value()) {
        emit failed(
            canceledAfterStop->clientRequestId,
            canceledAfterStop->itemId,
            cancellationMessage);
    }
    if (canceledQueued.has_value()) {
        qInfo().noquote()
            << "prepare_playback"
            << "phase=dropped"
            << "itemId=" << canceledQueued->itemId
            << "generation=" << canceledQueued->generation
            << "requestId=" << canceledQueued->token.requestId
            << "resourceKey=" << canceledQueued->token.requestKey
            << "lane=" << canceledQueued->token.laneKey
            << "reason=" << reason;
        emit failed(
            canceledQueued->clientRequestId,
            canceledQueued->itemId,
            cancellationMessage);
    }
    if (canceledActive.has_value()) {
        qInfo().noquote()
            << "prepare_playback"
            << "phase=invalidated"
            << "itemId=" << canceledActive->itemId
            << "generation=" << canceledActive->generation
            << "requestId=" << canceledActive->token.requestId
            << "resourceKey=" << canceledActive->token.requestKey
            << "lane=" << canceledActive->token.laneKey
            << "reason=" << reason;
        emit failed(
            canceledActive->clientRequestId,
            canceledActive->itemId,
            cancellationMessage);
    }
}

void PlaybackCoordinator::applyPrepareResult(
    const PrepareRequest &request,
    const YanamiOperationResult &operationResult,
    qint64 operationElapsedMs)
{
    if (operationResult.status != 0) {
        const PlaybackFailureSummary failureSummary =
            summarizePlaybackFailure(operationResult.error);
        qWarning().noquote()
            << "prepare_playback"
            << "phase=failed"
            << "itemId=" << request.itemId
            << "generation=" << request.generation
            << "requestId=" << request.token.requestId
            << "resourceKey=" << request.token.requestKey
            << "lane=" << request.token.laneKey
            << "elapsedMs=" << operationElapsedMs
            << "status=" << operationResult.status
            << "reason=backend_error"
            << "stage=" << failureSummary.stage
            << "stageElapsedMs=" << failureSummary.stageElapsedMs
            << "failureKind=" << failureSummary.kind;
        fail(
            request.clientRequestId,
            request.itemId,
            operationResult.error.isEmpty()
                ? tr("The operation failed.")
                : m_statusSink.userFacingBackendError(
                    operationResult.errorCode, operationResult.error));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        operationResult.payload,
        &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning().noquote()
            << "prepare_playback"
            << "phase=failed"
            << "itemId=" << request.itemId
            << "generation=" << request.generation
            << "requestId=" << request.token.requestId
            << "resourceKey=" << request.token.requestKey
            << "lane=" << request.token.laneKey
            << "elapsedMs=" << operationElapsedMs
            << "status=invalid_response"
            << "reason=invalid_json";
        fail(
            request.clientRequestId,
            request.itemId,
            tr("The playback response was invalid."));
        return;
    }

    const QJsonObject object = document.object();
    const QJsonValue schemaVersion = object.value(QStringLiteral("schemaVersion"));
    if (!schemaVersion.isDouble()
        || schemaVersion.toDouble() != static_cast<double>(desktopSchemaVersion)) {
        const QString received = schemaVersionLabel(schemaVersion);
        qWarning().noquote()
            << "backend_schema_incompatible"
            << "response=playback"
            << "expected=" << desktopSchemaVersion
            << "received=" << received;
        fail(
            request.clientRequestId,
            request.itemId,
            tr("The backend playback response uses an incompatible schema version "
               "(expected %1, received %2).")
                .arg(desktopSchemaVersion)
                .arg(received));
        return;
    }

    const QUrl mediaUrl(object.value(QStringLiteral("url")).toString());
    if (!mediaUrl.isValid()) {
        qWarning().noquote()
            << "prepare_playback"
            << "phase=failed"
            << "itemId=" << request.itemId
            << "generation=" << request.generation
            << "requestId=" << request.token.requestId
            << "resourceKey=" << request.token.requestKey
            << "lane=" << request.token.laneKey
            << "elapsedMs=" << operationElapsedMs
            << "status=invalid_response"
            << "reason=invalid_media_url";
        fail(
            request.clientRequestId,
            request.itemId,
            tr("Emby returned an invalid playback URL."));
        return;
    }

    const QString warning =
        object.value(QStringLiteral("danmakuWarning")).toString();
    m_statusSink.publishStatus(
        warning.isEmpty()
            ? tr("Playback ready.")
            : tr("Playback ready; danmaku unavailable: %1").arg(warning),
        false);
    const QJsonObject preparationDiagnostics =
        object.value(QStringLiteral("preparationDiagnostics")).toObject();
    if (!preparationDiagnostics.isEmpty()) {
        qInfo().noquote()
            << "prepare_playback_stages"
            << "itemId=" << request.itemId
            << "generation=" << request.generation
            << "requestId=" << request.token.requestId
            << "totalMs="
            << preparationDiagnostics.value(QStringLiteral("totalMs")).toInt()
            << "itemMs="
            << preparationDiagnostics.value(QStringLiteral("itemMs")).toInt()
            << "resolveMs="
            << preparationDiagnostics.value(QStringLiteral("resolveMs")).toInt()
            << "neighborsMs="
            << preparationDiagnostics.value(QStringLiteral("neighborsMs")).toInt()
            << "neighborsSucceeded="
            << preparationDiagnostics.value(
                   QStringLiteral("neighborsSucceeded")).toBool()
            << "playbackInfoMs="
            << preparationDiagnostics.value(
                   QStringLiteral("playbackInfoMs")).toInt()
            << "planningMs="
            << preparationDiagnostics.value(QStringLiteral("planningMs")).toInt()
            << "resolvedItemChanged="
            << preparationDiagnostics.value(
                   QStringLiteral("resolvedItemChanged")).toBool()
            << "backgroundImageDownloads="
            << preparationDiagnostics.value(
                   QStringLiteral("backgroundImageDownloads")).toInt()
            << "activeImageDownloads="
            << preparationDiagnostics.value(
                   QStringLiteral("activeImageDownloads")).toInt();
    }
    qInfo().noquote()
        << "prepare_playback"
        << "phase=ready"
        << "itemId=" << request.itemId
        << "generation=" << request.generation
        << "requestId=" << request.token.requestId
        << "resourceKey=" << request.token.requestKey
        << "lane=" << request.token.laneKey
        << "elapsedMs=" << operationElapsedMs
        << "status=success";

    m_currentContext = object.value(
        QStringLiteral("playbackContext")).toObject().toVariantMap();
    if (m_currentContext.isEmpty())
        m_currentContext = request.context;
    emit ready(
        request.clientRequestId,
        YanamiPlayback::descriptorFromResponse(object, m_currentContext));
}

void PlaybackCoordinator::invalidateAfterStop()
{
    ++m_afterStopSerial;
    m_afterStop.reset();
}

void PlaybackCoordinator::scheduleAfterStop(AfterStopRequest request)
{
    QTimer::singleShot(
        0,
        this,
        [this, request = std::move(request)] {
            if (m_shuttingDown
                || m_sessionFenced
                || request.serial != m_afterStopSerial) {
                if (!m_shuttingDown
                    && request.serial != m_afterStopSerial) {
                    emit failed(
                        request.clientRequestId,
                        request.itemId,
                        tr("Playback preparation was canceled."));
                }
                return;
            }
            prepareInContext(
                request.clientRequestId,
                request.itemId,
                request.context);
        });
}

void PlaybackCoordinator::startNextReport()
{
    const SessionState session = sessionState();
    YanamiPlayback::discardForeignSessionReports(
        m_reportQueue,
        session.generation,
        !available(session));
    if (m_reportInFlight || m_reportQueue.isEmpty()
        || !available(session)) {
        return;
    }
    m_activeReport = m_reportQueue.dequeue();
    const YanamiPlaybackReportRequest request = m_activeReport;
    m_reportInFlight = true;
    m_reportWatcher.setFuture(QtConcurrent::run(
        &m_reportPool,
        [this, request] {
            return m_runtime.reportPlayback(request.event, request.snapshot);
        }));
}

void PlaybackCoordinator::finishReport()
{
    const YanamiOperationResult operationResult = m_reportWatcher.result();
    const YanamiPlaybackReportRequest completedReport = m_activeReport;
    const bool stopped = completedReport.event == Event::Stopped;
    const QString stoppedReportSessionId = stopped
        ? completedReport.snapshot.reportSessionId
        : QString();
    const SessionState session = sessionState();
    const bool acceptedSession = YanamiPlayback::belongsToSession(
        completedReport,
        session.generation,
        !available(session));

    std::optional<AfterStopRequest> afterStop;
    if (stopped
        && acceptedSession
        && m_afterStop.has_value()
        && !m_afterStop->reportSessionId.isEmpty()
        && stoppedReportSessionId == m_afterStop->reportSessionId) {
        afterStop = std::move(m_afterStop);
        m_afterStop.reset();
    }

    if (operationResult.status != 0) {
        // Playback telemetry is best-effort. A transient network failure must
        // not block stopping or switching; later reports reconcile naturally.
        qWarning().noquote()
            << "playback_report_failed"
            << "event=" << static_cast<int>(completedReport.event)
            << "positionSeconds=" << completedReport.snapshot.positionSeconds
            << "status=" << operationResult.status
            << "reason=backend_error";
    }
    // Retire the completed report before external signals, but keep the lane
    // closed. A direct stoppedReported handler may enqueue; it cannot replace
    // the active watcher until this settlement is complete.
    m_activeReport = {};
    if (stopped && acceptedSession)
        emit stoppedReported();

    m_reportInFlight = false;
    startNextReport();
    if (afterStop.has_value())
        scheduleAfterStop(std::move(*afterStop));
}
