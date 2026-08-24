#include "DanmakuCoordinator.hpp"

#include "BackendInfrastructure.hpp"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr int desktopSchemaVersion = 8;
constexpr qsizetype maximumPendingMutations = 32;

QString invalidResponseMessage()
{
    return DanmakuCoordinator::tr(
        "The danmaku service returned an invalid response.");
}

} // namespace

DanmakuCoordinator::DanmakuCoordinator(
    RustBridgeRuntime &runtime,
    QThreadPool &queryPool,
    QThreadPool &mutationPool,
    SessionStateProvider sessionStateProvider,
    StatusSink &statusSink,
    QObject *parent)
    : DanmakuCoordinator(
        BackendOperations {
            [&runtime] { return runtime.ready(); },
            [&runtime] {
                return runtime.dandanplayCredentialSource();
            },
            [&runtime](const QString &appId,
                const QString &appSecret) {
                return runtime.configureDandanplay(appId, appSecret);
            },
            [&runtime] { return runtime.clearDandanplay(); },
            [&runtime](Operation operation,
                const QString &itemId,
                const QVariantMap &payload) {
                return runtime.danmaku(operation, itemId, payload);
            },
        },
        queryPool,
        mutationPool,
        std::move(sessionStateProvider),
        statusSink,
        parent)
{
}

DanmakuCoordinator::DanmakuCoordinator(
    BackendOperations backend,
    QThreadPool &queryPool,
    QThreadPool &mutationPool,
    SessionStateProvider sessionStateProvider,
    StatusSink &statusSink,
    QObject *parent)
    : DanmakuPort(parent)
    , m_backend(std::move(backend))
    , m_queryPool(queryPool)
    , m_mutationPool(mutationPool)
    , m_sessionStateProvider(std::move(sessionStateProvider))
    , m_statusSink(statusSink)
{
    connect(&m_configurationWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this, &DanmakuCoordinator::finishConfiguration);
    connect(&m_searchWatcher,
        &QFutureWatcher<OperationWorkResult>::finished,
        this, &DanmakuCoordinator::finishSearch);
    connect(&m_mutationWatcher,
        &QFutureWatcher<OperationWorkResult>::finished,
        this, &DanmakuCoordinator::finishMutation);
}

DanmakuCoordinator::~DanmakuCoordinator()
{
    shutdown();
    drain();
}

bool DanmakuCoordinator::configurationBusy() const
{
    return m_activeConfiguration.has_value()
        && !m_activeConfiguration->terminalDelivered;
}

bool DanmakuCoordinator::initializeCredentialStatus()
{
    if (m_shuttingDown)
        return false;
    m_initialized = true;
    m_acceptingRequests = backendReady();
    if (!m_acceptingRequests) {
        m_statusSink.publishStatus(tr("The backend is unavailable."), true);
        return false;
    }
    return refreshCredentialState(true);
}

void DanmakuCoordinator::search(
    const QString &requestId,
    const QString &itemId,
    const QString &anime)
{
    if (anime.trimmed().isEmpty()) {
        rejectOperation(requestId, itemId, Operation::Search,
            tr("Enter an anime title to search."), false, true);
        return;
    }
    OperationRequest request;
    request.clientRequestId = requestId;
    request.itemId = itemId;
    request.operation = Operation::Search;
    request.payload = {
        {QStringLiteral("anime"), anime},
    };
    submitOperation(std::move(request));
}

void DanmakuCoordinator::loadAutomatically(
    const QString &requestId,
    const QString &itemId)
{
    OperationRequest request;
    request.clientRequestId = requestId;
    request.itemId = itemId;
    request.operation = Operation::AutomaticLoad;
    submitOperation(std::move(request));
}

void DanmakuCoordinator::applyMatch(
    const QString &requestId,
    const QString &itemId,
    const QVariantMap &match,
    const QVariantMap &style)
{
    QVariantMap payload = style;
    payload.insert(QStringLiteral("episodeId"),
        match.value(QStringLiteral("episodeId")));
    payload.insert(QStringLiteral("animeTitle"),
        match.value(QStringLiteral("animeTitle"), QString()));
    payload.insert(QStringLiteral("episodeTitle"),
        match.value(QStringLiteral("episodeTitle"), QString()));

    OperationRequest request;
    request.clientRequestId = requestId;
    request.itemId = itemId;
    request.operation = Operation::ApplyMatch;
    request.payload = std::move(payload);
    submitOperation(std::move(request));
}

