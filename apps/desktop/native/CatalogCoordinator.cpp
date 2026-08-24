#include "CatalogCoordinator.hpp"

#include "BackendInfrastructure.hpp"
#include "CatalogFreshnessPolicy.hpp"

#include <QDateTime>
#include <QDebug>
#include <QTimer>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr qint64 collectionCacheTtlMs = 5 * 60 * 1000;
constexpr qint64 favoritesCacheFreshMs = 2 * 60 * 1000;
const QString libraryRequestKey = QStringLiteral("library");
const QString activityRequestKey = QStringLiteral("activity");
const QString favoritesRequestKey = QStringLiteral("favorites");
const QString libraryNavigationLaneKey = QStringLiteral("navigation.library");
const QString collectionNavigationLaneKey = QStringLiteral("navigation.collection");

QString collectionRequestKey(const QString &parentId)
{
    return QStringLiteral("collection:") + parentId;
}

} // namespace

CatalogCoordinator::CatalogCoordinator(
    RustBridgeRuntime &runtime,
    QThreadPool &navigationPool,
    SessionStateProvider sessionStateProvider,
    StatusSink &statusSink,
    QString cacheDirectory,
    CapabilitySink capabilitySink,
    QObject *parent)
    : CatalogPort(parent)
    , m_runtime(runtime)
    , m_navigationPool(navigationPool)
    , m_sessionStateProvider(std::move(sessionStateProvider))
    , m_statusSink(statusSink)
    , m_capabilitySink(std::move(capabilitySink))
    , m_mediaStore(std::make_unique<MediaStore>())
    , m_cacheDirectory(std::move(cacheDirectory))
{
    for (NavigationLane &lane : m_navigationLanes) {
        NavigationLane *lanePointer = &lane;
        connect(
            &lane.watcher,
            &QFutureWatcher<YanamiOperationResult>::finished,
            this,
            [this, lanePointer] { finishNavigationQuery(*lanePointer); });
    }
    connect(
        &m_activityWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this,
        &CatalogCoordinator::finishActivityRefresh);
    connect(
        &m_favoritesWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this,
        &CatalogCoordinator::finishFavoritesRefresh);
    connect(
        m_mediaStore.get(),
        &MediaStore::queryChanged,
        this,
        [this](const QString &kind, const QString &scopeId) {
            if (kind == QStringLiteral("collection")
                && scopeId == m_collectionDisplayedId) {
                emit stateChanged();
            }
        });
    connect(
        m_mediaStore.get(),
        &MediaStore::entityChanged,
        this,
        [this](const QString &entityId) {
            const QVariantMap parent = m_mediaStore->queryParent(
                QStringLiteral("collection"), m_collectionDisplayedId);
            if (!parent.isEmpty()
                && parent.value(QStringLiteral("id")).toString() == entityId) {
                emit stateChanged();
            }
        });
}

CatalogCoordinator::~CatalogCoordinator()
{
    shutdown();
    drain();
}

bool CatalogCoordinator::initializeFromSession()
{
    if (m_initialized)
        return activeSession();
    m_initialized = true;
    m_committedSession = currentSession();
    m_capabilities = m_committedSession.capabilities;
    m_sessionFenced = false;
    updateLibraryCachePath();
    publishCapabilities();
    if (!activeSession()) {
        emit stateChanged();
        return false;
    }
    loadLibraryCache();
    QTimer::singleShot(0, this, [this, generation = m_committedSession.generation] {
        if (acceptsSession(generation))
            loadLibrary();
    });
    emit stateChanged();
    return true;
}

void CatalogCoordinator::sessionTransitionStarted(const char *reason)
{
    if (m_shuttingDown)
        return;
    m_sessionFenced = true;
    resetNavigationQueries(reason);
    m_requests.invalidateAll();
    m_activityRefreshQueued = false;
    m_favoritesRefreshQueued = false;
    const bool stateWasBusy = m_activityRefreshing || m_favoritesRefreshing;
    m_activityRefreshing = false;
    m_favoritesRefreshing = false;
    if (stateWasBusy)
        emit stateChanged();
}

void CatalogCoordinator::sessionTransitionAborted()
{
    if (m_shuttingDown)
        return;
    m_sessionFenced = false;
    pumpNavigationQueries();
    if (m_activityRefreshQueued)
        QTimer::singleShot(0, this, &CatalogCoordinator::refreshActivity);
    if (m_favoritesRefreshQueued)
        QTimer::singleShot(0, this, &CatalogCoordinator::refreshFavorites);
    emit stateChanged();
}

