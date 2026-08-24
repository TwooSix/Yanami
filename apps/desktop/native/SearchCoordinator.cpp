#include "SearchCoordinator.hpp"

#include "BackendInfrastructure.hpp"
#include "SearchResultRowsModel.hpp"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr int desktopSchemaVersion = 8;
constexpr int hydrationDelayMs = 150;
const QString searchKind = QStringLiteral("search");
const QString searchTitlesKind = QStringLiteral("search-titles");
const QString searchEpisodesKind = QStringLiteral("search-episodes");

struct SearchSections
{
    QVariantList titles;
    QVariantList episodes;
};

SearchSections splitSearchItems(const QVariantList &items)
{
    SearchSections sections;
    sections.titles.reserve(items.size());
    sections.episodes.reserve(items.size());
    for (const QVariant &value : items) {
        const QVariantMap item = value.toMap();
        if (item.value(QStringLiteral("itemType")).toString()
            == QStringLiteral("Episode")) {
            sections.episodes.push_back(value);
        } else {
            // Series and movies are the primary title results. Seasons and
            // unknown future catalog types remain visible as a stable
            // compatibility fallback instead of disappearing.
            sections.titles.push_back(value);
        }
    }
    return sections;
}

QVariantList normalizedSearchItems(const QJsonObject &response)
{
    const QJsonObject entities =
        response.value(QStringLiteral("entities")).toObject();
    const QJsonObject query = response.value(QStringLiteral("queries"))
        .toObject().value(searchKind).toObject();
    const QJsonArray rows = query.value(QStringLiteral("rows")).toArray();
    QVariantList items;
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
        items.push_back(item);
    }
    return items;
}

qint64 jsonCount(
    const QJsonObject &object,
    const QString &key,
    qint64 fallback)
{
    const QJsonValue value = object.value(key);
    return value.isDouble()
        ? static_cast<qint64>(value.toDouble()) : fallback;
}

} // namespace

SearchCoordinator::SearchCoordinator(
    RustBridgeRuntime &runtime,
    QThreadPool &queryPool,
    QThreadPool &hydrationPool,
    SessionStateProvider sessionStateProvider,
    StatusSink &statusSink,
    QObject *parent)
    : SearchCoordinator(
          SearchBackendOperations {
              .ready = [&runtime] { return runtime.ready(); },
              .searchCatalog = [&runtime](const QString &query) {
                  return runtime.searchCatalog(query);
              },
              .hydrateCatalogSearchImages =
                  [&runtime](const QVariantMap &request) {
                      return runtime.hydrateCatalogSearchImages(request);
                  },
          },
          queryPool,
          hydrationPool,
          std::move(sessionStateProvider),
          statusSink,
          {},
          parent)
{
}

SearchCoordinator::SearchCoordinator(
    SearchBackendOperations operations,
    QThreadPool &queryPool,
    QThreadPool &hydrationPool,
    SessionStateProvider sessionStateProvider,
    StatusSink &statusSink,
    EventObserver eventObserver,
    QObject *parent)
    : SearchPort(parent)
    , m_operations(std::move(operations))
    , m_queryPool(queryPool)
    , m_hydrationPool(hydrationPool)
    , m_sessionStateProvider(std::move(sessionStateProvider))
    , m_statusSink(statusSink)
    , m_eventObserver(std::move(eventObserver))
    , m_mediaStore(std::make_unique<MediaStore>())
{
    m_mediaStore->queryModel(searchKind);
    MediaQueryModel *const titleResults =
        m_mediaStore->queryModel(searchTitlesKind);
    MediaQueryModel *const episodeResults =
        m_mediaStore->queryModel(searchEpisodesKind);
    m_resultRowsModel = std::make_unique<SearchResultRowsModel>(
        titleResults, episodeResults);
    m_hydrationDelay.setSingleShot(true);
    m_hydrationDelay.setInterval(hydrationDelayMs);
    connect(&m_watcher, &QFutureWatcher<YanamiOperationResult>::finished,
        this, &SearchCoordinator::finish);
    connect(&m_statusWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this, &SearchCoordinator::finishStatusPoll);
    connect(&m_hydrationWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this, &SearchCoordinator::finishHydration);
    connect(&m_hydrationDelay, &QTimer::timeout,
        this, &SearchCoordinator::hydrationDelayElapsed);
}

