#include "ApplicationViewModel.hpp"

#include "MediaStore.hpp"

#include <QSettings>
#include <QTimer>

#include <algorithm>

namespace {

constexpr int kDefaultLibrarySortMode = 1;

bool isLibrarySortModeValid(int sortMode)
{
    return sortMode >= 0 && sortMode <= 4;
}

void settleCatalogDisposition(
    AsyncResourceState *state,
    CatalogPort::RequestDisposition disposition,
    const QVariant &currentData,
    const QString &rejectionMessage)
{
    if (!state || disposition == CatalogPort::RequestDisposition::Accepted)
        return;
    const quint64 requestId = state->requestId();
    const QString resourceKey = state->resourceKey();
    if (disposition == CatalogPort::RequestDisposition::AlreadyCurrent) {
        state->resolve(requestId, resourceKey, 0, 0, currentData);
    } else {
        state->reject(requestId, resourceKey, 0, 0, rejectionMessage);
    }
}

} // namespace

SessionViewModel::SessionViewModel(SessionPort *port, QObject *parent)
    : QObject(parent), m_port(port)
{
    m_operation = new AsyncOperationState(this);
    if (port) {
        connect(port, &SessionPort::stateChanged,
            this, &SessionViewModel::stateChanged);
        connect(port, &SessionPort::operationCompleted,
            this, &SessionViewModel::handleOperationCompleted);
        connect(port, &SessionPort::operationFailed,
            this, &SessionViewModel::handleOperationFailed);
    }
}

bool SessionViewModel::connected() const { return m_port && m_port->connected(); }
quint64 SessionViewModel::generation() const { return m_port ? m_port->generation() : 0; }
bool SessionViewModel::busy() const { return m_port && m_port->busy(); }
QString SessionViewModel::displayName() const { return m_port ? m_port->displayName() : QString(); }
QString SessionViewModel::serverUrl() const { return m_port ? m_port->serverUrl() : QString(); }
QString SessionViewModel::userName() const { return m_port ? m_port->userName() : QString(); }
QString SessionViewModel::serverDomain() const { return m_port ? m_port->serverDomain() : QString(); }
bool SessionViewModel::administrator() const { return m_port && m_port->administrator(); }
bool SessionViewModel::canDownload() const { return m_port && m_port->canDownload(); }
bool SessionViewModel::canDelete() const { return m_port && m_port->canDelete(); }

void SessionViewModel::login(const QString &serverName, const QString &serverUrl,
    const QString &userName, const QString &password, bool allowInsecureHttp)
{
    if (m_operation->busy())
        return;
    const QString mutationId = QStringLiteral("session.login.%1")
        .arg(++m_operationSequence);
    m_pendingOperation = SessionPort::Operation::Login;
    m_hasPendingOperation = true;
    m_operation->begin(mutationId);
    if (m_port) {
        m_port->login(mutationId, serverName, serverUrl, userName, password,
            allowInsecureHttp);
    } else {
        m_hasPendingOperation = false;
        m_operation->reject(mutationId, tr("Backend is unavailable."));
    }
}

void SessionViewModel::logout()
{
    if (m_operation->busy())
        return;
    const QString mutationId = QStringLiteral("session.logout.%1")
        .arg(++m_operationSequence);
    m_pendingOperation = SessionPort::Operation::Logout;
    m_hasPendingOperation = true;
    m_operation->begin(mutationId);
    if (m_port) {
        m_port->logout(mutationId);
    } else {
        m_hasPendingOperation = false;
        m_operation->reject(mutationId, tr("Backend is unavailable."));
    }
}

void SessionViewModel::handleOperationCompleted(
    const QString &requestId,
    SessionPort::Operation operation)
{
    if (!m_hasPendingOperation || operation != m_pendingOperation
        || requestId != m_operation->mutationId()) {
        return;
    }
    m_hasPendingOperation = false;
    m_operation->resolve(requestId, connected());
}

void SessionViewModel::handleOperationFailed(
    const QString &requestId,
    SessionPort::Operation operation,
    const QString &message)
{
    if (!m_hasPendingOperation || operation != m_pendingOperation
        || requestId != m_operation->mutationId()) {
        return;
    }
    m_hasPendingOperation = false;
    m_operation->reject(requestId, message);
}

HomeViewModel::HomeViewModel(CatalogPort *port, QObject *parent)
    : QObject(parent), m_port(port)
{
    m_libraryState = new AsyncResourceState(this);
    m_activityState = new AsyncResourceState(this);
    m_collectionState = new AsyncResourceState(this);
    if (port) {
        connect(port, &CatalogPort::stateChanged, this,
            [this]() {
                settleResources();
                emit stateChanged();
            });
    }
}

