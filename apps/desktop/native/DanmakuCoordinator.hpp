#pragma once

#include "BackendPorts.hpp"
#include "RustBridgeRuntime.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QQueue>
#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class StatusSink;

// Owns every DanDanPlay request and request identity. Danmaku work is kept
// deliberately outside the generic media scheduler: search is latest-wins,
// while automatic matching and applying a selected match are serialized in
// submission order.
class DanmakuCoordinator final : public DanmakuPort
{
    Q_OBJECT

public:
    struct SessionState
    {
        quint64 generation = 0;
        bool connected = false;
    };

    using SessionStateProvider = std::function<SessionState()>;

    // Plain function values keep the coordinator independently testable and,
    // more importantly, let worker tasks retain only copyable callables. The
    // production constructor below binds these operations to RustBridgeRuntime;
    // no QObject or coordinator state is touched from a worker thread.
    struct BackendOperations
    {
        std::function<bool()> ready;
        std::function<YanamiStatusResult()> credentialSource;
        std::function<YanamiOperationResult(
            const QString &, const QString &)> configure;
        std::function<YanamiOperationResult()> clearConfiguration;
        std::function<YanamiOperationResult(
            Operation, const QString &, const QVariantMap &)> danmaku;
    };

    DanmakuCoordinator(
        RustBridgeRuntime &runtime,
        QThreadPool &queryPool,
        QThreadPool &mutationPool,
        SessionStateProvider sessionStateProvider,
        StatusSink &statusSink,
        QObject *parent = nullptr);
    DanmakuCoordinator(
        BackendOperations backend,
        QThreadPool &queryPool,
        QThreadPool &mutationPool,
        SessionStateProvider sessionStateProvider,
        StatusSink &statusSink,
        QObject *parent = nullptr);
    ~DanmakuCoordinator() override;

    bool configured() const override { return m_configured; }
    int credentialSource() const override { return m_credentialSource; }
    bool configurationBusy() const override;

    void search(
        const QString &requestId,
        const QString &itemId,
        const QString &anime) override;
    void loadAutomatically(
        const QString &requestId,
        const QString &itemId) override;
    void applyMatch(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &match,
        const QVariantMap &style) override;
    void configure(
        const QString &requestId,
        const QString &appId,
        const QString &appSecret) override;
    void clearConfiguration(const QString &requestId) override;

    // Called once after RuntimeHost has opened the Rust backend. A failed
    // status read does not invent a credential source; it is reported through
    // the shared structured StatusSink.
    bool initializeCredentialStatus();

    // The composition root invokes started synchronously before login/logout
    // work begins, then committed after the SessionStateProvider exposes the
    // resulting (or restored, after failure) session snapshot.
    void sessionTransitionStarted(
        const char *reason = "session_transition");
    void sessionTransitionCommitted();

    // shutdown fences consumers only. The composition root owns process-wide
    // Rust cancellation and calls it before drain().
    void shutdown();
    void drain();

private:
    struct ConfigurationRequest
    {
        QString clientRequestId;
        ConfigurationOperation operation = ConfigurationOperation::Configure;
        quint64 identity = 0;
        bool terminalDelivered = false;
    };

    struct OperationRequest
    {
        QString clientRequestId;
        QString itemId;
        Operation operation = Operation::Search;
        QVariantMap payload;
        quint64 identity = 0;
        quint64 sessionGeneration = 0;
        qint64 enqueuedAtMs = 0;
        std::shared_ptr<std::atomic_bool> decodeAllowed;
        bool terminalDelivered = false;
    };

    struct OperationWorkResult
    {
        int status = 1;
        QString errorCode;
        QString error;
        QVariantMap result;
        bool responseValid = false;
        qint64 decodeElapsedNs = 0;
    };

    SessionState sessionState() const;
    bool backendReady() const;
    bool operationAvailable(const SessionState &session) const;
    bool refreshCredentialState(bool publishFailure);

    void startConfiguration(
        ConfigurationRequest request,
        std::function<YanamiOperationResult()> work);
    void finishConfiguration();
    void rejectConfiguration(
        const QString &requestId,
        ConfigurationOperation operation,
        const QString &message);
    void failConfiguration(
        ConfigurationRequest &request,
        const QString &message);

    void submitOperation(OperationRequest request);
    void submitSearch(OperationRequest request);
    void startSearch(OperationRequest request);
    void finishSearch();
    void pumpSearch();

    void submitMutation(OperationRequest request);
    void startMutation(OperationRequest request);
    void finishMutation();
    void pumpMutation();
    bool mutationPendingFor(const QString &itemId) const;
    void fenceSearchForMutation(const QString &itemId);

    bool accepts(const OperationRequest &request) const;
    static OperationWorkResult prepareResult(
        Operation operation,
        YanamiOperationResult operationResult,
        const std::shared_ptr<std::atomic_bool> &decodeAllowed);
    static bool decodeResult(
        Operation operation,
        const QByteArray &payload,
        const std::shared_ptr<std::atomic_bool> &decodeAllowed,
        QVariantMap *result);
    QString backendFailure(
        const YanamiOperationResult &result) const;
    QString backendFailure(
        const OperationWorkResult &result) const;
    void applyResult(
        OperationRequest &request,
        const OperationWorkResult &result,
        qint64 elapsedMs);
    void completeOperation(
        OperationRequest &request,
        const QVariantMap &result);
    void failOperation(
        OperationRequest &request,
        const QString &message,
        bool nonModal,
        bool publishStatus = false);
    void rejectOperation(
        const QString &requestId,
        const QString &itemId,
        Operation operation,
        const QString &message,
        bool nonModal,
        bool publishStatus = false);
    static const char *operationName(Operation operation);

    BackendOperations m_backend;
    QThreadPool &m_queryPool;
    QThreadPool &m_mutationPool;
    SessionStateProvider m_sessionStateProvider;
    StatusSink &m_statusSink;

    QFutureWatcher<YanamiOperationResult> m_configurationWatcher;
    QElapsedTimer m_configurationTimer;
    std::optional<ConfigurationRequest> m_activeConfiguration;

    QFutureWatcher<OperationWorkResult> m_searchWatcher;
    QElapsedTimer m_searchTimer;
    std::optional<OperationRequest> m_activeSearch;
    std::optional<OperationRequest> m_queuedSearch;
    quint64 m_latestSearchIdentity = 0;

    QFutureWatcher<OperationWorkResult> m_mutationWatcher;
    QElapsedTimer m_mutationTimer;
    std::optional<OperationRequest> m_activeMutation;
    QQueue<OperationRequest> m_mutationQueue;

    quint64 m_nextRequestIdentity = 0;
    bool m_configured = false;
    int m_credentialSource = 0;
    bool m_initialized = false;
    bool m_acceptingRequests = false;
    bool m_sessionFenced = false;
    bool m_shuttingDown = false;
};