SearchCoordinator::~SearchCoordinator()
{
    shutdown();
    drain();
}

bool SearchCoordinator::initializeFromSession()
{
    if (m_initialized)
        return activeSession();
    m_initialized = true;
    m_committedSession = currentSession();
    m_sessionFenced = false;
    emit stateChanged();
    return activeSession();
}

void SearchCoordinator::sessionTransitionStarted()
{
    if (m_shuttingDown)
        return;
    m_sessionFenced = true;
    ++m_requestGeneration;
    ++m_statusGeneration;
    if (m_queuedRequest.has_value())
        trace(QStringLiteral("request_discarded"), *m_queuedRequest,
            QStringLiteral("session_transition_started"));
    m_queuedRequest.reset();
    cancelPendingHydration();
    updateSearching();
}

void SearchCoordinator::sessionTransitionAborted()
{
    if (m_shuttingDown)
        return;
    m_sessionFenced = false;
    if (activeSession()) {
        enqueue(m_query, false);
        return;
    }
    updateSearching();
    emit stateChanged();
}

void SearchCoordinator::sessionCommitted()
{
    if (m_shuttingDown)
        return;
    m_committedSession = currentSession();
    m_sessionFenced = false;
    ++m_requestGeneration;
    ++m_statusGeneration;
    if (m_queuedRequest.has_value())
        trace(QStringLiteral("request_discarded"), *m_queuedRequest,
            QStringLiteral("session_committed"));
    m_queuedRequest.reset();
    cancelPendingHydration();
    resetState();
    emit stateChanged();
}

void SearchCoordinator::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_sessionFenced = true;
    ++m_requestGeneration;
    ++m_statusGeneration;
    if (m_queuedRequest.has_value())
        trace(QStringLiteral("request_discarded"), *m_queuedRequest,
            QStringLiteral("shutdown"));
    m_queuedRequest.reset();
    cancelPendingHydration();
    updateSearching();
}

void SearchCoordinator::drain()
{
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
    if (m_statusWatcher.isRunning())
        m_statusWatcher.waitForFinished();
    if (m_hydrationWatcher.isRunning())
        m_hydrationWatcher.waitForFinished();
}

MediaQueryModel *SearchCoordinator::resultsModel() const
{
    return m_mediaStore->queryModel(searchKind);
}

MediaQueryModel *SearchCoordinator::titleResultsModel() const
{
    return m_mediaStore->queryModel(searchTitlesKind);
}

MediaQueryModel *SearchCoordinator::episodeResultsModel() const
{
    return m_mediaStore->queryModel(searchEpisodesKind);
}

QAbstractItemModel *SearchCoordinator::resultRowsModel() const
{
    return m_resultRowsModel.get();
}

void SearchCoordinator::requestSearch(const QString &query)
{
    enqueue(query.trimmed(), true);
}

void SearchCoordinator::inputPending()
{
    const auto currentIdentity = [this](const Request &request) {
        return request.sessionGeneration == m_committedSession.generation
            && request.requestGeneration == m_requestGeneration;
    };
    const auto currentRequest = [&currentIdentity](
                                    const std::optional<Request> &request) {
        return request.has_value() && currentIdentity(*request);
    };
    const bool currentStatus = m_activeStatusRequest.has_value()
        && m_activeStatusRequest->sessionGeneration == m_committedSession.generation
        && m_activeStatusRequest->requestGeneration == m_requestGeneration
        && m_activeStatusRequest->statusGeneration == m_statusGeneration;
    const bool currentHydration =
        (m_activeHydration.has_value()
            && currentIdentity(m_activeHydration->identity))
        || (m_pendingHydration.has_value()
            && currentIdentity(m_pendingHydration->identity));
    if (!currentRequest(m_activeRequest)
        && !currentRequest(m_queuedRequest)
        && !currentStatus && !currentHydration) {
        return;
    }

    if (currentRequest(m_queuedRequest)) {
        trace(QStringLiteral("request_discarded"), *m_queuedRequest,
            QStringLiteral("input_pending"));
    }
    m_queuedRequest.reset();
    cancelPendingHydration();
    ++m_requestGeneration;
    ++m_statusGeneration;
    const bool wasSearching = m_searching;
    updateSearching();
    if (wasSearching != m_searching)
        emit stateChanged();
}