MediaStore *HomeViewModel::mediaStore() const { return m_port ? m_port->mediaStore() : nullptr; }
bool HomeViewModel::libraryRefreshing() const { return m_port && m_port->libraryRefreshing(); }
bool HomeViewModel::activityRefreshing() const { return m_port && m_port->activityRefreshing(); }
bool HomeViewModel::collectionLoading() const { return m_port && m_port->collectionLoading(); }
bool HomeViewModel::collectionFetching() const { return m_port && m_port->collectionFetching(); }
bool HomeViewModel::libraryLoadFailed() const { return m_port && m_port->libraryLoadFailed(); }
QString HomeViewModel::collectionDisplayedId() const { return m_port ? m_port->collectionDisplayedId() : QString(); }
QString HomeViewModel::collectionTargetId() const { return m_port ? m_port->collectionTargetId() : QString(); }
QString HomeViewModel::collectionErrorId() const { return m_port ? m_port->collectionErrorId() : QString(); }
QVariantMap HomeViewModel::collectionParent() const { return m_port ? m_port->collectionParent() : QVariantMap(); }

void HomeViewModel::loadLibrary()
{
    m_libraryState->begin(QStringLiteral("library"), 0, 0, true);
    const CatalogPort::RequestDisposition disposition = m_port
        ? m_port->loadLibrary() : CatalogPort::RequestDisposition::Rejected;
    settleCatalogDisposition(m_libraryState, disposition,
        QVariant::fromValue(static_cast<QObject *>(mediaStore())),
        tr("The library is unavailable without an active server session."));
}

void HomeViewModel::refreshActivity()
{
    m_activityState->begin(QStringLiteral("activity"), 0, 0, true);
    const CatalogPort::RequestDisposition disposition = m_port
        ? m_port->refreshActivity() : CatalogPort::RequestDisposition::Rejected;
    settleCatalogDisposition(m_activityState, disposition, true,
        tr("Activity is unavailable without an active server session."));
}

void HomeViewModel::loadCollection(const QString &parentId)
{
    m_collectionState->begin(parentId, 0, 0, true);
    const CatalogPort::RequestDisposition disposition = m_port
        ? m_port->loadCollection(parentId)
        : CatalogPort::RequestDisposition::Rejected;
    settleCatalogDisposition(m_collectionState, disposition,
        collectionParent(),
        tr("The collection is unavailable without an active server session."));
}

void HomeViewModel::refreshCollection(const QString &parentId)
{
    m_collectionState->begin(parentId, 0, 0, true);
    const CatalogPort::RequestDisposition disposition = m_port
        ? m_port->refreshCollection(parentId)
        : CatalogPort::RequestDisposition::Rejected;
    settleCatalogDisposition(m_collectionState, disposition,
        collectionParent(),
        tr("The collection is unavailable without an active server session."));
}

void HomeViewModel::settleResources()
{
    if (!m_port)
        return;
    if ((m_libraryState->phase() == AsyncResourceState::Phase::Loading
            || m_libraryState->phase() == AsyncResourceState::Phase::Refreshing)
        && !m_port->libraryRefreshing()) {
        if (m_port->libraryLoadFailed()) {
            m_libraryState->reject(m_libraryState->requestId(),
                m_libraryState->resourceKey(), 0, 0,
                tr("The library could not be loaded."));
        } else {
            m_libraryState->resolve(m_libraryState->requestId(),
                m_libraryState->resourceKey(), 0, 0,
                QVariant::fromValue(static_cast<QObject *>(m_port->mediaStore())));
        }
    }
    if ((m_activityState->phase() == AsyncResourceState::Phase::Loading
            || m_activityState->phase() == AsyncResourceState::Phase::Refreshing)
        && !m_port->activityRefreshing()) {
        m_activityState->resolve(m_activityState->requestId(),
            m_activityState->resourceKey(), 0, 0, true);
    }
    if ((m_collectionState->phase() == AsyncResourceState::Phase::Loading
            || m_collectionState->phase() == AsyncResourceState::Phase::Refreshing)
        && !m_port->collectionLoading()) {
        if (m_port->collectionErrorId() == m_collectionState->resourceKey()) {
            m_collectionState->reject(m_collectionState->requestId(),
                m_collectionState->resourceKey(), 0, 0,
                tr("The collection could not be loaded."));
        } else {
            m_collectionState->resolve(m_collectionState->requestId(),
                m_collectionState->resourceKey(), 0, 0,
                m_port->collectionParent());
        }
    }
}

FavoritesViewModel::FavoritesViewModel(CatalogPort *port, QObject *parent)
    : QObject(parent), m_port(port)
{
    m_resourceState = new AsyncResourceState(this);
    if (port) {
        connect(port, &CatalogPort::stateChanged, this,
            [this]() {
                if (m_resourceState->phase()
                        != AsyncResourceState::Phase::Idle
                    && !m_port->favoritesRefreshing()) {
                    if (m_port->favoritesLoadFailed()) {
                        m_resourceState->reject(
                            m_resourceState->requestId(),
                            m_resourceState->resourceKey(), 0, 0,
                            tr("Favorites could not be loaded."));
                    } else {
                        m_resourceState->resolve(
                            m_resourceState->requestId(),
                            m_resourceState->resourceKey(), 0, 0, true);
                    }
                }
                emit stateChanged();
            });
    }
}

