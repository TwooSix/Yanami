#include "CatalogCoordinator.hpp"

#include "BackendInfrastructure.hpp"
#include "CatalogFreshnessPolicy.hpp"
#include "DevelopmentHooks.hpp"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QThread>
#include <QTimer>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr int desktopSchemaVersion = 8;
constexpr qint64 activityAutomaticRetryBaseMs = 5 * 1000;
constexpr qint64 activityAutomaticRetryMaxMs = 60 * 1000;
const QString libraryRequestKey = QStringLiteral("library");
const QString activityRequestKey = QStringLiteral("activity");
const QString favoritesRequestKey = QStringLiteral("favorites");
const QString libraryNavigationLaneKey = QStringLiteral("navigation.library");
const QString collectionNavigationLaneKey = QStringLiteral("navigation.collection");

QString schemaVersionLabel(const QJsonValue &value)
{
    if (value.isUndefined())
        return QStringLiteral("missing");
    if (!value.isDouble())
        return QStringLiteral("invalid");
    return QString::number(value.toDouble(), 'g', 16);
}

QVariantList normalizedQueryItems(
    const QJsonObject &response,
    const QString &kind)
{
    const QJsonObject entities =
        response.value(QStringLiteral("entities")).toObject();
    const QJsonObject query = response.value(QStringLiteral("queries"))
        .toObject().value(kind).toObject();
    QVariantList items;
    const QJsonArray rows = query.value(QStringLiteral("rows")).toArray();
    items.reserve(rows.size());
    for (const QJsonValue &rowValue : rows) {
        const QJsonObject row = rowValue.toObject();
        const QString entityId =
            row.value(QStringLiteral("entityId")).toString();
        const QJsonObject entity = entities.value(entityId).toObject();
        if (entityId.isEmpty() || entity.isEmpty())
            continue;
        QVariantMap item = entity.toVariantMap();
        const QVariantMap decoration = row.value(QStringLiteral("decoration"))
            .toObject().toVariantMap();
        for (auto iterator = decoration.cbegin();
             iterator != decoration.cend(); ++iterator) {
            item.insert(iterator.key(), iterator.value());
        }
        if (decoration.contains(QStringLiteral("title")))
            item.insert(QStringLiteral("titleIsContextual"), true);
        items.push_back(item);
    }
    return items;
}

QVariantMap normalizedQueryParent(
    const QJsonObject &response,
    const QString &kind)
{
    const QJsonObject query = response.value(QStringLiteral("queries"))
        .toObject().value(kind).toObject();
    const QString parentId =
        query.value(QStringLiteral("parentId")).toString();
    QVariantMap parent = response.value(QStringLiteral("entities"))
        .toObject().value(parentId).toObject().toVariantMap();
    const QVariantMap decoration = query.value(
        QStringLiteral("parentDecoration")).toObject().toVariantMap();
    for (auto iterator = decoration.cbegin();
         iterator != decoration.cend(); ++iterator) {
        parent.insert(iterator.key(), iterator.value());
    }
    return parent;
}

bool hasNormalizedQuery(const QJsonObject &response, const QString &kind)
{
    return response.value(QStringLiteral("entities")).isObject()
        && response.value(QStringLiteral("queries"))
            .toObject().value(kind).isObject();
}

} // namespace

bool CatalogCoordinator::promoteEquivalentNavigationQuery(
    NavigationKind kind,
    const QString &resourceKey,
    const QString &laneKey)
{
    const auto promote = [this, kind, &resourceKey, &laneKey](
                             NavigationRequest &request,
                             const char *location) {
        if (request.kind != kind
            || request.resourceKey != resourceKey
            || request.sessionGeneration != m_committedSession.generation
            || !m_requests.accepts(
                request.resourceToken, m_committedSession.generation)) {
            return false;
        }
        request.presentationToken = m_requests.beginLatest(
            laneKey, resourceKey, m_committedSession.generation);
        qInfo().noquote()
            << "navigation_query"
            << "phase=coalesced"
            << "lane=" << laneKey
            << "resourceKey=" << resourceKey
            << "location=" << location
            << "requestId=" << request.presentationToken.requestId;
        notifyNavigationStateChanged();
        return true;
    };
    for (NavigationLane &lane : m_navigationLanes) {
        if (lane.active.has_value() && promote(*lane.active, "active"))
            return true;
    }
    std::optional<NavigationRequest> &queued =
        kind == NavigationKind::Library
        ? m_queuedLibraryNavigation
        : m_queuedCollectionNavigation;
    return queued.has_value() && promote(*queued, "queued");
}

