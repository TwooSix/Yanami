#include "SessionCoordinator.hpp"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QtConcurrentRun>

namespace {

constexpr int desktopSchemaVersion = 8;

} // namespace

SessionCoordinator::SessionCoordinator(
    RuntimeHost &runtimeHost,
    QThreadPool &controlPool,
    StatusSink &statusSink,
    QObject *parent)
    : SessionPort(parent)
    , m_runtimeHost(runtimeHost)
    , m_controlPool(controlPool)
    , m_statusSink(statusSink)
{
    connect(&m_watcher, &QFutureWatcher<YanamiOperationResult>::finished,
        this, &SessionCoordinator::finishOperation);
}

SessionCoordinator::~SessionCoordinator()
{
    shutdown();
    drain();
}

bool SessionCoordinator::initialize()
{
    if (m_initialized)
        return m_runtimeHost.ready();

    m_initialized = true;
    m_acceptingRequests = m_runtimeHost.ready();
    ++m_lifecycleFence;
    ++m_generation;
    if (!m_runtimeHost.ready() || !m_runtimeHost.runtime()) {
        const QString message = tr("The backend is unavailable.");
        m_statusSink.publishStatus(message, true);
        emit stateChanged();
        emit lifecycleFenced(m_lifecycleFence);
        emit initialized(m_generation, false);
        return false;
    }

    const YanamiStatusResult connection =
        m_runtimeHost.runtime()->embyConnected();
    bool succeeded = connection.value >= 0;
    if (!succeeded) {
        m_connected = false;
        m_statusSink.publishStatus(
            m_statusSink.userFacingBackendError(
                connection.errorCode, connection.error),
            true);
    } else {
        m_connected = connection.value == 1;
    }
    succeeded = refreshIdentity(m_connected) && succeeded;
    if (!m_connected)
        replaceCapabilities(m_generation, {});

    emit stateChanged();
    emit lifecycleFenced(m_lifecycleFence);
    emit initialized(m_generation, m_connected);
    return succeeded;
}

void SessionCoordinator::shutdown()
{
    if (!m_initialized && !m_acceptingRequests)
        return;
    m_acceptingRequests = false;
    m_initialized = false;
    ++m_lifecycleFence;
    emit lifecycleFenced(m_lifecycleFence);

    if (!m_operation.has_value())
        return;
    const SessionPort::Operation operation = *m_operation;
    const QString requestId = m_operationRequestId;
    m_operation.reset();
    m_operationRequestId.clear();
    m_pendingUserName.clear();
    m_pendingDisplayName.clear();
    m_pendingServerUrl.clear();
    m_pendingServerDomain.clear();
    m_busy = false;
    m_transitioning = false;
    emit stateChanged();
    emit operationFailed(
        requestId, operation, tr("The operation was canceled."));
}

void SessionCoordinator::drain()
{
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
}

void SessionCoordinator::login(
    const QString &requestId,
    const QString &serverName,
    const QString &serverUrl,
    const QString &userName,
    const QString &password,
    bool allowInsecureHttp)
{
    if (!m_acceptingRequests || !m_runtimeHost.ready()) {
        rejectImmediately(requestId, SessionPort::Operation::Login,
            tr("The backend is unavailable."));
        return;
    }
    if (m_busy) {
        rejectImmediately(requestId, SessionPort::Operation::Login,
            tr("Another operation is already in progress."));
        return;
    }
    const QString name = serverName.trimmed();
    const QString url = serverUrl.trimmed();
    const QString user = userName.trimmed();
    if (name.isEmpty() || url.isEmpty() || user.isEmpty()) {
        rejectImmediately(requestId, SessionPort::Operation::Login,
            tr("Server name, URL, and username are required."));
        return;
    }
    RustBridgeRuntime *runtime = m_runtimeHost.runtime();
    m_pendingDisplayName = name;
    m_pendingServerUrl = url;
    m_pendingServerDomain = QUrl(url).host();
    startOperation(SessionPort::Operation::Login, requestId, user,
        [runtime, name, url, user, password, allowInsecureHttp] {
            return runtime->loginEmby(
                name, url, user, password, allowInsecureHttp);
        });
}

void SessionCoordinator::logout(const QString &requestId)
{
    if (!m_acceptingRequests || !m_runtimeHost.ready()) {
        rejectImmediately(requestId, SessionPort::Operation::Logout,
            tr("The backend is unavailable."));
        return;
    }
    if (m_busy) {
        rejectImmediately(requestId, SessionPort::Operation::Logout,
            tr("Another operation is already in progress."));
        return;
    }
    if (!m_connected) {
        rejectImmediately(requestId, SessionPort::Operation::Logout,
            tr("No Emby session is connected."));
        return;
    }
    RustBridgeRuntime *runtime = m_runtimeHost.runtime();
    startOperation(SessionPort::Operation::Logout, requestId, {},
        [runtime] { return runtime->logoutEmby(); });
}

bool SessionCoordinator::replaceCapabilities(
    quint64 sessionGeneration,
    const SessionCapabilities &capabilities)
{
    if (sessionGeneration != m_generation)
        return false;
    if (m_capabilities == capabilities)
        return true;
    m_capabilities = capabilities;
    emit stateChanged();
    return true;
}