bool FavoritesViewModel::refreshing() const { return m_port && m_port->favoritesRefreshing(); }
bool FavoritesViewModel::loadFailed() const { return m_port && m_port->favoritesLoadFailed(); }
void FavoritesViewModel::load()
{
    m_resourceState->begin(QStringLiteral("favorites"), 0, 0, true);
    const CatalogPort::RequestDisposition disposition = m_port
        ? m_port->loadFavorites() : CatalogPort::RequestDisposition::Rejected;
    settleCatalogDisposition(m_resourceState, disposition, true,
        tr("Favorites are unavailable without an active server session."));
}
void FavoritesViewModel::refresh()
{
    m_resourceState->begin(QStringLiteral("favorites"), 0, 0, true);
    const CatalogPort::RequestDisposition disposition = m_port
        ? m_port->refreshFavorites() : CatalogPort::RequestDisposition::Rejected;
    settleCatalogDisposition(m_resourceState, disposition, true,
        tr("Favorites are unavailable without an active server session."));
}

PlaybackViewModel::PlaybackViewModel(
    PlaybackPort *port,
    PlaybackReporterPort *reporter,
    QObject *parent)
    : QObject(parent), m_port(port), m_reporter(reporter)
{
    m_preparationState = new AsyncResourceState(this);
    if (!port)
        return;
    connect(port, &PlaybackPort::ready, this,
        [this](const QString &requestId, const QVariantMap &descriptor) {
            const AsyncResourceState::Phase phase = m_preparationState->phase();
            if (requestId != m_pendingRequestId
                || (phase != AsyncResourceState::Phase::Loading
                    && phase != AsyncResourceState::Phase::Refreshing)) {
                return;
            }
            const quint64 stateRequestId = m_preparationState->requestId();
            const QString resourceKey = m_preparationState->resourceKey();
            m_pendingRequestId.clear();
            if (!m_preparationState->resolve(stateRequestId,
                    resourceKey, 0, 0, descriptor)) {
                return;
            }
            emit ready(descriptor);
        });
    connect(port, &PlaybackPort::failed, this,
        [this](const QString &requestId, const QString &itemId,
            const QString &message) {
            const AsyncResourceState::Phase phase = m_preparationState->phase();
            if (requestId != m_pendingRequestId
                || (phase != AsyncResourceState::Phase::Loading
                    && phase != AsyncResourceState::Phase::Refreshing)) {
                return;
            }
            const quint64 stateRequestId = m_preparationState->requestId();
            const QString resourceKey = m_preparationState->resourceKey();
            m_pendingRequestId.clear();
            if (!m_preparationState->reject(stateRequestId,
                    resourceKey, 0, 0, message)) {
                return;
            }
            emit failed(itemId, message);
        });
    connect(port, &PlaybackPort::stoppedReported, this, &PlaybackViewModel::stoppedReported);
}

void PlaybackViewModel::prepare(const QString &itemId)
{
    const QString requestId = beginPreparation(itemId);
    if (m_port)
        m_port->prepare(requestId, itemId);
}
void PlaybackViewModel::prepareInContext(const QString &itemId, const QVariantMap &context)
{
    const QString requestId = beginPreparation(itemId);
    if (m_port)
        m_port->prepareInContext(requestId, itemId, context);
}
void PlaybackViewModel::cancelPreparation()
{
    m_pendingRequestId.clear();
    m_preparationState->detach();
    if (m_port)
        m_port->cancelPreparation();
}
void PlaybackViewModel::switchTo(const QString &itemId, double positionSeconds, bool paused)
{
    if (m_reporter)
        m_reporter->stopSession();
    const QString requestId = beginPreparation(itemId);
    if (m_port)
        m_port->switchTo(requestId, itemId, positionSeconds, paused);
}
void PlaybackViewModel::switchToInContext(const QString &itemId, const QVariantMap &context,
    double positionSeconds, bool paused)
{
    if (m_reporter)
        m_reporter->stopSession();
    const QString requestId = beginPreparation(itemId);
    if (m_port)
        m_port->switchToInContext(
            requestId, itemId, context, positionSeconds, paused);
}

QString PlaybackViewModel::beginPreparation(const QString &itemId)
{
    m_preparingItemId = itemId;
    m_preparationState->begin(itemId, 0, 0, false);
    m_pendingRequestId = QStringLiteral("playback.%1.%2")
        .arg(++m_requestSequence)
        .arg(m_preparationState->requestId());
    return m_pendingRequestId;
}
bool PlaybackViewModel::attachPlayer(QObject *player)
{
    return m_reporter && m_reporter->attachPlayer(player);
}

bool PlaybackViewModel::beginSession(
    const QString &reportSessionId,
    const QVariantList &embyTracks)
{
    return m_reporter
        && m_reporter->beginSession(reportSessionId, embyTracks);
}

void PlaybackViewModel::stopSession()
{
    if (m_reporter)
        m_reporter->stopSession();
}

void PlaybackViewModel::invalidateSession()
{
    cancelPreparation();
    stopSession();
}