void DanmakuCoordinator::configure(
    const QString &requestId,
    const QString &appId,
    const QString &appSecret)
{
    if (!m_acceptingRequests || !backendReady()
        || !m_backend.configure) {
        rejectConfiguration(requestId, ConfigurationOperation::Configure,
            tr("The backend is unavailable."));
        return;
    }
    if (configurationBusy()) {
        rejectConfiguration(requestId, ConfigurationOperation::Configure,
            tr("Another operation is already in progress."));
        return;
    }
    const QString normalizedAppId = appId.trimmed();
    if (normalizedAppId.isEmpty() || appSecret.isEmpty()) {
        rejectConfiguration(requestId, ConfigurationOperation::Configure,
            tr("AppId and AppSecret are required."));
        return;
    }

    ConfigurationRequest request;
    request.clientRequestId = requestId;
    request.operation = ConfigurationOperation::Configure;
    request.identity = ++m_nextRequestIdentity;
    const auto configureBackend = m_backend.configure;
    startConfiguration(std::move(request),
        [configureBackend, normalizedAppId, appSecret] {
            return configureBackend(
                normalizedAppId, appSecret);
        });
}

void DanmakuCoordinator::clearConfiguration(const QString &requestId)
{
    if (!m_acceptingRequests || !backendReady()
        || !m_backend.clearConfiguration) {
        rejectConfiguration(requestId, ConfigurationOperation::Clear,
            tr("The backend is unavailable."));
        return;
    }
    if (configurationBusy()) {
        rejectConfiguration(requestId, ConfigurationOperation::Clear,
            tr("Another operation is already in progress."));
        return;
    }

    ConfigurationRequest request;
    request.clientRequestId = requestId;
    request.operation = ConfigurationOperation::Clear;
    request.identity = ++m_nextRequestIdentity;
    const auto clearBackend = m_backend.clearConfiguration;
    startConfiguration(std::move(request),
        [clearBackend] { return clearBackend(); });
}

DanmakuCoordinator::SessionState DanmakuCoordinator::sessionState() const
{
    return m_sessionStateProvider
        ? m_sessionStateProvider() : SessionState {};
}

bool DanmakuCoordinator::backendReady() const
{
    return m_backend.ready && m_backend.ready();
}

bool DanmakuCoordinator::operationAvailable(
    const SessionState &session) const
{
    return m_initialized
        && m_acceptingRequests
        && !m_shuttingDown
        && !m_sessionFenced
        && backendReady()
        && m_backend.danmaku
        && session.connected;
}

bool DanmakuCoordinator::refreshCredentialState(bool publishFailure)
{
    if (!backendReady() || !m_backend.credentialSource) {
        if (publishFailure) {
            m_statusSink.publishStatus(
                tr("The backend is unavailable."), true);
        }
        return false;
    }
    const YanamiStatusResult status =
        m_backend.credentialSource();
    if (status.value < 0) {
        if (publishFailure) {
            m_statusSink.publishStatus(
                m_statusSink.userFacingDanmakuError(
                    status.errorCode, status.error),
                true);
        }
        return false;
    }

    const bool configured = status.value > 0;
    if (m_configured == configured
        && m_credentialSource == status.value) {
        return true;
    }
    m_configured = configured;
    m_credentialSource = status.value;
    emit stateChanged();
    return true;
}

void DanmakuCoordinator::startConfiguration(
    ConfigurationRequest request,
    std::function<YanamiOperationResult()> work)
{
    m_activeConfiguration = std::move(request);
    m_configurationTimer.start();
    qInfo().noquote()
        << "danmaku_configuration"
        << "phase=start"
        << "requestIdentity=" << m_activeConfiguration->identity
        << "operation="
        << (m_activeConfiguration->operation
                    == ConfigurationOperation::Configure
                ? "configure" : "clear");
    emit stateChanged();
    m_configurationWatcher.setFuture(
        QtConcurrent::run(&m_mutationPool, std::move(work)));
}