void SearchCoordinator::refresh()
{
    const auto isCurrentQuery = [this](const std::optional<Request> &request) {
        return request.has_value()
            && request->query == m_query
            && request->sessionGeneration == m_committedSession.generation
            && request->requestGeneration == m_requestGeneration;
    };
    if (!activeSession() || m_query.isEmpty()
        || isCurrentQuery(m_activeRequest)
        || isCurrentQuery(m_queuedRequest)
        || m_activeStatusRequest.has_value()) {
        return;
    }

    // A query without a committed revision is either the first request or a
    // failed request. Retrying it directly is the only way to establish the
    // baseline that makes subsequent status polls cheap.
    if (m_publishedQuery != m_query
        || m_publishedCatalogRevision.isEmpty()) {
        enqueue(m_query, false);
        return;
    }

    startStatusPoll(StatusRequest {
        .query = m_query,
        .catalogRevision = m_publishedCatalogRevision,
        .sessionGeneration = m_committedSession.generation,
        .requestGeneration = m_requestGeneration,
        .statusGeneration = ++m_statusGeneration,
    });
}

SearchSessionState SearchCoordinator::currentSession() const
{
    return m_sessionStateProvider
        ? m_sessionStateProvider() : SearchSessionState {};
}

bool SearchCoordinator::activeSession() const
{
    if (!m_initialized || m_shuttingDown || m_sessionFenced
        || !m_operations.ready || !m_operations.ready()
        || !m_committedSession.connected) {
        return false;
    }
    const SearchSessionState current = currentSession();
    return current.connected
        && current.generation == m_committedSession.generation;
}

bool SearchCoordinator::accepts(const Request &request) const
{
    return activeSession()
        && request.sessionGeneration == m_committedSession.generation
        && request.requestGeneration == m_requestGeneration;
}

bool SearchCoordinator::accepts(const StatusRequest &request) const
{
    return activeSession()
        && request.sessionGeneration == m_committedSession.generation
        && request.requestGeneration == m_requestGeneration
        && request.statusGeneration == m_statusGeneration
        && request.query == m_query
        && request.query == m_publishedQuery
        && request.catalogRevision == m_publishedCatalogRevision;
}

void SearchCoordinator::resetState()
{
    m_query.clear();
    m_publishedQuery.clear();
    m_publishedCatalogRevision.clear();
    m_error.clear();
    m_cachedCount = 0;
    m_totalCount = -1;
    m_totalMatches = 0;
    m_hasMore = false;
    m_syncing = false;
    m_complete = false;
    m_indexError = false;
    m_indexErrorDetail.clear();
    m_statusFailureReported = false;
    m_mediaStore->clear();
    m_mediaStore->setQuery(searchKind, {}, {});
    m_mediaStore->setQuery(searchTitlesKind, {}, {});
    m_mediaStore->setQuery(searchEpisodesKind, {}, {});
    updateSearching();
}

void SearchCoordinator::enqueue(QString query, bool clearForEmptyQuery)
{
    cancelPendingHydration();
    if (!activeSession()) {
        if (!query.isEmpty()) {
            m_error = tr("Search requires an active Emby connection.");
            emit stateChanged();
        }
        return;
    }

    m_query = std::move(query);
    const Request request {
        .query = m_query,
        .sessionGeneration = m_committedSession.generation,
        .requestGeneration = ++m_requestGeneration,
    };
    m_error.clear();
    if (clearForEmptyQuery && m_query.isEmpty()) {
        m_totalMatches = 0;
        m_hasMore = false;
        m_publishedQuery.clear();
        m_publishedCatalogRevision.clear();
        m_mediaStore->setQuery(searchKind, {}, {});
        m_mediaStore->setQuery(searchTitlesKind, {}, {});
        m_mediaStore->setQuery(searchEpisodesKind, {}, {});
    }
    // The future may be finished while its queued `finished` signal has not
    // run yet. The request identity, not QFutureWatcher::isRunning(), is the
    // authority that prevents a new future from overwriting that completion.
    if (m_activeRequest.has_value()) {
        if (m_queuedRequest.has_value()) {
            trace(QStringLiteral("request_discarded"), *m_queuedRequest,
                QStringLiteral("superseded_in_queue"));
        }
        m_queuedRequest = request;
        trace(QStringLiteral("query_submitted"), request);
        updateSearching();
        emit stateChanged();
        return;
    }
    trace(QStringLiteral("query_submitted"), request);
    start(request);
}