DanmakuViewModel::DanmakuViewModel(DanmakuPort *port, QObject *parent)
    : QObject(parent), m_port(port)
{
    m_searchState = new AsyncResourceState(this);
    m_automaticLoadState = new AsyncResourceState(this);
    m_applyOperation = new AsyncOperationState(this);
    m_configurationOperation = new AsyncOperationState(this);
    if (!port)
        return;
    connect(port, &DanmakuPort::stateChanged,
        this, &DanmakuViewModel::configuredChanged);
    connect(port, &DanmakuPort::stateChanged, this, &DanmakuViewModel::configurationBusyChanged);
    connect(port, &DanmakuPort::operationCompleted, this, &DanmakuViewModel::handleCompleted);
    connect(port, &DanmakuPort::operationFailed, this, &DanmakuViewModel::handleFailed);
    connect(port, &DanmakuPort::configurationCompleted,
        this, &DanmakuViewModel::handleConfigurationCompleted);
    connect(port, &DanmakuPort::configurationFailed,
        this, &DanmakuViewModel::handleConfigurationFailed);
}

bool DanmakuViewModel::configured() const { return m_port && m_port->configured(); }
int DanmakuViewModel::credentialSource() const { return m_port ? m_port->credentialSource() : 0; }
bool DanmakuViewModel::working() const { return m_searchPending || m_automaticLoadPending || m_applyMatchPending; }
bool DanmakuViewModel::configurationBusy() const { return m_port && m_port->configurationBusy(); }

void DanmakuViewModel::search(const QString &itemId, const QString &anime)
{
    const quint64 stateRequestId = m_searchState->begin(itemId, 0, 0, false);
    m_searchRequestId = QStringLiteral("danmaku.search.%1.%2")
        .arg(++m_operationSequence).arg(stateRequestId);
    setPending(DanmakuPort::Operation::Search, true);
    if (m_port) { m_port->search(m_searchRequestId, itemId, anime); return; }
    m_searchRequestId.clear();
    m_searchState->reject(stateRequestId, itemId, 0, 0,
        tr("Backend is unavailable."));
    setPending(DanmakuPort::Operation::Search, false);
    emit searchFailed(itemId, tr("Backend is unavailable."), false);
}

void DanmakuViewModel::loadAutomatically(const QString &itemId)
{
    const quint64 stateRequestId = m_automaticLoadState->begin(
        itemId, 0, 0, false);
    m_automaticLoadRequestId = QStringLiteral("danmaku.auto.%1.%2")
        .arg(++m_operationSequence).arg(stateRequestId);
    setPending(DanmakuPort::Operation::AutomaticLoad, true);
    if (m_port) {
        m_port->loadAutomatically(m_automaticLoadRequestId, itemId);
        return;
    }
    m_automaticLoadRequestId.clear();
    m_automaticLoadState->reject(stateRequestId, itemId, 0, 0,
        tr("Backend is unavailable."));
    setPending(DanmakuPort::Operation::AutomaticLoad, false);
    emit automaticLoadFailed(itemId, tr("Backend is unavailable."), false);
}

void DanmakuViewModel::applyMatch(const QString &itemId,
    const QVariantMap &match, const QVariantMap &style)
{
    if (m_applyOperation->busy())
        return;
    m_applyRequestId = QStringLiteral("danmaku.apply.%1.%2")
        .arg(itemId).arg(++m_operationSequence);
    m_applyItemId = itemId;
    m_applyOperation->begin(m_applyRequestId);
    setPending(DanmakuPort::Operation::ApplyMatch, true);
    if (m_port) {
        m_port->applyMatch(m_applyRequestId, itemId, match, style);
        return;
    }
    const QString requestId = m_applyRequestId;
    m_applyRequestId.clear();
    m_applyItemId.clear();
    m_applyOperation->reject(requestId, tr("Backend is unavailable."));
    setPending(DanmakuPort::Operation::ApplyMatch, false);
    emit matchApplyFailed(itemId, tr("Backend is unavailable."), false);
}

void DanmakuViewModel::configure(const QString &appId, const QString &appSecret)
{
    if (m_configurationOperation->busy())
        return;
    const QString mutationId = QStringLiteral("danmaku.configure.%1")
        .arg(++m_operationSequence);
    m_pendingConfigurationOperation =
        DanmakuPort::ConfigurationOperation::Configure;
    m_hasPendingConfigurationOperation = true;
    m_configurationOperation->begin(mutationId);
    if (m_port) {
        m_port->configure(mutationId, appId, appSecret);
    } else {
        m_hasPendingConfigurationOperation = false;
        m_configurationOperation->reject(
            mutationId, tr("Backend is unavailable."));
    }
}
void DanmakuViewModel::clearConfiguration()
{
    if (m_configurationOperation->busy())
        return;
    const QString mutationId = QStringLiteral("danmaku.clear.%1")
        .arg(++m_operationSequence);
    m_pendingConfigurationOperation =
        DanmakuPort::ConfigurationOperation::Clear;
    m_hasPendingConfigurationOperation = true;
    m_configurationOperation->begin(mutationId);
    if (m_port) {
        m_port->clearConfiguration(mutationId);
    } else {
        m_hasPendingConfigurationOperation = false;
        m_configurationOperation->reject(
            mutationId, tr("Backend is unavailable."));
    }
}