void CatalogCoordinator::submitNavigationQuery(NavigationRequest request)
{
    for (NavigationLane &lane : m_navigationLanes) {
        if (!lane.active.has_value() && !lane.watcher.isRunning()) {
            startNavigationQuery(lane, std::move(request));
            notifyNavigationStateChanged();
            return;
        }
    }
    std::optional<NavigationRequest> &queued =
        request.kind == NavigationKind::Library
        ? m_queuedLibraryNavigation
        : m_queuedCollectionNavigation;
    if (queued.has_value()) {
        qInfo().noquote()
            << "navigation_query"
            << "phase=drop"
            << "lane=" << queued->presentationToken.laneKey
            << "resourceKey=" << queued->resourceKey
            << "reason=latest_wins";
    }
    qInfo().noquote()
        << "navigation_query"
        << "phase=queued"
        << "lane=" << request.presentationToken.laneKey
        << "requestId=" << request.presentationToken.requestId
        << "resourceKey=" << request.resourceKey;
    queued = std::move(request);
    notifyNavigationStateChanged();
}

void CatalogCoordinator::startNavigationQuery(
    NavigationLane &lane,
    NavigationRequest request)
{
    if (lane.active.has_value() || lane.watcher.isRunning())
        return;
    const CatalogSessionState session = currentSession();
    if (!activeSession()
        || request.sessionGeneration != session.generation
        || !m_requests.acceptsLatest(
            request.presentationToken, session.generation)) {
        return;
    }
    lane.active = std::move(request);
    lane.operationTimer.start();
    const NavigationRequest transportRequest = *lane.active;
    const qint64 schedulerWaitMs = transportRequest.enqueuedAtMs > 0
        ? QDateTime::currentMSecsSinceEpoch() - transportRequest.enqueuedAtMs
        : 0;
    qInfo().noquote()
        << "navigation_query"
        << "phase=start"
        << "lane=" << transportRequest.presentationToken.laneKey
        << "requestId=" << transportRequest.presentationToken.requestId
        << "resourceKey=" << transportRequest.resourceKey
        << "sessionGeneration=" << transportRequest.sessionGeneration
        << "schedulerWaitMs=" << schedulerWaitMs;
    lane.watcher.setFuture(QtConcurrent::run(
        &m_navigationPool,
        [this, transportRequest] {
            const qint64 queueWaitMs = transportRequest.enqueuedAtMs > 0
                ? QDateTime::currentMSecsSinceEpoch()
                    - transportRequest.enqueuedAtMs
                : 0;
            qInfo().noquote()
                << "navigation_query"
                << "phase=transport_start"
                << "lane=" << transportRequest.presentationToken.laneKey
                << "requestId=" << transportRequest.presentationToken.requestId
                << "resourceKey=" << transportRequest.resourceKey
                << "queueWaitMs=" << queueWaitMs;
            if (transportRequest.kind == NavigationKind::Library) {
                return m_runtime.catalog(
                    RustBridgeRuntime::CatalogQuery::Library);
            }
#ifdef YANAMI_ENABLE_DEV_HOOKS
            bool delayIsValid = false;
            const int requestedDelay = DevelopmentHooks::intValue(
                DevelopmentHooks::Variable::CollectionDelayMs,
                &delayIsValid);
            const int developmentDelayMs = delayIsValid
                ? qBound(0, requestedDelay, 30000)
                : 0;
            if (developmentDelayMs > 0) {
                QThread::msleep(
                    static_cast<unsigned long>(developmentDelayMs));
            }
#endif
            return m_runtime.catalog(
                RustBridgeRuntime::CatalogQuery::Collection,
                transportRequest.parentId);
        }));
}