void SearchCoordinator::start(Request request)
{
    if (!activeSession())
        return;
    Q_ASSERT(!m_activeRequest.has_value());
    Q_ASSERT(!m_watcher.isRunning());
    m_activeRequest = std::move(request);
    updateSearching();
    const Request identity = *m_activeRequest;
    const auto searchOperation = m_operations.searchCatalog;
    const EventObserver observer = m_eventObserver;
    SearchCoordinatorEvent workerEvent {
        .query = identity.query,
        .sessionGeneration = identity.sessionGeneration,
        .requestGeneration = identity.requestGeneration,
        .activeCount = 1,
        .queuedCount = m_queuedRequest.has_value() ? 1 : 0,
    };
    m_watcher.setFuture(QtConcurrent::run(
        &m_queryPool,
        [searchOperation, observer, workerEvent]() mutable {
            workerEvent.milestone = QStringLiteral("worker_started");
            if (observer)
                observer(workerEvent);
            YanamiOperationResult result;
            if (searchOperation) {
                result = searchOperation(workerEvent.query);
            } else {
                result.status = 1;
                result.errorCode = QStringLiteral("unavailable");
                result.error = QStringLiteral("catalog search operation is unavailable");
            }
            workerEvent.milestone = QStringLiteral("worker_finished");
            if (observer)
                observer(workerEvent);
            return result;
        }));
    emit stateChanged();
}

void SearchCoordinator::finish()
{
    const YanamiOperationResult result = m_watcher.result();
    const std::optional<Request> completed =
        std::exchange(m_activeRequest, std::nullopt);
    if (completed.has_value() && accepts(*completed)) {
        const bool applied = applyResponse(*completed, result);
        if (!applied && completed->query != m_publishedQuery) {
            m_mediaStore->setQuery(searchKind, {}, {});
            m_mediaStore->setQuery(searchTitlesKind, {}, {});
            m_mediaStore->setQuery(searchEpisodesKind, {}, {});
            m_publishedQuery.clear();
            m_publishedCatalogRevision.clear();
            m_totalMatches = 0;
            m_hasMore = false;
        }
    } else if (completed.has_value()) {
        trace(QStringLiteral("request_discarded"), *completed,
            QStringLiteral("stale_completion"));
    }

    if (m_queuedRequest.has_value() && activeSession()) {
        Request next = std::move(*m_queuedRequest);
        m_queuedRequest.reset();
        start(std::move(next));
        return;
    }
    m_queuedRequest.reset();
    updateSearching();
    emit stateChanged();
}

void SearchCoordinator::startStatusPoll(StatusRequest request)
{
    if (!activeSession())
        return;
    Q_ASSERT(!m_activeStatusRequest.has_value());
    Q_ASSERT(!m_statusWatcher.isRunning());
    m_activeStatusRequest = std::move(request);
    const auto searchOperation = m_operations.searchCatalog;
    m_statusWatcher.setFuture(QtConcurrent::run(
        &m_queryPool,
        [searchOperation] {
            if (searchOperation)
                return searchOperation(QString());
            YanamiOperationResult result;
            result.status = 1;
            result.errorCode = QStringLiteral("unavailable");
            result.error = QStringLiteral(
                "catalog search status operation is unavailable");
            return result;
        }));
}