void DanmakuViewModel::invalidateSession()
{
    m_searchRequestId.clear();
    m_automaticLoadRequestId.clear();
    m_applyRequestId.clear();
    m_applyItemId.clear();
    m_searchState->detach();
    m_automaticLoadState->detach();
    m_applyOperation->reset();
    m_hasPendingConfigurationOperation = false;
    m_configurationOperation->reset();
    setPending(DanmakuPort::Operation::Search, false);
    setPending(DanmakuPort::Operation::AutomaticLoad, false);
    setPending(DanmakuPort::Operation::ApplyMatch, false);
}

void DanmakuViewModel::handleConfigurationCompleted(
    const QString &requestId,
    DanmakuPort::ConfigurationOperation operation)
{
    if (!m_hasPendingConfigurationOperation
        || operation != m_pendingConfigurationOperation
        || requestId != m_configurationOperation->mutationId()) {
        return;
    }
    m_hasPendingConfigurationOperation = false;
    m_configurationOperation->resolve(requestId, configured());
}

void DanmakuViewModel::handleConfigurationFailed(
    const QString &requestId,
    DanmakuPort::ConfigurationOperation operation,
    const QString &message)
{
    if (!m_hasPendingConfigurationOperation
        || operation != m_pendingConfigurationOperation
        || requestId != m_configurationOperation->mutationId()) {
        return;
    }
    m_hasPendingConfigurationOperation = false;
    m_configurationOperation->reject(requestId, message);
}

void DanmakuViewModel::setPending(DanmakuPort::Operation operation, bool pending)
{
    const bool wasWorking = working();
    switch (operation) {
    case DanmakuPort::Operation::Search: m_searchPending = pending; break;
    case DanmakuPort::Operation::AutomaticLoad: m_automaticLoadPending = pending; break;
    case DanmakuPort::Operation::ApplyMatch: m_applyMatchPending = pending; break;
    }
    if (working() != wasWorking)
        emit workingChanged();
}

void DanmakuViewModel::handleCompleted(const QString &requestId,
    const QString &itemId,
    DanmakuPort::Operation operation, const QVariantMap &result)
{
    switch (operation) {
    case DanmakuPort::Operation::Search: {
        if (requestId != m_searchRequestId
            || itemId != m_searchState->resourceKey()) return;
        const quint64 stateRequestId = m_searchState->requestId();
        m_searchRequestId.clear();
        if (!m_searchState->resolve(stateRequestId, itemId, 0, 0, result))
            return;
        setPending(operation, false);
        emit searchCompleted(itemId, result);
        break;
    }
    case DanmakuPort::Operation::AutomaticLoad: {
        if (requestId != m_automaticLoadRequestId
            || itemId != m_automaticLoadState->resourceKey()) return;
        const quint64 stateRequestId = m_automaticLoadState->requestId();
        m_automaticLoadRequestId.clear();
        if (!m_automaticLoadState->resolve(
                stateRequestId, itemId, 0, 0, result)) return;
        setPending(operation, false);
        emit automaticLoadCompleted(itemId, result);
        break;
    }
    case DanmakuPort::Operation::ApplyMatch: {
        if (!m_applyOperation->busy() || requestId != m_applyRequestId
            || itemId != m_applyItemId) return;
        const QString mutationId = m_applyRequestId;
        m_applyRequestId.clear();
        m_applyItemId.clear();
        if (!m_applyOperation->resolve(mutationId, result)) return;
        setPending(operation, false);
        emit matchApplied(itemId, result);
        break;
    }
    }
}

void DanmakuViewModel::handleFailed(const QString &requestId,
    const QString &itemId,
    DanmakuPort::Operation operation, const QString &message, bool nonModal)
{
    switch (operation) {
    case DanmakuPort::Operation::Search: {
        if (requestId != m_searchRequestId
            || itemId != m_searchState->resourceKey()) return;
        const quint64 stateRequestId = m_searchState->requestId();
        m_searchRequestId.clear();
        if (!m_searchState->reject(stateRequestId, itemId, 0, 0, message))
            return;
        setPending(operation, false);
        emit searchFailed(itemId, message, nonModal);
        break;
    }
    case DanmakuPort::Operation::AutomaticLoad: {
        if (requestId != m_automaticLoadRequestId
            || itemId != m_automaticLoadState->resourceKey()) return;
        const quint64 stateRequestId = m_automaticLoadState->requestId();
        m_automaticLoadRequestId.clear();
        if (!m_automaticLoadState->reject(
                stateRequestId, itemId, 0, 0, message)) return;
        setPending(operation, false);
        emit automaticLoadFailed(itemId, message, nonModal);
        break;
    }
    case DanmakuPort::Operation::ApplyMatch: {
        if (!m_applyOperation->busy() || requestId != m_applyRequestId
            || itemId != m_applyItemId) return;
        const QString mutationId = m_applyRequestId;
        m_applyRequestId.clear();
        m_applyItemId.clear();
        if (!m_applyOperation->reject(mutationId, message)) return;
        setPending(operation, false);
        emit matchApplyFailed(itemId, message, nonModal);
        break;
    }
    }
}

