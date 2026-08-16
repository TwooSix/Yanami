#pragma once

#include "BackendPorts.hpp"
#include "PlaybackReportQueue.hpp"
#include "RequestCoordinator.hpp"
#include "RustBridgeRuntime.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QQueue>
#include <QThreadPool>
#include <QVariantMap>

#include <functional>
#include <optional>

class StatusSink;

class PlaybackCoordinator final : public PlaybackPort
{
    Q_OBJECT

public:
    struct SessionState
    {
        quint64 generation = 0;
        bool connected = false;
    };

    using SessionStateProvider = std::function<SessionState()>;

    PlaybackCoordinator(
        RustBridgeRuntime &runtime,
        QThreadPool &preparePool,
        QThreadPool &reportPool,
        SessionStateProvider sessionStateProvider,
        StatusSink &statusSink,
        QObject *parent = nullptr);
    ~PlaybackCoordinator() override;

    void prepare(
        const QString &requestId,
        const QString &itemId) override;
    void prepareInContext(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &context) override;
    void cancelPreparation() override;
    void switchTo(
        const QString &requestId,
        const QString &itemId,
        double positionSeconds,
        bool paused) override;
    void switchToInContext(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &context,
        double positionSeconds,
        bool paused) override;
    void report(Event event, const Snapshot &snapshot) override;

    // The composition root calls the fence synchronously before a login or
    // logout starts. Results already in flight remain safe to drain, but can
    // no longer publish into the old or incoming session.
    void fenceSessionTransition(const char *reason = "session_transition");
    void resumeAfterSessionTransition();

    // shutdown() only fences this feature. The composition root owns the
    // process-wide Rust cancellation and must invoke it before drain().
    void shutdown();
    void drain();

private:
    struct PrepareRequest
    {
        QString clientRequestId;
        QString itemId;
        QVariantMap context;
        LatestRequestToken token;
        quint64 generation = 0;
        qint64 enqueuedAtMs = 0;
    };

    struct AfterStopRequest
    {
        quint64 serial = 0;
        QString clientRequestId;
        QString itemId;
        QVariantMap context;
        QString reportSessionId;
    };

    SessionState sessionState() const;
    bool available(const SessionState &session) const;
    void fail(
        const QString &requestId,
        const QString &itemId,
        const QString &message);

    void submitPrepare(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &context,
        quint64 generation,
        const SessionState &session);
    void startPrepare(PrepareRequest request);
    void finishPrepare();
    void pumpPrepare();
    void resetPreparation(const char *reason, bool clearReports);
    void applyPrepareResult(
        const PrepareRequest &request,
        const YanamiOperationResult &operationResult,
        qint64 operationElapsedMs);

    void invalidateAfterStop();
    void scheduleAfterStop(AfterStopRequest request);
    void startNextReport();
    void finishReport();

    RustBridgeRuntime &m_runtime;
    QThreadPool &m_preparePool;
    QThreadPool &m_reportPool;
    SessionStateProvider m_sessionStateProvider;
    StatusSink &m_statusSink;
    RequestCoordinator m_requests;

    QFutureWatcher<YanamiOperationResult> m_prepareWatcher;
    QElapsedTimer m_prepareTimer;
    std::optional<PrepareRequest> m_activePrepare;
    std::optional<PrepareRequest> m_queuedPrepare;
    quint64 m_prepareGeneration = 0;
    QVariantMap m_currentContext;

    QFutureWatcher<YanamiOperationResult> m_reportWatcher;
    QQueue<YanamiPlaybackReportRequest> m_reportQueue;
    YanamiPlaybackReportRequest m_activeReport;
    bool m_reportInFlight = false;

    std::optional<AfterStopRequest> m_afterStop;
    quint64 m_afterStopSerial = 0;
    bool m_sessionFenced = false;
    bool m_shuttingDown = false;
};