void CatalogCoordinator::finishNavigationQuery(NavigationLane &lane)
{
    if (!lane.active.has_value())
        return;
    const YanamiOperationResult operationResult = lane.watcher.result();
    const NavigationRequest request = std::move(*lane.active);
    const qint64 operationElapsedMs = lane.operationTimer.isValid()
        ? lane.operationTimer.elapsed()
        : -1;
    lane.operationTimer.invalidate();
    lane.active.reset();
    applyNavigationResult(request, operationResult, operationElapsedMs);
    notifyNavigationStateChanged();
    QTimer::singleShot(0, this, [this] {
        pumpNavigationQueries();
        notifyNavigationStateChanged();
    });
}

void CatalogCoordinator::pumpNavigationQueries()
{
    if (m_shuttingDown || m_sessionFenced)
        return;
    for (NavigationLane &lane : m_navigationLanes) {
        if (lane.active.has_value() || lane.watcher.isRunning())
            continue;
        while (!lane.active.has_value()) {
            std::optional<NavigationRequest> request;
            if (m_queuedCollectionNavigation.has_value()) {
                request = std::move(m_queuedCollectionNavigation);
                m_queuedCollectionNavigation.reset();
            } else if (m_queuedLibraryNavigation.has_value()) {
                request = std::move(m_queuedLibraryNavigation);
                m_queuedLibraryNavigation.reset();
            } else {
                break;
            }
            if (!acceptsSession(request->sessionGeneration)
                || !m_requests.acceptsLatest(
                    request->presentationToken,
                    m_committedSession.generation)) {
                qInfo().noquote()
                    << "navigation_query"
                    << "phase=drop"
                    << "lane=" << request->presentationToken.laneKey
                    << "resourceKey=" << request->resourceKey
                    << "reason=stale_before_dispatch";
                continue;
            }
            startNavigationQuery(lane, std::move(*request));
        }
    }
}

void CatalogCoordinator::resetNavigationQueries(const char *reason)
{
    const bool hadVisibleWork = libraryRefreshing() || collectionLoading();
    if (m_queuedLibraryNavigation.has_value()
        || m_queuedCollectionNavigation.has_value()) {
        qInfo().noquote()
            << "navigation_query"
            << "phase=reset"
            << "reason=" << reason;
    }
    m_queuedLibraryNavigation.reset();
    m_queuedCollectionNavigation.reset();
    m_requests.invalidateLatestLane(libraryNavigationLaneKey);
    m_requests.invalidateLatestLane(collectionNavigationLaneKey);
    if (hadVisibleWork)
        notifyNavigationStateChanged();
}

bool CatalogCoordinator::navigationQueryPending(
    NavigationKind kind,
    const QString &parentId,
    bool activeOnly) const
{
    const auto matches = [this, kind, &parentId](
                             const NavigationRequest &request) {
        return request.kind == kind
            && (parentId.isEmpty() || request.parentId == parentId)
            && acceptsSession(request.sessionGeneration)
            && m_requests.acceptsLatest(
                request.presentationToken, m_committedSession.generation);
    };
    for (const NavigationLane &lane : m_navigationLanes) {
        if (lane.active.has_value() && matches(*lane.active))
            return true;
    }
    if (activeOnly)
        return false;
    const std::optional<NavigationRequest> &queued =
        kind == NavigationKind::Library
        ? m_queuedLibraryNavigation
        : m_queuedCollectionNavigation;
    return queued.has_value() && matches(*queued);
}

void CatalogCoordinator::notifyNavigationStateChanged()
{
    emit stateChanged();
}