MediaActionsViewModel::MediaActionsViewModel(MediaPort *port, QObject *parent)
    : QObject(parent), m_port(port)
{
    if (!port)
        return;
    connect(port, &MediaPort::scanProgressChanged,
        this, &MediaActionsViewModel::libraryScanProgressChanged);
    connect(port, &MediaPort::operationCompleted,
        this, &MediaActionsViewModel::handleCompleted);
    connect(port, &MediaPort::operationFailed,
        this, &MediaActionsViewModel::handleFailed);
}

QVariantMap MediaActionsViewModel::libraryScanProgress() const
{ return m_port ? m_port->libraryScanProgress() : QVariantMap(); }

namespace {
QString unavailableMessage() { return MediaActionsViewModel::tr("Backend is unavailable."); }
}

void MediaActionsViewModel::refreshMetadata(const QString &itemId, const QString &mode,
    bool replaceImages, const QString &source)
{
    submitOperation(itemId, MediaPort::Operation::RefreshMetadata,
        [this, itemId, mode, replaceImages, source](const QString &id) {
            if (m_port) m_port->refreshMetadata(
                id, itemId, mode, replaceImages, source);
            else handleFailed(id, itemId, MediaPort::Operation::RefreshMetadata,
                unavailableMessage(), false);
        });
}
void MediaActionsViewModel::removeFromPlaylist(const QString &itemId, const QString &playlistId,
    const QString &entryId)
{
    submitOperation(itemId, MediaPort::Operation::RemoveFromPlaylist,
        [this, itemId, playlistId, entryId](const QString &id) {
            if (m_port) m_port->removeFromPlaylist(id, itemId, playlistId, entryId);
            else handleFailed(id, itemId, MediaPort::Operation::RemoveFromPlaylist,
                unavailableMessage(), false);
        });
}
void MediaActionsViewModel::setPlayed(const QString &itemId, bool played)
{
    submitOperation(itemId, MediaPort::Operation::SetPlayed,
        [this, itemId, played](const QString &id) {
            if (m_port) m_port->setPlayed(id, itemId, played);
            else handleFailed(id, itemId, MediaPort::Operation::SetPlayed,
                unavailableMessage(), false);
        });
}
void MediaActionsViewModel::setFavorite(const QString &itemId, bool favorite)
{
    submitOperation(itemId, MediaPort::Operation::SetFavorite,
        [this, itemId, favorite](const QString &id) {
            if (m_port) m_port->setFavorite(id, itemId, favorite);
            else handleFailed(id, itemId, MediaPort::Operation::SetFavorite,
                unavailableMessage(), false);
        });
}
void MediaActionsViewModel::scanLibraryFiles(const QString &itemId)
{
    submitOperation(itemId, MediaPort::Operation::ScanLibraryFiles,
        [this, itemId](const QString &id) {
            if (m_port) m_port->scanLibraryFiles(id, itemId);
            else handleFailed(id, itemId, MediaPort::Operation::ScanLibraryFiles,
                unavailableMessage(), false);
        });
}
void MediaActionsViewModel::deleteItem(const QString &itemId)
{
    submitOperation(itemId, MediaPort::Operation::DeleteItem,
        [this, itemId](const QString &id) {
            if (m_port) m_port->deleteItem(id, itemId);
            else handleFailed(id, itemId, MediaPort::Operation::DeleteItem,
                unavailableMessage(), false);
        });
}

void MediaActionsViewModel::submitOperation(
    const QString &itemId,
    MediaPort::Operation operation,
    std::function<void(const QString &)> dispatch)
{
    const QString key = operationKey(itemId, operation);
    AsyncOperationState *state = m_operationStates.value(key);
    if (!state) {
        state = new AsyncOperationState(this);
        m_operationStates.insert(key, state);
    }
    const QString mutationId = QStringLiteral("media.%1.%2.%3")
        .arg(static_cast<int>(operation))
        .arg(itemId)
        .arg(++m_operationSequence);
    QQueue<PendingOperation> &pending = m_pendingOperations[key];
    pending.enqueue(PendingOperation{mutationId, itemId, operation,
        std::move(dispatch)});
    m_knownRequests.insert(mutationId, key);
    if (pending.size() == 1) {
        state->begin(mutationId);
        dispatchNextOperation(key);
    }
}

void MediaActionsViewModel::dispatchNextOperation(const QString &key)
{
    auto iterator = m_pendingOperations.find(key);
    if (iterator == m_pendingOperations.end() || iterator->isEmpty())
        return;
    const PendingOperation pending = iterator->head();
    if (pending.dispatch)
        pending.dispatch(pending.requestId);
}

QString MediaActionsViewModel::operationKey(
    const QString &itemId,
    MediaPort::Operation operation) const
{
    return QString::number(static_cast<int>(operation))
        + QLatin1Char(':') + itemId;
}