void DanmakuCoordinator::finishConfiguration()
{
    const YanamiOperationResult result = m_configurationWatcher.result();
    const qint64 elapsedMs = m_configurationTimer.isValid()
        ? m_configurationTimer.elapsed() : -1;
    m_configurationTimer.invalidate();
    if (!m_activeConfiguration.has_value())
        return;

    ConfigurationRequest request =
        std::move(*m_activeConfiguration);
    m_activeConfiguration.reset();
    qInfo().noquote()
        << "danmaku_configuration"
        << "phase=finish"
        << "requestIdentity=" << request.identity
        << "elapsedMs=" << elapsedMs
        << "status=" << result.status
        << "consumerSettled=" << request.terminalDelivered;
    emit stateChanged();
    if (request.terminalDelivered)
        return;

    if (result.status != 0) {
        const QString message = backendFailure(result);
        m_statusSink.publishStatus(message, true);
        failConfiguration(request, message);
        return;
    }

    if (request.operation == ConfigurationOperation::Configure) {
        m_statusSink.publishStatus(
            tr("DanDanPlay credentials validated and saved securely."),
            false);
    } else {
        m_statusSink.publishStatus(
            tr("DanDanPlay credentials removed."), false);
    }
    // Keep a successful mutation terminal even when the following status read
    // fails; the read failure remains visible in the shared status service.
    refreshCredentialState(true);
    request.terminalDelivered = true;
    emit configurationCompleted(
        request.clientRequestId, request.operation);
}

void DanmakuCoordinator::rejectConfiguration(
    const QString &requestId,
    ConfigurationOperation operation,
    const QString &message)
{
    m_statusSink.publishStatus(message, true);
    emit configurationFailed(requestId, operation, message);
}

void DanmakuCoordinator::failConfiguration(
    ConfigurationRequest &request,
    const QString &message)
{
    if (request.terminalDelivered)
        return;
    request.terminalDelivered = true;
    emit configurationFailed(
        request.clientRequestId, request.operation, message);
}

void DanmakuCoordinator::submitOperation(OperationRequest request)
{
    if (request.itemId.trimmed().isEmpty()) {
        rejectOperation(request.clientRequestId, request.itemId,
            request.operation,
            tr("A media item is required for this action."), false);
        return;
    }
    const SessionState session = sessionState();
    if (!operationAvailable(session)) {
        const QString message = m_sessionFenced
            ? tr("The action was canceled because the Emby session changed.")
            : (!backendReady() || !m_acceptingRequests
                    ? tr("The backend is unavailable.")
                    : tr("This action requires an active Emby connection."));
        rejectOperation(request.clientRequestId, request.itemId,
            request.operation, message, false);
        return;
    }

    request.identity = ++m_nextRequestIdentity;
    request.sessionGeneration = session.generation;
    request.enqueuedAtMs = QDateTime::currentMSecsSinceEpoch();
    request.decodeAllowed = std::make_shared<std::atomic_bool>(true);
    if (request.operation == Operation::Search)
        submitSearch(std::move(request));
    else
        submitMutation(std::move(request));
}

void DanmakuCoordinator::submitSearch(OperationRequest request)
{
    m_latestSearchIdentity = request.identity;
    std::optional<OperationRequest> supersededActive =
        std::move(m_activeSearch);
    std::optional<OperationRequest> supersededQueued =
        std::move(m_queuedSearch);
    const bool laneWasActive = supersededActive.has_value();
    m_activeSearch.reset();
    m_queuedSearch.reset();

    if (laneWasActive
        || m_searchWatcher.isRunning()
        || mutationPendingFor(request.itemId)) {
        qInfo().noquote()
            << "danmaku_search"
            << "phase=queued"
            << "requestIdentity=" << request.identity
            << "item=" << request.itemId;
        m_queuedSearch = std::move(request);
    } else {
        startSearch(std::move(request));
    }
    if (supersededActive.has_value()
        && !supersededActive->terminalDelivered) {
        failOperation(*supersededActive,
            tr("The request was superseded."), true);
    }
    if (supersededQueued.has_value()) {
        failOperation(*supersededQueued,
            tr("The request was superseded."), true);
    }
}

