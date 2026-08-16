#pragma once

#include "BackendInfrastructure.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QThreadPool>

#include <optional>

struct SessionCapabilities
{
    bool administrator = false;
    bool canDownload = false;
    bool canDelete = false;

    friend bool operator==(
        const SessionCapabilities &left,
        const SessionCapabilities &right) = default;
};

// Owns the complete Emby authentication state machine. It is both the typed
// SessionPort consumed by presentation and the lifecycle owner observed by the
// composition root when all other feature coordinators must fence old work.
class SessionCoordinator final : public SessionPort
{
    Q_OBJECT

public:
    SessionCoordinator(
        RuntimeHost &runtimeHost,
        QThreadPool &controlPool,
        StatusSink &statusSink,
        QObject *parent = nullptr);
    ~SessionCoordinator() override;

    bool initialize();
    void shutdown();
    void drain();

    bool connected() const override { return m_connected; }
    quint64 generation() const override { return m_generation; }
    bool busy() const override { return m_busy; }
    QString displayName() const override { return m_displayName; }
    QString serverUrl() const override { return m_serverUrl; }
    QString userName() const override { return m_userName; }
    QString serverDomain() const override { return m_serverDomain; }
    bool administrator() const override
    { return m_capabilities.administrator; }
    bool canDownload() const override { return m_capabilities.canDownload; }
    bool canDelete() const override { return m_capabilities.canDelete; }

    quint64 lifecycleFence() const { return m_lifecycleFence; }
    bool transitioning() const { return m_transitioning; }

    void login(
        const QString &requestId,
        const QString &serverName,
        const QString &serverUrl,
        const QString &userName,
        const QString &password,
        bool allowInsecureHttp) override;
    void logout(const QString &requestId) override;

    // Catalog/cache may replace only the capabilities belonging to the
    // currently committed session. A stale generation is rejected.
    bool replaceCapabilities(
        quint64 sessionGeneration,
        const SessionCapabilities &capabilities);

signals:
    void initialized(quint64 sessionGeneration, bool connected);
    void transitionStarted(
        quint64 lifecycleFence,
        SessionPort::Operation operation,
        const QString &requestId);
    void committed(quint64 sessionGeneration, bool connected);
    void lifecycleFenced(quint64 lifecycleFence);

private:
    void startOperation(
        SessionPort::Operation operation,
        const QString &requestId,
        const QString &pendingUserName,
        std::function<YanamiOperationResult()> work);
    void finishOperation();
    void rejectImmediately(
        const QString &requestId,
        SessionPort::Operation operation,
        const QString &message);
    bool refreshIdentity(bool connectedSession);
    bool applyIdentityPayload(
        const QByteArray &payload,
        bool connectedSession);
    QString operationName(SessionPort::Operation operation) const;

    RuntimeHost &m_runtimeHost;
    QThreadPool &m_controlPool;
    StatusSink &m_statusSink;
    QFutureWatcher<YanamiOperationResult> m_watcher;
    QElapsedTimer m_operationTimer;
    std::optional<SessionPort::Operation> m_operation;
    QString m_operationRequestId;
    QString m_pendingUserName;
    QString m_pendingDisplayName;
    QString m_pendingServerUrl;
    QString m_pendingServerDomain;
    QString m_displayName;
    QString m_serverUrl;
    QString m_userName;
    QString m_serverDomain;
    SessionCapabilities m_capabilities;
    quint64 m_generation = 0;
    quint64 m_lifecycleFence = 0;
    bool m_initialized = false;
    bool m_acceptingRequests = false;
    bool m_connected = false;
    bool m_busy = false;
    bool m_transitioning = false;
};