void MediaActionsViewModel::handleCompleted(const QString &requestId,
    const QString &itemId,
    MediaPort::Operation operation, const QVariantMap &result)
{
    const QString key = operationKey(itemId, operation);
    if (m_knownRequests.value(requestId) != key)
        return;
    bool dispatchNext = false;
    auto pendingIterator = m_pendingOperations.find(key);
    if (pendingIterator != m_pendingOperations.end()) {
        if (pendingIterator->isEmpty()
            || pendingIterator->head().requestId != requestId) {
            return;
        }
        m_knownRequests.remove(requestId);
        pendingIterator->dequeue();
        AsyncOperationState *state = m_operationStates.value(key);
        if (!state || !state->resolve(requestId, result))
            return;
        if (pendingIterator->isEmpty()) {
            m_pendingOperations.erase(pendingIterator);
        } else {
            state->begin(pendingIterator->head().requestId);
            dispatchNext = true;
        }
    } else {
        m_knownRequests.remove(requestId);
    }
    switch (operation) {
    case MediaPort::Operation::RefreshMetadata: emit metadataRefreshed(itemId, result); break;
    case MediaPort::Operation::RemoveFromPlaylist: emit removedFromPlaylist(itemId, result); break;
    case MediaPort::Operation::SetPlayed:
        emit playedChanged(itemId, result.value(QStringLiteral("requestedPlayed")).toBool(), result); break;
    case MediaPort::Operation::SetFavorite:
        emit favoriteChanged(itemId, result.value(QStringLiteral("requestedFavorite")).toBool(), result); break;
    case MediaPort::Operation::ScanLibraryFiles: emit libraryFilesScanStarted(itemId, result); break;
    case MediaPort::Operation::DeleteItem: emit itemDeleted(itemId, result); break;
    default: break;
    }
    if (dispatchNext)
        dispatchNextOperation(key);
}

void MediaActionsViewModel::handleFailed(const QString &requestId,
    const QString &itemId,
    MediaPort::Operation operation, const QString &message, bool nonModal)
{
    const QString key = operationKey(itemId, operation);
    if (m_knownRequests.value(requestId) != key)
        return;
    bool dispatchNext = false;
    auto pendingIterator = m_pendingOperations.find(key);
    if (pendingIterator != m_pendingOperations.end()) {
        if (pendingIterator->isEmpty()
            || pendingIterator->head().requestId != requestId) {
            return;
        }
        m_knownRequests.remove(requestId);
        pendingIterator->dequeue();
        AsyncOperationState *state = m_operationStates.value(key);
        if (!state || !state->reject(requestId, message))
            return;
        if (pendingIterator->isEmpty()) {
            m_pendingOperations.erase(pendingIterator);
        } else {
            state->begin(pendingIterator->head().requestId);
            dispatchNext = true;
        }
    } else {
        m_knownRequests.remove(requestId);
    }
    switch (operation) {
    case MediaPort::Operation::RefreshMetadata: emit metadataRefreshFailed(itemId, message, nonModal); break;
    case MediaPort::Operation::RemoveFromPlaylist: emit removeFromPlaylistFailed(itemId, message, nonModal); break;
    case MediaPort::Operation::SetPlayed: emit playStateChangeFailed(itemId, message, nonModal); break;
    case MediaPort::Operation::SetFavorite: emit favoriteChangeFailed(itemId, message, nonModal); break;
    case MediaPort::Operation::ScanLibraryFiles: emit libraryFilesScanFailed(itemId, message, nonModal); break;
    case MediaPort::Operation::DeleteItem: emit itemDeleteFailed(itemId, message, nonModal); break;
    default: break;
    }
    if (dispatchNext)
        dispatchNextOperation(key);
}

PreferencesViewModel::PreferencesViewModel(QObject *parent) : QObject(parent)
{
    bool converted = false;
    const int storedSortMode = QSettings()
        .value(QStringLiteral("library/sortMode"), kDefaultLibrarySortMode)
        .toInt(&converted);
    m_librarySortMode = converted && isLibrarySortModeValid(storedSortMode)
        ? storedSortMode : kDefaultLibrarySortMode;
}

QVariantMap PreferencesViewModel::danmakuStyle() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("danmaku"));
    return {
        {QStringLiteral("fontSize"), settings.value(QStringLiteral("fontSize"), 42.0)},
        {QStringLiteral("opacity"), settings.value(QStringLiteral("opacity"), 0.88)},
        {QStringLiteral("scrollDuration"), settings.value(QStringLiteral("scrollDuration"), 9.0)},
        {QStringLiteral("displayArea"), settings.value(QStringLiteral("displayArea"), 0.70)},
        {QStringLiteral("density"), settings.value(QStringLiteral("density"), 14)},
        {QStringLiteral("timeOffset"), settings.value(QStringLiteral("timeOffset"), 0.0)},
        {QStringLiteral("blockedTerms"), settings.value(QStringLiteral("blockedTerms"), QString())},
        {QStringLiteral("showScroll"), settings.value(QStringLiteral("showScroll"), true)},
        {QStringLiteral("showTop"), settings.value(QStringLiteral("showTop"), true)},
        {QStringLiteral("showBottom"), settings.value(QStringLiteral("showBottom"), true)},
        {QStringLiteral("topMargin"), settings.value(QStringLiteral("topMargin"), 0.0)},
    };
}