void DanmakuCoordinator::startSearch(OperationRequest request)
{
    const qint64 schedulerWaitMs = request.enqueuedAtMs > 0
        ? QDateTime::currentMSecsSinceEpoch() - request.enqueuedAtMs : 0;
    const quint64 identity = request.identity;
    const QString itemId = request.itemId;
    QVariantMap payload = std::move(request.payload);
    const auto decodeAllowed = request.decodeAllowed;
    const auto runDanmaku = m_backend.danmaku;
    m_activeSearch = std::move(request);
    m_searchTimer.start();
    qInfo().noquote()
        << "danmaku_search"
        << "phase=start"
        << "requestIdentity=" << identity
        << "item=" << itemId
        << "schedulerWaitMs=" << schedulerWaitMs;
    m_searchWatcher.setFuture(QtConcurrent::run(
        &m_queryPool,
        [runDanmaku, itemId, payload = std::move(payload),
            decodeAllowed] {
            return prepareResult(
                Operation::Search,
                runDanmaku(Operation::Search, itemId, payload),
                decodeAllowed);
        }));
}

void DanmakuCoordinator::finishSearch()
{
    const OperationWorkResult result = m_searchWatcher.result();
    const qint64 elapsedMs = m_searchTimer.isValid()
        ? m_searchTimer.elapsed() : -1;
    m_searchTimer.invalidate();
    if (!m_activeSearch.has_value()) {
        QTimer::singleShot(0, this, &DanmakuCoordinator::pumpSearch);
        return;
    }

    OperationRequest request = std::move(*m_activeSearch);
    m_activeSearch.reset();
    qInfo().noquote()
        << "danmaku_search"
        << "phase=finish"
        << "requestIdentity=" << request.identity
        << "item=" << request.itemId
        << "elapsedMs=" << elapsedMs
        << "status=" << result.status
        << "consumerSettled=" << request.terminalDelivered;
    if (!request.terminalDelivered) {
        if (request.identity != m_latestSearchIdentity
            || !accepts(request)) {
            failOperation(request,
                tr("The request was superseded."), true);
        } else {
            applyResult(request, result, elapsedMs);
        }
    }
    QTimer::singleShot(0, this, &DanmakuCoordinator::pumpSearch);
}

void DanmakuCoordinator::pumpSearch()
{
    if (m_shuttingDown
        || m_activeSearch.has_value()
        || m_searchWatcher.isRunning()
        || !m_queuedSearch.has_value()) {
        return;
    }
    if (mutationPendingFor(m_queuedSearch->itemId))
        return;

    OperationRequest request = std::move(*m_queuedSearch);
    m_queuedSearch.reset();
    if (request.identity != m_latestSearchIdentity
        || !accepts(request)) {
        failOperation(request,
            tr("The request was superseded."), true);
        return;
    }
    startSearch(std::move(request));
}

void DanmakuCoordinator::submitMutation(OperationRequest request)
{
    const qsizetype pending = m_mutationQueue.size()
        + (m_activeMutation.has_value() ? 1 : 0);
    if (pending >= maximumPendingMutations) {
        rejectOperation(request.clientRequestId, request.itemId,
            request.operation,
            tr("Too many actions are waiting. Please try again."), false);
        return;
    }

    fenceSearchForMutation(request.itemId);
    qInfo().noquote()
        << "danmaku_mutation"
        << "phase=queued"
        << "requestIdentity=" << request.identity
        << "operation=" << operationName(request.operation)
        << "item=" << request.itemId;
    m_mutationQueue.enqueue(std::move(request));
    pumpMutation();
}

void DanmakuCoordinator::startMutation(OperationRequest request)
{
    const qint64 schedulerWaitMs = request.enqueuedAtMs > 0
        ? QDateTime::currentMSecsSinceEpoch() - request.enqueuedAtMs : 0;
    const quint64 identity = request.identity;
    const QString itemId = request.itemId;
    const Operation operation = request.operation;
    QVariantMap payload = std::move(request.payload);
    const auto decodeAllowed = request.decodeAllowed;
    const auto runDanmaku = m_backend.danmaku;
    m_activeMutation = std::move(request);
    m_mutationTimer.start();
    qInfo().noquote()
        << "danmaku_mutation"
        << "phase=start"
        << "requestIdentity=" << identity
        << "operation=" << operationName(operation)
        << "item=" << itemId
        << "schedulerWaitMs=" << schedulerWaitMs;
    m_mutationWatcher.setFuture(QtConcurrent::run(
        &m_mutationPool,
        [runDanmaku, operation, itemId,
            payload = std::move(payload), decodeAllowed] {
            return prepareResult(
                operation,
                runDanmaku(operation, itemId, payload),
                decodeAllowed);
        }));
}