void SessionCoordinator::startOperation(
    SessionPort::Operation operation,
    const QString &requestId,
    const QString &pendingUserName,
    std::function<YanamiOperationResult()> work)
{
    m_operation = operation;
    m_operationRequestId = requestId;
    m_pendingUserName = pendingUserName;
    m_busy = true;
    m_transitioning = true;
    ++m_lifecycleFence;
    m_operationTimer.start();
    qInfo().noquote()
        << "session_operation"
        << "phase=start"
        << "operation=" << operationName(operation)
        << "lifecycleFence=" << m_lifecycleFence;
    emit transitionStarted(m_lifecycleFence, operation, requestId);
    emit stateChanged();
    m_watcher.setFuture(QtConcurrent::run(&m_controlPool, std::move(work)));
}

void SessionCoordinator::finishOperation()
{
    const YanamiOperationResult result = m_watcher.result();
    if (!m_operation.has_value())
        return;
    const SessionPort::Operation operation = *m_operation;
    const QString requestId = m_operationRequestId;
    const qint64 elapsedMs = m_operationTimer.isValid()
        ? m_operationTimer.elapsed() : -1;
    m_operationTimer.invalidate();
    qInfo().noquote()
        << "session_operation"
        << "phase=finish"
        << "operation=" << operationName(operation)
        << "elapsedMs=" << elapsedMs
        << "status=" << result.status;

    QString failureMessage;
    if (result.status != 0) {
        failureMessage = result.error.isEmpty()
            ? tr("The operation failed.")
            : m_statusSink.userFacingBackendError(
                result.errorCode, result.error);
        if (operation == SessionPort::Operation::Login)
            m_pendingUserName.clear();
        m_pendingDisplayName.clear();
        m_pendingServerUrl.clear();
        m_pendingServerDomain.clear();
        m_statusSink.publishStatus(failureMessage, true);
    } else {
        ++m_generation;
        if (operation == SessionPort::Operation::Login) {
            m_connected = true;
            m_userName = m_pendingUserName;
            m_displayName = m_pendingDisplayName;
            m_serverUrl = m_pendingServerUrl;
            m_serverDomain = m_pendingServerDomain;
            m_pendingUserName.clear();
            m_pendingDisplayName.clear();
            m_pendingServerUrl.clear();
            m_pendingServerDomain.clear();
            replaceCapabilities(m_generation, {});
            const bool identityReady = applyIdentityPayload(
                result.payload, true);
            if (identityReady)
                m_statusSink.publishStatus(tr("Connected to Emby."), false);
        } else {
            m_connected = false;
            m_userName.clear();
            replaceCapabilities(m_generation, {});
            const bool identityReady = refreshIdentity(false);
            if (identityReady) {
                m_statusSink.publishStatus(tr(
                    "Disconnected from Emby and removed the saved token."),
                    false);
            }
        }
    }

    if (failureMessage.isEmpty()) {
        // Commit while the operation is still busy. A direct stateChanged or
        // completion handler must not be able to start transition B before
        // the composition root has committed transition A.
        emit committed(m_generation, m_connected);
    }

    m_operation.reset();
    m_operationRequestId.clear();
    m_busy = false;
    m_transitioning = false;
    if (failureMessage.isEmpty()) {
        emit operationCompleted(requestId, operation);
    } else {
        emit operationFailed(requestId, operation, failureMessage);
    }
    emit stateChanged();
}

void SessionCoordinator::rejectImmediately(
    const QString &requestId,
    SessionPort::Operation operation,
    const QString &message)
{
    m_statusSink.publishStatus(message, true);
    emit operationFailed(requestId, operation, message);
}

bool SessionCoordinator::refreshIdentity(bool connectedSession)
{
    if (!m_runtimeHost.ready() || !m_runtimeHost.runtime())
        return false;
    const YanamiOperationResult result =
        m_runtimeHost.runtime()->embySettings();
    if (result.status != 0) {
        m_statusSink.publishStatus(
            m_statusSink.userFacingBackendError(
                result.errorCode, result.error),
            true);
        return false;
    }

    return applyIdentityPayload(result.payload, connectedSession);
}

bool SessionCoordinator::applyIdentityPayload(
    const QByteArray &payload,
    bool connectedSession)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        m_statusSink.publishStatus(
            tr("The saved Emby settings were invalid."), true);
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schemaVersion")).toInt()
            != desktopSchemaVersion
        || !object.value(QStringLiteral("displayName")).isString()
        || !object.value(QStringLiteral("serverUrl")).isString()
        || !object.value(QStringLiteral("userName")).isString()
        || !object.value(QStringLiteral("serverDomain")).isString()) {
        m_statusSink.publishStatus(
            tr("The saved Emby settings were invalid."), true);
        return false;
    }

    m_displayName = object.value(QStringLiteral("displayName")).toString();
    m_serverUrl = object.value(QStringLiteral("serverUrl")).toString();
    m_userName = connectedSession
        ? object.value(QStringLiteral("userName")).toString()
        : QString();
    m_serverDomain = object.value(QStringLiteral("serverDomain")).toString();
    return true;
}

QString SessionCoordinator::operationName(
    SessionPort::Operation operation) const
{
    return operation == SessionPort::Operation::Login
        ? QStringLiteral("login_emby")
        : QStringLiteral("logout_emby");
}