void CatalogCoordinator::applyNavigationResult(
    const NavigationRequest &request,
    const YanamiOperationResult &operationResult,
    qint64 operationElapsedMs)
{
    const bool currentSession = acceptsSession(request.sessionGeneration);
    const bool resourceCurrent = currentSession
        && m_requests.accepts(
            request.resourceToken, m_committedSession.generation);
    const bool presentationCurrent = currentSession
        && m_requests.acceptsLatest(
            request.presentationToken, m_committedSession.generation);
    const bool currentCollection = presentationCurrent
        && request.kind == NavigationKind::Collection
        && request.parentId == m_collectionTargetId;
    qInfo().noquote()
        << "navigation_query"
        << "phase=finish"
        << "lane=" << request.presentationToken.laneKey
        << "requestId=" << request.presentationToken.requestId
        << "resourceKey=" << request.resourceKey
        << "elapsedMs=" << operationElapsedMs
        << "status=" << operationResult.status
        << "resourceCurrent=" << resourceCurrent
        << "presentationCurrent=" << presentationCurrent;
    if (!resourceCurrent) {
        if (currentSession) {
            if (request.kind == NavigationKind::Library) {
                QTimer::singleShot(0, this, [this, generation = request.sessionGeneration] {
                    if (acceptsSession(generation))
                        loadLibrary();
                });
            } else {
                m_mediaStore->markQueryStale(
                    QStringLiteral("collection"), request.parentId);
                if (request.parentId == m_collectionTargetId) {
                    QTimer::singleShot(
                        0,
                        this,
                        [this, parentId = request.parentId,
                            generation = request.sessionGeneration] {
                            if (acceptsSession(generation))
                                loadCollection(parentId);
                        });
                }
            }
        }
        return;
    }
    if (operationResult.status != 0) {
        if (request.kind == NavigationKind::Library
            && presentationCurrent) {
            m_libraryLoadFailed = true;
            emit stateChanged();
            m_statusSink.publishStatus(
                operationResult.error.isEmpty()
                    ? tr("The operation failed.")
                    : m_statusSink.userFacingBackendError(
                        operationResult.errorCode, operationResult.error),
                true);
        } else if (currentCollection) {
            m_collectionErrorId = request.parentId;
            emit stateChanged();
            m_statusSink.publishStatus(
                operationResult.error.isEmpty()
                    ? tr("The operation failed.")
                    : m_statusSink.userFacingBackendError(
                        operationResult.errorCode, operationResult.error),
                true);
        }
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        operationResult.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (request.kind == NavigationKind::Library
            && presentationCurrent) {
            m_libraryLoadFailed = true;
            emit stateChanged();
            m_statusSink.publishStatus(
                tr("The media library response was invalid."), true);
        } else if (currentCollection) {
            m_collectionErrorId = request.parentId;
            emit stateChanged();
            m_statusSink.publishStatus(
                tr("The collection response was invalid."), true);
        }
        return;
    }
    const QJsonObject object = document.object();
    if (request.kind == NavigationKind::Library) {
        if (!validateResponseSchema(object, "library", presentationCurrent)) {
            if (presentationCurrent) {
                m_libraryLoadFailed = true;
                emit stateChanged();
            }
            return;
        }
        if (!applyLibraryObject(object, request.activityRevision)) {
            if (presentationCurrent) {
                m_libraryLoadFailed = true;
                emit stateChanged();
                m_statusSink.publishStatus(
                    tr("The media library response was invalid."), true);
            }
            return;
        }
        if (m_libraryLoadFailed) {
            m_libraryLoadFailed = false;
            emit stateChanged();
        }
        m_requests.invalidate({activityRequestKey, favoritesRequestKey});
        m_lastFullLibraryRefreshMs = QDateTime::currentMSecsSinceEpoch();
        saveLibraryCache();
        if (presentationCurrent) {
            m_statusSink.publishStatus(
                tr("Loaded %1 libraries, %2 titles and %3 recent episodes.")
                    .arg(m_mediaStore->libraryViewsModel()->rowCount())
                    .arg(m_mediaStore->libraryModel()->rowCount())
                    .arg(m_mediaStore->recentModel()->rowCount()),
                false);
        }
        return;
    }
    if (!validateResponseSchema(object, "collection", currentCollection)) {
        if (currentCollection) {
            m_collectionErrorId = request.parentId;
            emit stateChanged();
        }
        return;
    }
    if (!hasNormalizedQuery(object, QStringLiteral("collection"))) {
        if (currentCollection) {
            m_collectionErrorId = request.parentId;
            emit stateChanged();
            m_statusSink.publishStatus(
                tr("The collection response was invalid."), true);
        }
        return;
    }
    const QVariantMap collectionParent = normalizedQueryParent(
        object, QStringLiteral("collection"));
    const QVariantList collectionItems = normalizedQueryItems(
        object, QStringLiteral("collection"));
    m_requests.invalidate({activityRequestKey, favoritesRequestKey});
    m_mediaStore->setQuery(
        QStringLiteral("collection"),
        request.parentId,
        collectionItems,
        collectionParent);
    if (currentCollection)
        publishCachedCollection(request.parentId);
    saveLibraryCache();
    qInfo().noquote()
        << "collection_response"
        << "parent=" << request.parentId
        << "target=" << m_collectionTargetId
        << "published=" << currentCollection
        << "items=" << collectionItems.size();
    if (currentCollection) {
        if (!m_collectionErrorId.isEmpty()) {
            m_collectionErrorId.clear();
            emit stateChanged();
        }
        m_statusSink.publishStatus(
            tr("Loaded %1 items.").arg(collectionItems.size()), false);
    }
}