void DanmakuCoordinator::finishMutation()
{
    const OperationWorkResult result = m_mutationWatcher.result();
    const qint64 elapsedMs = m_mutationTimer.isValid()
        ? m_mutationTimer.elapsed() : -1;
    m_mutationTimer.invalidate();
    if (!m_activeMutation.has_value()) {
        QTimer::singleShot(0, this, &DanmakuCoordinator::pumpMutation);
        return;
    }

    OperationRequest request = std::move(*m_activeMutation);
    m_activeMutation.reset();
    qInfo().noquote()
        << "danmaku_mutation"
        << "phase=finish"
        << "requestIdentity=" << request.identity
        << "operation=" << operationName(request.operation)
        << "item=" << request.itemId
        << "elapsedMs=" << elapsedMs
        << "status=" << result.status
        << "consumerSettled=" << request.terminalDelivered;
    if (!request.terminalDelivered) {
        if (!accepts(request)) {
            failOperation(request,
                tr("The action was canceled because the Emby session changed."),
                false);
        } else {
            applyResult(request, result, elapsedMs);
        }
    }
    QTimer::singleShot(0, this, &DanmakuCoordinator::pumpMutation);
    QTimer::singleShot(0, this, &DanmakuCoordinator::pumpSearch);
}

void DanmakuCoordinator::pumpMutation()
{
    if (m_shuttingDown
        || m_activeMutation.has_value()
        || m_mutationWatcher.isRunning()) {
        return;
    }
    while (!m_mutationQueue.isEmpty()) {
        OperationRequest request = m_mutationQueue.dequeue();
        if (!accepts(request)) {
            failOperation(request,
                tr("The action was canceled because the Emby session changed."),
                false);
            continue;
        }
        startMutation(std::move(request));
        return;
    }
    pumpSearch();
}

bool DanmakuCoordinator::mutationPendingFor(
    const QString &itemId) const
{
    if (m_activeMutation.has_value()
        && m_activeMutation->itemId == itemId)
        return true;
    for (const OperationRequest &request : m_mutationQueue) {
        if (request.itemId == itemId)
            return true;
    }
    return false;
}

void DanmakuCoordinator::fenceSearchForMutation(
    const QString &itemId)
{
    if (m_activeSearch.has_value()
        && m_activeSearch->itemId == itemId
        && !m_activeSearch->terminalDelivered) {
        failOperation(*m_activeSearch,
            tr("The request was superseded."), true);
    }
    if (m_queuedSearch.has_value()
        && m_queuedSearch->itemId == itemId) {
        failOperation(*m_queuedSearch,
            tr("The request was superseded."), true);
        m_queuedSearch.reset();
    }
}

bool DanmakuCoordinator::accepts(
    const OperationRequest &request) const
{
    const SessionState current = sessionState();
    return operationAvailable(current)
        && request.sessionGeneration == current.generation;
}

DanmakuCoordinator::OperationWorkResult
DanmakuCoordinator::prepareResult(
    Operation operation,
    YanamiOperationResult operationResult,
    const std::shared_ptr<std::atomic_bool> &decodeAllowed)
{
    OperationWorkResult prepared;
    prepared.status = operationResult.status;
    prepared.errorCode = std::move(operationResult.errorCode);
    prepared.error = std::move(operationResult.error);
    if (prepared.status != 0)
        return prepared;
    if (decodeAllowed
        && !decodeAllowed->load(std::memory_order_acquire)) {
        return prepared;
    }

    QElapsedTimer decodeTimer;
    decodeTimer.start();
    prepared.responseValid = decodeResult(
        operation, operationResult.payload, decodeAllowed,
        &prepared.result);
    prepared.decodeElapsedNs = decodeTimer.nsecsElapsed();
    return prepared;
}