void CatalogCoordinator::sessionCommitted()
{
    if (m_shuttingDown)
        return;
    const CatalogSessionState next = currentSession();
    clearLibraryCache();
    resetCommittedState(false);
    m_committedSession = next;
    m_capabilities = next.capabilities;
    m_sessionFenced = false;
    updateLibraryCachePath();
    publishCapabilities();
    if (activeSession()) {
        loadLibraryCache();
        QTimer::singleShot(0, this, [this, generation = next.generation] {
            if (acceptsSession(generation))
                loadLibrary();
        });
    }
    emit stateChanged();
}

void CatalogCoordinator::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_sessionFenced = true;
    resetNavigationQueries("shutdown");
    m_requests.invalidateAll();
    m_activityRefreshQueued = false;
    m_favoritesRefreshQueued = false;
}

void CatalogCoordinator::drain()
{
    for (NavigationLane &lane : m_navigationLanes) {
        if (lane.watcher.isRunning())
            lane.watcher.waitForFinished();
    }
    if (m_activityWatcher.isRunning())
        m_activityWatcher.waitForFinished();
    if (m_favoritesWatcher.isRunning())
        m_favoritesWatcher.waitForFinished();
}

bool CatalogCoordinator::libraryRefreshing() const
{
    return navigationQueryPending(NavigationKind::Library);
}

bool CatalogCoordinator::collectionLoading() const
{
    return !m_collectionTargetId.isEmpty()
        && navigationQueryPending(
            NavigationKind::Collection, m_collectionTargetId);
}

bool CatalogCoordinator::collectionFetching() const
{
    return !m_collectionTargetId.isEmpty()
        && navigationQueryPending(
            NavigationKind::Collection, m_collectionTargetId, true);
}

QVariantMap CatalogCoordinator::collectionParent() const
{
    return m_mediaStore->queryParent(
        QStringLiteral("collection"), m_collectionDisplayedId);
}

CatalogPort::RequestDisposition CatalogCoordinator::loadLibrary()
{
    if (!activeSession())
        return RequestDisposition::Rejected;
    if (promoteEquivalentNavigationQuery(
            NavigationKind::Library,
            libraryRequestKey,
            libraryNavigationLaneKey)) {
        return RequestDisposition::Accepted;
    }
    if (m_libraryLoadFailed) {
        m_libraryLoadFailed = false;
    }
    NavigationRequest request;
    request.kind = NavigationKind::Library;
    request.resourceKey = libraryRequestKey;
    request.resourceToken = m_requests.begin(
        libraryRequestKey, m_committedSession.generation);
    request.presentationToken = m_requests.beginLatest(
        libraryNavigationLaneKey,
        libraryRequestKey,
        m_committedSession.generation);
    request.sessionGeneration = m_committedSession.generation;
    request.activityRevision = ++m_activityRevisionIssued;
    request.enqueuedAtMs = QDateTime::currentMSecsSinceEpoch();
    request.hadCachedData = m_mediaStore->libraryViewsModel()->rowCount() > 0
        || m_mediaStore->libraryModel()->rowCount() > 0;
    submitNavigationQuery(std::move(request));
    return RequestDisposition::Accepted;
}

CatalogPort::RequestDisposition CatalogCoordinator::refreshActivity()
{
    return requestActivityRefresh(true);
}

void CatalogCoordinator::invalidateActivity()
{
    if (!activeSession())
        return;
    m_mediaStore->markQueryStale(QStringLiteral("resume"));
    m_mediaStore->markQueryStale(QStringLiteral("recent"));
    m_requests.invalidate({activityRequestKey});
    m_activityRetryAfterMs = 0;
    emit stateChanged();
}

CatalogPort::RequestDisposition CatalogCoordinator::ensureActivityFresh()
{
    return requestActivityRefresh(false);
}