void SearchCoordinator::finishStatusPoll()
{
    const YanamiOperationResult result = m_statusWatcher.result();
    const std::optional<StatusRequest> completed =
        std::exchange(m_activeStatusRequest, std::nullopt);
    if (!completed.has_value() || !accepts(*completed))
        return;

    // Status polling is deliberately non-destructive. A transient failure or
    // incompatible payload leaves the committed rows, error copy, and pending
    // image hydration untouched; the next timer tick can try again.
    if (result.status != 0) {
        if (result.errorCode != QStringLiteral("cancelled")
            && !m_statusFailureReported) {
            qWarning().noquote()
                << "catalog_search_status_failed"
                << "code=" << result.errorCode
                << "message=" << result.error;
        }
        if (result.errorCode != QStringLiteral("cancelled"))
            m_statusFailureReported = true;
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        result.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (!m_statusFailureReported) {
            qWarning().noquote()
                << "catalog_search_status_dropped"
                << "reason=invalid_json";
        }
        m_statusFailureReported = true;
        return;
    }
    const QJsonObject object = document.object();
    const QJsonValue statusValue = object.value(
        QStringLiteral("searchStatus"));
    if (object.value(QStringLiteral("schemaVersion")).toInt(-1)
            != desktopSchemaVersion
        || !statusValue.isObject()) {
        if (!m_statusFailureReported) {
            qWarning().noquote()
                << "catalog_search_status_dropped"
                << "reason=incompatible_response";
        }
        m_statusFailureReported = true;
        return;
    }
    const QJsonObject status = statusValue.toObject();
    const QJsonValue revisionValue = status.value(
        QStringLiteral("catalogRevision"));
    if (status.value(QStringLiteral("query")).toString() != QString()
        || !revisionValue.isString()
        || revisionValue.toString().isEmpty()) {
        if (!m_statusFailureReported) {
            qWarning().noquote()
                << "catalog_search_status_dropped"
                << "reason=invalid_status";
        }
        m_statusFailureReported = true;
        return;
    }

    m_statusFailureReported = false;
    const QString revision = revisionValue.toString();

    // The revision describes searchable content, while these fields also
    // describe the indexer's lifecycle. Apply lifecycle-only transitions even
    // when no searchable row changed; this keeps the progress/error copy
    // honest without touching models or image hydration.
    const qint64 cachedCount = jsonCount(
        status, QStringLiteral("cachedCount"), 0);
    const qint64 totalCount = status.value(
        QStringLiteral("totalCount")).isNull()
        ? -1 : jsonCount(status, QStringLiteral("totalCount"), -1);
    const bool complete = status.value(
        QStringLiteral("complete")).toBool(false);
    const bool syncing = status.value(
        QStringLiteral("syncing")).toBool(false);
    const bool indexError = status.value(
        QStringLiteral("indexError")).toBool(false);
    bool statusChanged = cachedCount != m_cachedCount
        || totalCount != m_totalCount
        || complete != m_complete
        || syncing != m_syncing;
    m_cachedCount = cachedCount;
    m_totalCount = totalCount;
    m_complete = complete;
    m_syncing = syncing;
    statusChanged = applyIndexErrorState(indexError,
        status.value(QStringLiteral("indexErrorDetail")).toString())
        || statusChanged;
    if (statusChanged)
        emit stateChanged();
    if (revision == completed->catalogRevision)
        return;

    // enqueue() is the existing latest-wins path. It is intentionally reached
    // only after a revision change, so an unchanged poll cannot republish rows
    // or cancel/schedule hydration.
    enqueue(completed->query, false);
}