void CatalogCoordinator::finishActivityRefresh()
{
    const YanamiOperationResult operationResult = m_activityWatcher.result();
    const qint64 elapsedMs = m_activityTimer.isValid()
        ? m_activityTimer.elapsed()
        : -1;
    m_activityTimer.invalidate();
    const bool currentSession = acceptsSession(m_activitySessionGeneration);
    const char *outcome = "success";
    bool requestSucceeded = false;
    bool requestSuperseded = false;
    if (currentSession) {
        if (!CatalogFreshnessPolicy::activityResultMayAffectState(
                m_requests.accepts(
                    m_activityRequest, m_committedSession.generation),
                m_activityRequestRevision,
                m_activityRevisionCommitted)) {
            outcome = "stale_revision";
            requestSuperseded = true;
            m_activityRefreshQueued = true;
        } else if (operationResult.status != 0) {
            outcome = "backend_error";
            qWarning().noquote()
                << "activity_refresh_failed"
                << "status=" << operationResult.status;
        } else {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(
                operationResult.payload, &parseError);
            if (parseError.error != QJsonParseError::NoError
                || !document.isObject()) {
                outcome = "invalid_json";
                qWarning().noquote()
                    << "activity_refresh_invalid_json"
                    << "parse_error=" << parseError.errorString();
            } else {
                const QJsonObject object = document.object();
                if (!validateResponseSchema(object, "activity", false)) {
                    outcome = "incompatible_schema";
                } else if (!applyActivityObject(
                               object, m_activityRequestRevision)) {
                    outcome = "invalid_shape";
                    qWarning() << "activity_refresh_invalid_shape";
                } else {
                    requestSucceeded = true;
                    saveLibraryCache();
                }
            }
        }
    } else {
        outcome = "stale_session";
    }
    qInfo().noquote()
        << "activity_refresh"
        << "phase=finish"
        << "elapsedMs=" << elapsedMs
        << "status=" << operationResult.status
        << "outcome=" << outcome
        << "sessionGeneration=" << m_activitySessionGeneration;
    if (currentSession && requestSucceeded) {
        m_activityLoadFailed = false;
        m_activityConsecutiveFailures = 0;
        m_activityRetryAfterMs = 0;
    } else if (currentSession && !requestSuperseded) {
        m_activityLoadFailed = true;
        m_activityConsecutiveFailures = qMin(
            m_activityConsecutiveFailures + 1, 5);
        const qint64 retryDelay = qMin(
            activityAutomaticRetryMaxMs,
            activityAutomaticRetryBaseMs
                * (qint64 {1} << (m_activityConsecutiveFailures - 1)));
        m_activityRetryAfterMs = QDateTime::currentMSecsSinceEpoch()
            + retryDelay;
        qWarning().noquote()
            << "activity_refresh_backoff"
            << "failures=" << m_activityConsecutiveFailures
            << "retryDelayMs=" << retryDelay;
    }
    if (activeSession() && m_activityRefreshQueued) {
        m_activityRefreshQueued = false;
        QTimer::singleShot(0, this,
            &CatalogCoordinator::beginActivityRefresh);
        return;
    }
    m_activityRefreshQueued = false;
    if (m_activityRefreshing) {
        m_activityRefreshing = false;
        emit stateChanged();
    }
}