CatalogPort::RequestDisposition CatalogCoordinator::requestActivityRefresh(
    bool force)
{
    if (!activeSession())
        return RequestDisposition::Rejected;
    if (m_activityRefreshing || m_activityWatcher.isRunning()) {
        if (force) {
            m_activityRefreshQueued = true;
            qInfo().noquote()
                << "backend_queue"
                << "action=enqueue"
                << "kind=activity_refresh"
                << "reason=forced_while_in_flight";
        }
        return RequestDisposition::Accepted;
    }
    if (!force) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now < m_activityRetryAfterMs)
            return RequestDisposition::AlreadyCurrent;
        const auto freshness = [this](const QString &kind) {
            return CatalogQueryFreshness {
                .available = m_mediaStore->hasQuery(kind),
                .stale = m_mediaStore->queryStale(kind),
                .fetchedAtMs = m_mediaStore->queryFetchedAtMs(kind),
            };
        };
        if (CatalogFreshnessPolicy::activityIsFresh(
                freshness(QStringLiteral("resume")),
                freshness(QStringLiteral("recent")), now)) {
            return RequestDisposition::AlreadyCurrent;
        }
    }
    m_activityRefreshing = true;
    beginActivityRefresh();
    emit stateChanged();
    return RequestDisposition::Accepted;
}

void CatalogCoordinator::beginActivityRefresh()
{
    if (!activeSession()) {
        if (m_activityRefreshing) {
            m_activityRefreshing = false;
            emit stateChanged();
        }
        return;
    }
    Q_ASSERT(!m_activityWatcher.isRunning());
    m_activityRefreshQueued = false;
    m_activitySessionGeneration = m_committedSession.generation;
    m_activityRequest = m_requests.begin(
        activityRequestKey, m_committedSession.generation);
    m_activityRequestRevision = ++m_activityRevisionIssued;
    m_activityTimer.start();
    qInfo().noquote()
        << "activity_refresh"
        << "phase=start"
        << "sessionGeneration=" << m_activitySessionGeneration
        << "requestGeneration=" << m_activityRequest.queryGeneration;
    m_activityWatcher.setFuture(QtConcurrent::run(
        &m_navigationPool,
        [this] {
            return m_runtime.catalog(
                RustBridgeRuntime::CatalogQuery::Activity);
        }));
}

CatalogPort::RequestDisposition CatalogCoordinator::loadFavorites()
{
    return startFavoritesRefresh(false);
}

CatalogPort::RequestDisposition CatalogCoordinator::refreshFavorites()
{
    return startFavoritesRefresh(true);
}

CatalogPort::RequestDisposition CatalogCoordinator::startFavoritesRefresh(
    bool force)
{
    if (!activeSession())
        return RequestDisposition::Rejected;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_lastFavoritesRefreshMs > 0
        && now - m_lastFavoritesRefreshMs < favoritesCacheFreshMs) {
        return RequestDisposition::AlreadyCurrent;
    }
    if (m_favoritesRefreshing || m_favoritesWatcher.isRunning()) {
        if (force)
            m_favoritesRefreshQueued = true;
        return RequestDisposition::Accepted;
    }
    m_favoritesRefreshing = true;
    beginFavoritesRefresh(force);
    emit stateChanged();
    return RequestDisposition::Accepted;
}

void CatalogCoordinator::beginFavoritesRefresh(bool force)
{
    if (!activeSession()) {
        if (m_favoritesRefreshing) {
            m_favoritesRefreshing = false;
            emit stateChanged();
        }
        return;
    }
    Q_ASSERT(!m_favoritesWatcher.isRunning());
    m_favoritesRefreshQueued = false;
    m_favoritesSessionGeneration = m_committedSession.generation;
    m_favoritesRequest = m_requests.begin(
        favoritesRequestKey, m_committedSession.generation);
    qInfo().noquote()
        << "favorites_refresh"
        << "phase=start"
        << "force=" << force
        << "cachedItems="
        << m_mediaStore->favoritesModel()->rowCount()
        << "sessionGeneration=" << m_favoritesSessionGeneration
        << "requestGeneration=" << m_favoritesRequest.queryGeneration;
    m_favoritesWatcher.setFuture(QtConcurrent::run(
        &m_navigationPool,
        [this] {
            return m_runtime.catalog(
                RustBridgeRuntime::CatalogQuery::Favorites);
        }));
}

CatalogPort::RequestDisposition CatalogCoordinator::loadCollection(
    const QString &parentId)
{
    return requestCollection(parentId, false);
}