bool DanmakuCoordinator::decodeResult(
    Operation operation,
    const QByteArray &payload,
    const std::shared_ptr<std::atomic_bool> &decodeAllowed,
    QVariantMap *result)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return false;
    }
    if (decodeAllowed
        && !decodeAllowed->load(std::memory_order_acquire)) {
        return false;
    }
    const QJsonObject object = document.object();
    const QJsonValue schema = object.value(QStringLiteral("schemaVersion"));
    const QJsonValue statusValue = object.value(QStringLiteral("status"));
    if (!schema.isDouble()
        || schema.toInt() != desktopSchemaVersion
        || !statusValue.isString()) {
        return false;
    }

    const QString status = statusValue.toString();
    if (operation == Operation::Search) {
        if (status != QStringLiteral("search-results")
            || !object.value(QStringLiteral("animes")).isArray()) {
            return false;
        }
    } else if (status == QStringLiteral("loaded")) {
        if (!object.value(QStringLiteral("comments")).isArray()
            || !object.value(QStringLiteral("title")).isString()
            || !object.value(QStringLiteral("commentCount")).isDouble()
            || !object.value(QStringLiteral("stale")).isBool()
            || !object.value(
                    QStringLiteral("matchedTimeOffset")).isDouble()) {
            return false;
        }
    } else if (status == QStringLiteral("no-match")
               || status == QStringLiteral("choice-required")) {
        const QJsonValue episodeSuggestion =
            object.value(QStringLiteral("episodeSuggestion"));
        if (!object.value(QStringLiteral("animeSuggestion")).isString()
            || (!episodeSuggestion.isNull()
                && !episodeSuggestion.isString())
            || !object.value(QStringLiteral("matches")).isArray()) {
            return false;
        }
    } else {
        return false;
    }

    if (decodeAllowed
        && !decodeAllowed->load(std::memory_order_acquire)) {
        return false;
    }

    *result = object.toVariantMap();
    result->remove(QStringLiteral("schemaVersion"));
    return true;
}

QString DanmakuCoordinator::backendFailure(
    const YanamiOperationResult &result) const
{
    if (!result.errorCode.isEmpty() || !result.error.isEmpty()) {
        return m_statusSink.userFacingDanmakuError(
            result.errorCode, result.error);
    }
    return tr("The operation failed.");
}

QString DanmakuCoordinator::backendFailure(
    const OperationWorkResult &result) const
{
    if (!result.errorCode.isEmpty() || !result.error.isEmpty()) {
        return m_statusSink.userFacingDanmakuError(
            result.errorCode, result.error);
    }
    return tr("The operation failed.");
}

void DanmakuCoordinator::applyResult(
    OperationRequest &request,
    const OperationWorkResult &operationResult,
    qint64 elapsedMs)
{
    if (operationResult.status != 0) {
        const QString message = backendFailure(operationResult);
        qWarning().noquote()
            << "danmaku_operation"
            << "phase=failed"
            << "requestIdentity=" << request.identity
            << "operation=" << operationName(request.operation)
            << "item=" << request.itemId
            << "elapsedMs=" << elapsedMs
            << "status=" << operationResult.status
            << "errorCode=" << operationResult.errorCode;
        const bool automaticLoad =
            request.operation == Operation::AutomaticLoad;
        failOperation(request, message, automaticLoad, !automaticLoad);
        return;
    }

    if (!operationResult.responseValid) {
        qWarning().noquote()
            << "danmaku_operation"
            << "phase=failed"
            << "requestIdentity=" << request.identity
            << "operation=" << operationName(request.operation)
            << "item=" << request.itemId
            << "reason=invalid_response";
        const bool automaticLoad =
            request.operation == Operation::AutomaticLoad;
        failOperation(request, invalidResponseMessage(),
            automaticLoad, !automaticLoad);
        return;
    }

    qInfo().noquote()
        << "danmaku_operation"
        << "phase=completed"
        << "requestIdentity=" << request.identity
        << "operation=" << operationName(request.operation)
        << "item=" << request.itemId
        << "status="
        << operationResult.result.value(
               QStringLiteral("status")).toString()
        << "decodeMs="
        << (static_cast<double>(operationResult.decodeElapsedNs)
            / 1'000'000.0)
        << "comments="
        << operationResult.result.value(
               QStringLiteral("commentCount")).toInt()
        << "candidates="
        << operationResult.result.value(
               QStringLiteral("matches")).toList().size();
    completeOperation(request, operationResult.result);
}