void CatalogCoordinator::finishFavoritesRefresh()
{
    const YanamiOperationResult operationResult = m_favoritesWatcher.result();
    const bool currentSession = acceptsSession(m_favoritesSessionGeneration);
    const char *outcome = "success";
    if (currentSession) {
        if (operationResult.status != 0) {
            outcome = "backend_error";
            m_favoritesLoadFailed = true;
        } else {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(
                operationResult.payload, &parseError);
            if (parseError.error != QJsonParseError::NoError
                || !document.isObject()) {
                outcome = "invalid_json";
                m_favoritesLoadFailed = true;
            } else {
                const QJsonObject object = document.object();
                if (!validateResponseSchema(object, "favorites", false)
                    || !hasNormalizedQuery(
                        object, QStringLiteral("favorites"))) {
                    outcome = "invalid_shape";
                    m_favoritesLoadFailed = true;
                } else if (!m_requests.accepts(
                               m_favoritesRequest,
                               m_committedSession.generation)) {
                    outcome = "stale_revision";
                    m_favoritesRefreshQueued = true;
                } else {
                    const QVariantList favorites = normalizedQueryItems(
                        object, QStringLiteral("favorites"));
                    m_mediaStore->setQuery(
                        QStringLiteral("favorites"), {}, favorites);
                    m_lastFavoritesRefreshMs =
                        QDateTime::currentMSecsSinceEpoch();
                    m_favoritesLoadFailed = false;
                    saveLibraryCache();
                }
            }
        }
    } else {
        outcome = "stale_session";
    }
    qInfo().noquote()
        << "favorites_refresh"
        << "phase=finish"
        << "status=" << operationResult.status
        << "outcome=" << outcome
        << "items="
        << m_mediaStore->favoritesModel()->rowCount()
        << "sessionGeneration=" << m_favoritesSessionGeneration;
    if (activeSession() && m_favoritesRefreshQueued) {
        m_favoritesRefreshQueued = false;
        QTimer::singleShot(0, this,
            [this] { beginFavoritesRefresh(true); });
        return;
    }
    m_favoritesRefreshQueued = false;
    if (m_favoritesRefreshing) {
        m_favoritesRefreshing = false;
        emit stateChanged();
    } else if (currentSession) {
        emit stateChanged();
    }
}

bool CatalogCoordinator::validateResponseSchema(
    const QJsonObject &object,
    const char *responseName,
    bool reportStatus)
{
    const QJsonValue version = object.value(QStringLiteral("schemaVersion"));
    if (version.isDouble()
        && version.toDouble() == static_cast<double>(desktopSchemaVersion)) {
        return true;
    }
    const QString received = schemaVersionLabel(version);
    qWarning().noquote()
        << "backend_schema_incompatible"
        << "response=" << responseName
        << "expected=" << desktopSchemaVersion
        << "received=" << received;
    if (reportStatus) {
        m_statusSink.publishStatus(
            tr("The backend %1 response uses an incompatible schema version "
               "(expected %2, received %3).")
                .arg(QString::fromLatin1(responseName))
                .arg(desktopSchemaVersion)
                .arg(received),
            true);
    }
    return false;
}

