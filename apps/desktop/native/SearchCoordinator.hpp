#pragma once

#include "BackendPorts.hpp"
#include "MediaStore.hpp"
#include "RustBridgeRuntime.hpp"

#include <QFutureWatcher>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class StatusSink;
class SearchResultRowsModel;

struct SearchSessionState
{
    quint64 generation = 0;
    bool connected = false;
};

// A narrow callable boundary keeps the coordinator's queue/session semantics
// executable in a deterministic harness without replacing its production
// state machine. The ordinary desktop constructor binds these operations to
// RustBridgeRuntime.
struct SearchBackendOperations
{
    std::function<bool()> ready;
    std::function<YanamiOperationResult(const QString &)> searchCatalog;
    std::function<YanamiOperationResult(const QVariantMap &)>
        hydrateCatalogSearchImages;
};

struct SearchCoordinatorEvent
{
    QString milestone;
    QString reason;
    QString query;
    quint64 sessionGeneration = 0;
    quint64 requestGeneration = 0;
    int activeCount = 0;
    int queuedCount = 0;
    QStringList publishedItemIds;
};

// Owns the bounded desktop projection of the on-disk catalog search. The full
// catalog never crosses this boundary: Rust/SQLite returns only the current
// Top-K page and this coordinator publishes that page through MediaStore.
class SearchCoordinator final : public SearchPort
{
    Q_OBJECT

public:
    using SessionStateProvider = std::function<SearchSessionState()>;
    // Worker lifecycle events can arrive from the query lane; observers must
    // be thread-safe and must not re-enter the coordinator.
    using EventObserver = std::function<void(const SearchCoordinatorEvent &)>;

    SearchCoordinator(
        RustBridgeRuntime &runtime,
        QThreadPool &queryPool,
        QThreadPool &hydrationPool,
        SessionStateProvider sessionStateProvider,
        StatusSink &statusSink,
        QObject *parent = nullptr);
    SearchCoordinator(
        SearchBackendOperations operations,
        QThreadPool &queryPool,
        QThreadPool &hydrationPool,
        SessionStateProvider sessionStateProvider,
        StatusSink &statusSink,
        EventObserver eventObserver,
        QObject *parent = nullptr);
    ~SearchCoordinator() override;

    bool initializeFromSession();
    void sessionTransitionStarted();
    void sessionTransitionAborted();
    void sessionCommitted();
    void shutdown();
    void drain();

    MediaQueryModel *resultsModel() const override;
    MediaQueryModel *titleResultsModel() const override;
    MediaQueryModel *episodeResultsModel() const override;
    QAbstractItemModel *resultRowsModel() const override;
    QString query() const override { return m_query; }
    bool searching() const override { return m_searching; }
    bool syncing() const override { return m_syncing; }
    bool complete() const override { return m_complete; }
    qint64 cachedCount() const override { return m_cachedCount; }
    qint64 totalCount() const override { return m_totalCount; }
    qint64 totalMatches() const override { return m_totalMatches; }
    bool hasMore() const override { return m_hasMore; }
    QString error() const override { return m_error; }

    void inputPending() override;
    void requestSearch(const QString &query) override;
    void refresh() override;

private:
    struct Request
    {
        QString query;
        quint64 sessionGeneration = 0;
        quint64 requestGeneration = 0;
    };

    struct HydrationRequest
    {
        Request identity;
        QVariantMap payload;
    };

    struct StatusRequest
    {
        QString query;
        QString catalogRevision;
        quint64 sessionGeneration = 0;
        quint64 requestGeneration = 0;
        quint64 statusGeneration = 0;
    };

    SearchSessionState currentSession() const;
    bool activeSession() const;
    bool accepts(const Request &request) const;
    bool accepts(const StatusRequest &request) const;
    void resetState();
    void enqueue(QString query, bool clearForEmptyQuery);
    void start(Request request);
    void finish();
    void startStatusPoll(StatusRequest request);
    void finishStatusPoll();
    bool applyIndexErrorState(bool indexError, const QString &detail);
    void cancelPendingHydration();
    void scheduleHydration(
        const Request &request,
        const QVariantList &items);
    void hydrationDelayElapsed();
    void startHydration(HydrationRequest request);
    void finishHydration();
    bool applyResponse(
        const Request &request,
        const YanamiOperationResult &result);
    void updateSearching();
    void trace(
        const QString &milestone,
        const Request &request,
        const QString &reason = {},
        const QStringList &publishedItemIds = {}) const;

    SearchBackendOperations m_operations;
    QThreadPool &m_queryPool;
    QThreadPool &m_hydrationPool;
    SessionStateProvider m_sessionStateProvider;
    StatusSink &m_statusSink;
    EventObserver m_eventObserver;
    std::unique_ptr<MediaStore> m_mediaStore;
    std::unique_ptr<SearchResultRowsModel> m_resultRowsModel;
    QFutureWatcher<YanamiOperationResult> m_watcher;
    QFutureWatcher<YanamiOperationResult> m_statusWatcher;
    QFutureWatcher<YanamiOperationResult> m_hydrationWatcher;
    QTimer m_hydrationDelay;
    std::optional<Request> m_activeRequest;
    std::optional<Request> m_queuedRequest;
    std::optional<StatusRequest> m_activeStatusRequest;
    std::optional<HydrationRequest> m_activeHydration;
    std::optional<HydrationRequest> m_pendingHydration;
    SearchSessionState m_committedSession;
    QString m_query;
    QString m_publishedQuery;
    QString m_publishedCatalogRevision;
    QString m_error;
    qint64 m_cachedCount = 0;
    qint64 m_totalCount = -1;
    qint64 m_totalMatches = 0;
    quint64 m_requestGeneration = 0;
    quint64 m_statusGeneration = 0;
    bool m_searching = false;
    bool m_syncing = false;
    bool m_complete = false;
    bool m_hasMore = false;
    bool m_indexError = false;
    QString m_indexErrorDetail;
    bool m_statusFailureReported = false;
    bool m_initialized = false;
    bool m_sessionFenced = true;
    bool m_shuttingDown = false;
};