bool SearchCoordinator::applyIndexErrorState(
    bool indexError,
    const QString &detail)
{
    const QString normalizedDetail = indexError
        ? detail.trimmed().left(1'024) : QString();
    const bool changed = indexError != m_indexError
        || normalizedDetail != m_indexErrorDetail;
    if (indexError
        && (!m_indexError || normalizedDetail != m_indexErrorDetail)) {
        qWarning().noquote()
            << "catalog_index_retry_scheduled"
            << "cached=" << m_cachedCount
            << "detail=" << (normalizedDetail.isEmpty()
                ? QStringLiteral("unavailable") : normalizedDetail);
    } else if (!indexError && m_indexError) {
        qInfo().noquote()
            << "catalog_index_retry_started"
            << "cached=" << m_cachedCount;
    }

    const QString indexErrorText = tr(
        "Background catalog indexing was interrupted and will retry automatically.");
    if (indexError)
        m_error = indexErrorText;
    else if (m_error == indexErrorText)
        m_error.clear();
    m_indexError = indexError;
    m_indexErrorDetail = normalizedDetail;
    return changed;
}

void SearchCoordinator::cancelPendingHydration()
{
    m_hydrationDelay.stop();
    m_pendingHydration.reset();
}

void SearchCoordinator::scheduleHydration(
    const Request &request,
    const QVariantList &items)
{
    cancelPendingHydration();
    if (request.query.isEmpty() || !accepts(request)
        || request.query != m_publishedQuery) {
        return;
    }
    QVariantList hydrationItems;
    hydrationItems.reserve(items.size());
    QSet<QString> hydrationKeys;
    for (const QVariant &value : items) {
        const QVariantMap item = value.toMap();
        QString itemId = item.value(
            QStringLiteral("imageItemId")).toString();
        if (itemId.isEmpty()) {
            // Preserve compatibility with a response produced immediately
            // before image owners became explicit.
            itemId = item.value(QStringLiteral("id")).toString();
        }
        QString itemType = item.value(
            QStringLiteral("imageItemType")).toString();
        if (itemType.isEmpty()) {
            // Responses produced before image owners became explicit only
            // carry the media item's own type.
            itemType = item.value(QStringLiteral("itemType")).toString();
        }
        const QString imageTag = item.value(
            QStringLiteral("imageTag")).toString();
        if (itemId.isEmpty() || itemType.isEmpty() || imageTag.isEmpty())
            continue;
        const QString hydrationKey = itemId + QChar(0x1f)
            + itemType + QChar(0x1f) + imageTag;
        if (hydrationKeys.contains(hydrationKey))
            continue;
        hydrationKeys.insert(hydrationKey);
        hydrationItems.push_back(QVariantMap {
            {QStringLiteral("itemId"), itemId},
            {QStringLiteral("itemType"), itemType},
            {QStringLiteral("imageTag"), imageTag},
        });
    }
    if (hydrationItems.isEmpty())
        return;
    m_pendingHydration = HydrationRequest {
        .identity = request,
        .payload = {
            {QStringLiteral("query"), request.query},
            {QStringLiteral("items"), hydrationItems},
        },
    };
    m_hydrationDelay.start();
}

void SearchCoordinator::hydrationDelayElapsed()
{
    if (!m_pendingHydration.has_value())
        return;
    if (m_activeHydration.has_value())
        return;
    HydrationRequest request = std::move(*m_pendingHydration);
    m_pendingHydration.reset();
    startHydration(std::move(request));
}

void SearchCoordinator::startHydration(HydrationRequest request)
{
    if (!accepts(request.identity) || request.identity.query.isEmpty()
        || request.identity.query != m_publishedQuery) {
        return;
    }
    Q_ASSERT(!m_activeHydration.has_value());
    Q_ASSERT(!m_hydrationWatcher.isRunning());
    m_activeHydration = std::move(request);
    const QVariantMap payload = m_activeHydration->payload;
    const auto hydrationOperation = m_operations.hydrateCatalogSearchImages;
    m_hydrationWatcher.setFuture(QtConcurrent::run(
        &m_hydrationPool,
        [hydrationOperation, payload] {
            if (hydrationOperation)
                return hydrationOperation(payload);
            YanamiOperationResult result;
            result.status = 1;
            result.errorCode = QStringLiteral("unavailable");
            result.error = QStringLiteral(
                "catalog search image hydration is unavailable");
            return result;
        }));
}

void SearchCoordinator::finishHydration()
{
    const YanamiOperationResult result = m_hydrationWatcher.result();
    const std::optional<HydrationRequest> completed =
        std::exchange(m_activeHydration, std::nullopt);
    if (completed.has_value() && accepts(completed->identity)
        && completed->identity.query == m_publishedQuery) {
        if (result.status == 0) {
            qInfo().noquote()
                << "catalog_search_image_hydration"
                << "queryLength=" << completed->identity.query.size()
                << "status=scheduled";
        } else if (result.errorCode != QStringLiteral("cancelled")) {
            qWarning().noquote()
                << "catalog_search_image_hydration_failed"
                << "queryLength=" << completed->identity.query.size()
                << "code=" << result.errorCode
                << "message=" << result.error;
        }
    }

    if (m_pendingHydration.has_value()
        && !m_hydrationDelay.isActive()) {
        hydrationDelayElapsed();
    }
}

bool SearchCoordinator::applyResponse(
    const Request &request,
    const YanamiOperationResult &result)
{
    if (result.status != 0) {
        m_error = result.error.isEmpty()
            ? tr("Search could not be completed.")
            : m_statusSink.userFacingBackendError(
                  result.errorCode, result.error);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        result.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        m_error = tr("The search response was invalid.");
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schemaVersion")).toInt(-1)
            != desktopSchemaVersion
        || !object.value(QStringLiteral("entities")).isObject()
        || !object.value(QStringLiteral("queries")).toObject()
                .value(searchKind).isObject()
        || !object.value(QStringLiteral("searchStatus")).isObject()) {
        m_error = tr("The search response was incompatible.");
        return false;
    }

    const QJsonObject status = object.value(
        QStringLiteral("searchStatus")).toObject();
    const QJsonValue revisionValue = status.value(
        QStringLiteral("catalogRevision"));
    const QString responseQuery = status.value(
        QStringLiteral("query")).toString();
    if (responseQuery != request.query
        || !revisionValue.isString()
        || revisionValue.toString().isEmpty()) {
        m_error = tr("The search response was invalid.");
        qWarning().noquote()
            << "search_response_dropped"
            << "reason=query_mismatch"
            << "requested=" << request.query
            << "received=" << responseQuery;
        return false;
    }

    const QVariantList items = normalizedSearchItems(object);
    const SearchSections sections = splitSearchItems(items);
    const qint64 fetchedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_mediaStore->setQuery(
        searchKind, {}, request.query.isEmpty() ? QVariantList {} : items,
        {}, fetchedAtMs);
    m_mediaStore->setQuery(
        searchTitlesKind, {},
        request.query.isEmpty() ? QVariantList {} : sections.titles,
        {}, fetchedAtMs);
    m_mediaStore->setQuery(
        searchEpisodesKind, {},
        request.query.isEmpty() ? QVariantList {} : sections.episodes,
        {}, fetchedAtMs);
    m_publishedQuery = request.query;
    m_publishedCatalogRevision = revisionValue.toString();
    m_statusFailureReported = false;
    m_cachedCount = jsonCount(status, QStringLiteral("cachedCount"), 0);
    m_totalCount = status.value(QStringLiteral("totalCount")).isNull()
        ? -1 : jsonCount(status, QStringLiteral("totalCount"), -1);
    m_totalMatches = request.query.isEmpty()
        ? 0 : jsonCount(status, QStringLiteral("totalMatches"), items.size());
    m_hasMore = !request.query.isEmpty()
        && status.value(QStringLiteral("hasMore")).toBool(false);
    m_complete = status.value(QStringLiteral("complete")).toBool(false);
    m_syncing = status.value(QStringLiteral("syncing")).toBool(false);
    m_error.clear();
    applyIndexErrorState(
        status.value(QStringLiteral("indexError")).toBool(false),
        status.value(QStringLiteral("indexErrorDetail")).toString());
    qInfo().noquote()
        << "catalog_search"
        << "queryLength=" << request.query.size()
        << "results=" << items.size()
        << "totalMatches=" << m_totalMatches
        << "cached=" << m_cachedCount
        << "complete=" << m_complete
        << "syncing=" << m_syncing
        << "indexError=" << m_indexError;
    QStringList publishedItemIds;
    publishedItemIds.reserve(items.size());
    for (const QVariant &value : items) {
        const QString itemId = value.toMap().value(
            QStringLiteral("id")).toString();
        if (!itemId.isEmpty())
            publishedItemIds.push_back(itemId);
    }
    trace(QStringLiteral("publish_committed"), request, {}, publishedItemIds);
    scheduleHydration(request, items);
    emit stateChanged();
    return true;
}

void SearchCoordinator::updateSearching()
{
    const auto isCurrentSearch = [this](const std::optional<Request> &request) {
        return activeSession()
            && request.has_value()
            && !request->query.isEmpty()
            && request->sessionGeneration == m_committedSession.generation
            && request->requestGeneration == m_requestGeneration;
    };
    m_searching = isCurrentSearch(m_activeRequest)
        || isCurrentSearch(m_queuedRequest);
}

void SearchCoordinator::trace(
    const QString &milestone,
    const Request &request,
    const QString &reason,
    const QStringList &publishedItemIds) const
{
    if (!m_eventObserver)
        return;
    m_eventObserver(SearchCoordinatorEvent {
        .milestone = milestone,
        .reason = reason,
        .query = request.query,
        .sessionGeneration = request.sessionGeneration,
        .requestGeneration = request.requestGeneration,
        .activeCount = m_activeRequest.has_value() ? 1 : 0,
        .queuedCount = m_queuedRequest.has_value() ? 1 : 0,
        .publishedItemIds = publishedItemIds,
    });
}