CatalogPort::RequestDisposition CatalogCoordinator::requestCollection(
    const QString &parentId,
    bool forceRefresh)
{
    if (!activeSession() || parentId.trimmed().isEmpty())
        return RequestDisposition::Rejected;
    if (m_collectionTargetId != parentId) {
        m_collectionTargetId = parentId;
        m_requests.invalidateLatestLane(collectionNavigationLaneKey);
    }
    if (m_collectionErrorId == parentId) {
        m_collectionErrorId.clear();
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool hasCachedCollection = m_mediaStore->hasQuery(
        QStringLiteral("collection"), parentId);
    qInfo().noquote()
        << "collection_request"
        << "parent=" << parentId
        << "cached=" << hasCachedCollection
        << "pending=" << collectionLoading()
        << "target=" << m_collectionTargetId;
    if (hasCachedCollection) {
        publishCachedCollection(parentId);
        if (!forceRefresh
            && !m_mediaStore->queryStale(
                QStringLiteral("collection"), parentId)
            && now - m_mediaStore->queryFetchedAtMs(
                QStringLiteral("collection"), parentId)
                < collectionCacheTtlMs) {
            // This is the only path that does not register a request. Publish
            // the cached target now so presentation may settle immediately.
            emit stateChanged();
            return RequestDisposition::AlreadyCurrent;
        }
    }
    const QString resourceKey = collectionRequestKey(parentId);
    if (promoteEquivalentNavigationQuery(
            NavigationKind::Collection,
            resourceKey,
            collectionNavigationLaneKey)) {
        if (forceRefresh) {
            m_mediaStore->markQueryStale(
                QStringLiteral("collection"), parentId);
        }
        return RequestDisposition::Accepted;
    }
    NavigationRequest request;
    request.kind = NavigationKind::Collection;
    request.resourceKey = resourceKey;
    request.parentId = parentId;
    request.resourceToken = m_requests.begin(
        resourceKey, m_committedSession.generation);
    request.presentationToken = m_requests.beginLatest(
        collectionNavigationLaneKey,
        resourceKey,
        m_committedSession.generation);
    request.sessionGeneration = m_committedSession.generation;
    request.enqueuedAtMs = QDateTime::currentMSecsSinceEpoch();
    request.hadCachedData = hasCachedCollection;
    submitNavigationQuery(std::move(request));
    if (forceRefresh) {
        // Admission is visible before stale-state signals can synchronously
        // ask presentation to settle the refresh operation.
        m_mediaStore->markQueryStale(
            QStringLiteral("collection"), parentId);
    }
    return RequestDisposition::Accepted;
}

CatalogPort::RequestDisposition CatalogCoordinator::refreshCollection(
    const QString &parentId)
{
    if (!activeSession() || parentId.trimmed().isEmpty())
        return RequestDisposition::Rejected;
    const RequestDisposition disposition = requestCollection(parentId, true);
    return disposition == RequestDisposition::AlreadyCurrent
        ? RequestDisposition::Rejected
        : disposition;
}

void CatalogCoordinator::publishCachedCollection(const QString &parentId)
{
    if (!m_mediaStore->hasQuery(QStringLiteral("collection"), parentId))
        return;
    m_collectionDisplayedId = parentId;
}

CatalogSessionState CatalogCoordinator::currentSession() const
{
    return m_sessionStateProvider
        ? m_sessionStateProvider()
        : CatalogSessionState {};
}

bool CatalogCoordinator::activeSession() const
{
    if (!m_initialized || m_shuttingDown || m_sessionFenced
        || !m_runtime.ready() || !m_committedSession.connected) {
        return false;
    }
    const CatalogSessionState current = currentSession();
    return current.connected
        && current.generation == m_committedSession.generation;
}

bool CatalogCoordinator::acceptsSession(quint64 generation) const
{
    return activeSession()
        && generation == m_committedSession.generation;
}

void CatalogCoordinator::publishCapabilities()
{
    if (m_capabilitySink && m_committedSession.generation != 0)
        m_capabilitySink(m_committedSession.generation, m_capabilities);
}

void CatalogCoordinator::resetCommittedState(bool removeDiskCache)
{
    resetNavigationQueries("session_reset");
    m_requests.invalidateAll();
    if (removeDiskCache)
        clearLibraryCache();
    m_mediaStore->clear();
    m_collectionTargetId.clear();
    m_collectionDisplayedId.clear();
    m_collectionErrorId.clear();
    m_capabilities = {};
    m_lastFullLibraryRefreshMs = 0;
    m_lastFavoritesRefreshMs = 0;
    m_activityRetryAfterMs = 0;
    m_activityConsecutiveFailures = 0;
    m_activityRequestRevision = 0;
    m_activityRevisionIssued = 0;
    m_activityRevisionCommitted = 0;
    m_activityRefreshQueued = false;
    m_favoritesRefreshQueued = false;
    m_activityRefreshing = false;
    m_activityLoadFailed = false;
    m_favoritesRefreshing = false;
    m_favoritesLoadFailed = false;
    m_libraryLoadFailed = false;
    emit stateChanged();
}