void PreferencesViewModel::saveDanmakuStyle(const QVariantMap &style)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("danmaku"));
    const QVariantMap defaults = danmakuStyle();
    for (const QString &key : { QStringLiteral("fontSize"), QStringLiteral("opacity"),
             QStringLiteral("scrollDuration"), QStringLiteral("displayArea"),
             QStringLiteral("density"), QStringLiteral("timeOffset"),
             QStringLiteral("blockedTerms"), QStringLiteral("showScroll"),
             QStringLiteral("showTop"), QStringLiteral("showBottom"),
             QStringLiteral("topMargin") }) {
        settings.setValue(key, style.value(key, defaults.value(key)));
    }
    emit danmakuStyleChanged();
}

void PreferencesViewModel::setLibrarySortMode(int sortMode)
{
    if (!isLibrarySortModeValid(sortMode) || m_librarySortMode == sortMode)
        return;
    m_librarySortMode = sortMode;
    QSettings().setValue(QStringLiteral("library/sortMode"), sortMode);
    emit librarySortModeChanged();
}

ApplicationStatusViewModel::ApplicationStatusViewModel(
    ApplicationStatusPort *port, QObject *parent)
    : QObject(parent), m_port(port)
{
    if (port)
        connect(port, &ApplicationStatusPort::stateChanged,
            this, &ApplicationStatusViewModel::stateChanged);
}

bool ApplicationStatusViewModel::ready() const { return m_port && m_port->ready(); }
QString ApplicationStatusViewModel::message() const { return m_port ? m_port->message() : QString(); }
bool ApplicationStatusViewModel::error() const { return m_port && m_port->error(); }
void ApplicationStatusViewModel::clear() { if (m_port) m_port->clear(); }

ApplicationViewModel::ApplicationViewModel(const BackendPortSet &ports, QObject *parent)
    : QObject(parent)
{
    initialize(ports);
}

void ApplicationViewModel::initialize(const BackendPortSet &ports)
{
    m_session = new SessionViewModel(ports.session, this);
    m_home = new HomeViewModel(ports.catalog, this);
    m_favorites = new FavoritesViewModel(ports.catalog, this);
    m_playback = new PlaybackViewModel(
        ports.playback, ports.playbackReporter, this);
    m_danmaku = new DanmakuViewModel(ports.danmaku, this);
    m_mediaActions = new MediaActionsViewModel(ports.media, this);
    m_imageEditor = new ImageEditorViewModel(ports.media, this);
    m_metadataEditor = new MetadataEditorViewModel(ports.media, this);
    m_mediaTarget = new MediaTargetFlowViewModel(ports.media, this);
    m_preferences = new PreferencesViewModel(this);
    m_status = new ApplicationStatusViewModel(ports.status, this);
    m_updates = new UpdateChecker(this);

    const QPointer<SessionPort> session = ports.session;
    if (session) {
        m_sessionGeneration = session->generation();
        m_imageEditor->setSessionGeneration(m_sessionGeneration);
        connect(session, &SessionPort::stateChanged, this,
            [this, session] {
                if (!session || session->generation() == m_sessionGeneration)
                    return;
                m_sessionGeneration = session->generation();
                m_imageEditor->setSessionGeneration(m_sessionGeneration);
                m_metadataEditor->invalidateSession();
                m_mediaTarget->cancel();
                m_playback->invalidateSession();
                m_danmaku->invalidateSession();
            });
    }

    const QPointer<CatalogPort> catalog = ports.catalog;
    if (ports.playback && catalog) {
        connect(ports.playback, &PlaybackPort::stoppedReported, this,
            [this, catalog, session] {
                const quint64 generation = session ? session->generation() : 0;
                QTimer::singleShot(800, this, [catalog, session, generation] {
                    if (catalog
                        && (!session || session->generation() == generation)) {
                        catalog->refreshActivity();
                    }
                });
                QTimer::singleShot(3200, this, [catalog, session, generation] {
                    if (catalog
                        && (!session || session->generation() == generation)) {
                        catalog->refreshActivity();
                    }
                });
            });
    }
    if (m_mediaActions && catalog) {
        connect(m_mediaActions, &MediaActionsViewModel::playedChanged, this,
            [this, catalog, session](const QString &, bool,
                const QVariantMap &result) {
                if (result.value(QStringLiteral("reconcileComplete"), true).toBool())
                    return;
                const quint64 generation = session ? session->generation() : 0;
                QTimer::singleShot(1800, this,
                    [catalog, session, generation] {
                    if (!catalog
                        || (session && session->generation() != generation)) {
                        return;
                    }
                    catalog->loadLibrary();
                    const QString parentId = catalog->collectionDisplayedId();
                    if (!parentId.isEmpty())
                        catalog->refreshCollection(parentId);
                });
            });
    }
}