void DanmakuCoordinator::completeOperation(
    OperationRequest &request,
    const QVariantMap &result)
{
    if (request.terminalDelivered)
        return;
    request.terminalDelivered = true;
    emit operationCompleted(request.clientRequestId,
        request.itemId, request.operation, result);
}

void DanmakuCoordinator::failOperation(
    OperationRequest &request,
    const QString &message,
    bool nonModal,
    bool publishStatus)
{
    if (request.terminalDelivered)
        return;
    if (request.decodeAllowed) {
        request.decodeAllowed->store(
            false, std::memory_order_release);
    }
    request.terminalDelivered = true;
    if (publishStatus)
        m_statusSink.publishStatus(message, !nonModal);
    emit operationFailed(request.clientRequestId,
        request.itemId, request.operation, message, nonModal);
}

void DanmakuCoordinator::rejectOperation(
    const QString &requestId,
    const QString &itemId,
    Operation operation,
    const QString &message,
    bool nonModal,
    bool publishStatus)
{
    if (publishStatus)
        m_statusSink.publishStatus(message, !nonModal);
    emit operationFailed(
        requestId, itemId, operation, message, nonModal);
}

const char *DanmakuCoordinator::operationName(Operation operation)
{
    switch (operation) {
    case Operation::Search: return "search";
    case Operation::AutomaticLoad: return "automatic_load";
    case Operation::ApplyMatch: return "apply_match";
    }
    return "unknown";
}

void DanmakuCoordinator::sessionTransitionStarted(const char *reason)
{
    if (m_shuttingDown || m_sessionFenced)
        return;
    m_sessionFenced = true;
    qInfo().noquote()
        << "danmaku_session_fence"
        << "phase=started"
        << "reason=" << reason;

    if (m_activeSearch.has_value()) {
        failOperation(*m_activeSearch,
            tr("The request was superseded."), true);
    }
    if (m_queuedSearch.has_value()) {
        failOperation(*m_queuedSearch,
            tr("The request was superseded."), true);
        m_queuedSearch.reset();
    }

    const QString mutationMessage =
        tr("The action was canceled because the Emby session changed.");
    if (m_activeMutation.has_value()) {
        failOperation(
            *m_activeMutation, mutationMessage, false);
    }
    while (!m_mutationQueue.isEmpty()) {
        OperationRequest request = m_mutationQueue.dequeue();
        failOperation(request, mutationMessage, false);
    }
}

void DanmakuCoordinator::sessionTransitionCommitted()
{
    if (m_shuttingDown)
        return;
    m_sessionFenced = false;
    m_acceptingRequests = m_initialized && backendReady();
    const SessionState session = sessionState();
    qInfo().noquote()
        << "danmaku_session_fence"
        << "phase=committed"
        << "sessionGeneration=" << session.generation
        << "connected=" << session.connected;
    pumpMutation();
    pumpSearch();
}

void DanmakuCoordinator::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_acceptingRequests = false;
    m_sessionFenced = true;

    const QString canceled = tr("The operation was canceled.");
    bool configurationStateChanged = false;
    if (m_activeConfiguration.has_value()
        && !m_activeConfiguration->terminalDelivered) {
        failConfiguration(*m_activeConfiguration, canceled);
        configurationStateChanged = true;
    }
    if (configurationStateChanged)
        emit stateChanged();

    if (m_activeSearch.has_value())
        failOperation(*m_activeSearch, canceled, true);
    if (m_queuedSearch.has_value()) {
        failOperation(*m_queuedSearch, canceled, true);
        m_queuedSearch.reset();
    }
    if (m_activeMutation.has_value())
        failOperation(*m_activeMutation, canceled, true);
    while (!m_mutationQueue.isEmpty()) {
        OperationRequest request = m_mutationQueue.dequeue();
        failOperation(request, canceled, true);
    }
}

void DanmakuCoordinator::drain()
{
    if (m_configurationWatcher.isRunning())
        m_configurationWatcher.waitForFinished();
    if (m_searchWatcher.isRunning())
        m_searchWatcher.waitForFinished();
    if (m_mutationWatcher.isRunning())
        m_mutationWatcher.waitForFinished();
}