bool CatalogCoordinator::applyLibraryObject(
    const QJsonObject &object,
    quint64 activityRevision)
{
    if (!hasNormalizedQuery(object, QStringLiteral("library"))
        || !hasNormalizedQuery(object, QStringLiteral("views"))
        || !hasNormalizedQuery(object, QStringLiteral("resume"))
        || !hasNormalizedQuery(object, QStringLiteral("recent"))
        || !object.value(QStringLiteral("userCapabilities")).isObject()) {
        return false;
    }
    const QVariantList mediaItems = normalizedQueryItems(
        object, QStringLiteral("library"));
    QVariantList libraryViews;
    const QVariantList responseViews = normalizedQueryItems(
        object, QStringLiteral("views"));
    libraryViews.reserve(responseViews.size());
    for (const QVariant &view : responseViews) {
        const QVariantMap values = view.toMap();
        if (values.value(QStringLiteral("collectionType"))
                .toString()
                .compare(QStringLiteral("boxsets"), Qt::CaseInsensitive)
            == 0) {
            continue;
        }
        libraryViews.push_back(view);
    }
    const QJsonObject capabilityObject = object.value(
        QStringLiteral("userCapabilities")).toObject();
    const CatalogUserCapabilities capabilities {
        capabilityObject.value(
            QStringLiteral("isAdministrator")).toBool(),
        capabilityObject.value(QStringLiteral("canDownload")).toBool(),
        capabilityObject.value(QStringLiteral("canDelete")).toBool(),
    };
    const qint64 fetchedAt = QDateTime::currentMSecsSinceEpoch();
    m_mediaStore->setQuery(
        QStringLiteral("library"), {}, mediaItems, {}, fetchedAt);
    m_mediaStore->setQuery(
        QStringLiteral("views"), {}, libraryViews, {}, fetchedAt);
    if (!applyActivityQueries(object, activityRevision))
        return false;
    if (m_capabilities != capabilities) {
        m_capabilities = capabilities;
        publishCapabilities();
    }
    qInfo().noquote()
        << "emby_user_capabilities"
        << "administrator=" << capabilities.administrator
        << "download=" << capabilities.canDownload
        << "delete=" << capabilities.canDelete;
    return true;
}

bool CatalogCoordinator::applyActivityObject(
    const QJsonObject &object,
    quint64 activityRevision)
{
    if (!hasNormalizedQuery(object, QStringLiteral("resume"))
        || !hasNormalizedQuery(object, QStringLiteral("recent"))) {
        return false;
    }
    return applyActivityQueries(object, activityRevision);
}

bool CatalogCoordinator::applyActivityQueries(
    const QJsonObject &object,
    quint64 activityRevision)
{
    if (!CatalogFreshnessPolicy::activitySnapshotMayCommit(
            activityRevision, m_activityRevisionCommitted)) {
        qInfo().noquote()
            << "activity_snapshot"
            << "phase=drop"
            << "revision=" << activityRevision
            << "committedRevision=" << m_activityRevisionCommitted;
        return true;
    }
    const QVariantList resumeItems = normalizedQueryItems(
        object, QStringLiteral("resume"));
    const QVariantList recentItems = normalizedQueryItems(
        object, QStringLiteral("recent"));
    const qint64 fetchedAt = QDateTime::currentMSecsSinceEpoch();
    m_mediaStore->setQuery(
        QStringLiteral("resume"), {}, resumeItems, {}, fetchedAt);
    m_mediaStore->setQuery(
        QStringLiteral("recent"), {}, recentItems, {}, fetchedAt);
    m_activityRevisionCommitted = activityRevision;
    // Library and targeted Activity are both legitimate producers. Any
    // committed snapshot repairs the shared activity resource state; a
    // dropped older snapshot must not clear a newer failure.
    m_activityLoadFailed = false;
    m_activityConsecutiveFailures = 0;
    m_activityRetryAfterMs = 0;
    return true;
}
