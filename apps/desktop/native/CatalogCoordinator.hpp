#pragma once

#include "BackendPorts.hpp"
#include "MediaStore.hpp"
#include "RequestCoordinator.hpp"
#include "RustBridgeRuntime.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QSet>
#include <QThreadPool>

#include <array>
#include <functional>
#include <memory>
#include <optional>

class StatusSink;
class QJsonObject;

struct CatalogUserCapabilities
{
    bool administrator = false;
    bool canDownload = false;
    bool canDelete = false;

    friend bool operator==(
        const CatalogUserCapabilities &left,
        const CatalogUserCapabilities &right) = default;
};

struct CatalogSessionState
{
    quint64 generation = 0;
    bool connected = false;
    QString displayName;
    QString serverUrl;
    QString userName;
    QString serverDomain;
    CatalogUserCapabilities capabilities;
};

// The only write surface exposed to MediaCoordinator. It deliberately omits
// catalog navigation, cache files, request lanes and the MediaStore itself.
class CatalogMutationSink
{
public:
    virtual ~CatalogMutationSink() = default;

    virtual QString beginOptimisticStateMutation(
        const QString &itemId,
        MediaPort::Operation operation,
        const QVariantMap &payload,
        quint64 submissionSequence) = 0;
    virtual bool applyAuthoritativeStates(const QVariantList &states) = 0;
    virtual void rollbackOptimisticStateMutation(
        const QString &mutationId) = 0;
    virtual void commitOptimisticStateMutation(
        const QString &mutationId) = 0;
    virtual void applyEntityPatch(const QVariantMap &item) = 0;
    virtual void applyInvalidationEvent(const QVariantMap &event) = 0;
    virtual void applyPendingMetadataPatch(
        const QString &itemId,
        const QVariantMap &payload) = 0;
    virtual void applyContainerMutation(
        const QString &itemId,
        MediaPort::Operation operation,
        const QVariantMap &result) = 0;
    virtual void beginRefreshProtection(const QString &itemId) = 0;
    virtual void endRefreshProtection(const QString &itemId) = 0;
    virtual void invalidatePresentationCache() = 0;
    virtual void scheduleContentReconciliation(const QString &reason) = 0;
};

class CatalogCoordinator final
    : public CatalogPort
    , public CatalogMutationSink
{
    Q_OBJECT

public:
    using SessionStateProvider = std::function<CatalogSessionState()>;
    using CapabilitySink = std::function<void(
        quint64 sessionGeneration,
        const CatalogUserCapabilities &capabilities)>;

    CatalogCoordinator(
        RustBridgeRuntime &runtime,
        QThreadPool &navigationPool,
        SessionStateProvider sessionStateProvider,
        StatusSink &statusSink,
        QString cacheDirectory,
        CapabilitySink capabilitySink,
        QObject *parent = nullptr);
    ~CatalogCoordinator() override;

    bool initializeFromSession();
    void sessionTransitionStarted(
        const char *reason = "session_transition");
    void sessionTransitionAborted();
    void sessionCommitted();
    void shutdown();
    void drain();

    MediaStore *mediaStore() const override { return m_mediaStore.get(); }
    bool libraryRefreshing() const override;
    bool activityRefreshing() const override { return m_activityRefreshing; }
    bool collectionLoading() const override;
    bool collectionFetching() const override;
    bool libraryLoadFailed() const override { return m_libraryLoadFailed; }
    bool activityLoadFailed() const override { return m_activityLoadFailed; }
    bool favoritesRefreshing() const override { return m_favoritesRefreshing; }
    bool favoritesLoadFailed() const override { return m_favoritesLoadFailed; }
    QString collectionDisplayedId() const override
    { return m_collectionDisplayedId; }
    QString collectionTargetId() const override { return m_collectionTargetId; }
    QString collectionErrorId() const override { return m_collectionErrorId; }
    QVariantMap collectionParent() const override;

    RequestDisposition loadLibrary() override;
    void invalidateActivity() override;
    void invalidateSeriesContinue(const QString &seriesId) override;
    RequestDisposition ensureActivityFresh() override;
    RequestDisposition refreshActivity() override;
    RequestDisposition loadFavorites() override;
    RequestDisposition refreshFavorites() override;
    RequestDisposition loadCollection(const QString &parentId) override;
    RequestDisposition refreshCollection(const QString &parentId) override;

    QString beginOptimisticStateMutation(
        const QString &itemId,
        MediaPort::Operation operation,
        const QVariantMap &payload,
        quint64 submissionSequence) override;
    bool applyAuthoritativeStates(const QVariantList &states) override;
    void rollbackOptimisticStateMutation(
        const QString &mutationId) override;
    void commitOptimisticStateMutation(
        const QString &mutationId) override;
    void applyEntityPatch(const QVariantMap &item) override;
    void applyInvalidationEvent(const QVariantMap &event) override;
    void applyPendingMetadataPatch(
        const QString &itemId,
        const QVariantMap &payload) override;
    void applyContainerMutation(
        const QString &itemId,
        MediaPort::Operation operation,
        const QVariantMap &result) override;
    void beginRefreshProtection(const QString &itemId) override;
    void endRefreshProtection(const QString &itemId) override;
    void invalidatePresentationCache() override;
    void scheduleContentReconciliation(const QString &reason) override;

private:
    enum class NavigationKind {
        Library,
        Collection,
    };

    struct NavigationRequest
    {
        NavigationKind kind = NavigationKind::Library;
        QString resourceKey;
        QString parentId;
        RequestCommitToken resourceToken;
        LatestRequestToken presentationToken;
        quint64 sessionGeneration = 0;
        quint64 activityRevision = 0;
        qint64 enqueuedAtMs = 0;
        bool hadCachedData = false;
    };

    struct NavigationLane
    {
        QFutureWatcher<YanamiOperationResult> watcher;
        std::optional<NavigationRequest> active;
        QElapsedTimer operationTimer;
    };

    CatalogSessionState currentSession() const;
    bool activeSession() const;
    bool acceptsSession(quint64 generation) const;
    void publishCapabilities();
    void resetCommittedState(bool removeDiskCache);

    void beginActivityRefresh();
    RequestDisposition requestActivityRefresh(bool force);
    RequestDisposition startFavoritesRefresh(bool force);
    void beginFavoritesRefresh(bool force);
    void finishActivityRefresh();
    void finishFavoritesRefresh();
    void publishCachedCollection(const QString &parentId);

    bool promoteEquivalentNavigationQuery(
        NavigationKind kind,
        const QString &resourceKey,
        const QString &laneKey);
    void submitNavigationQuery(NavigationRequest request);
    void startNavigationQuery(
        NavigationLane &lane,
        NavigationRequest request);
    void finishNavigationQuery(NavigationLane &lane);
    void pumpNavigationQueries();
    void resetNavigationQueries(const char *reason);
    bool navigationQueryPending(
        NavigationKind kind,
        const QString &parentId = {},
        bool activeOnly = false) const;
    RequestDisposition requestCollection(
        const QString &parentId,
        bool forceRefresh);
    void notifyNavigationStateChanged();
    void applyNavigationResult(
        const NavigationRequest &request,
        const YanamiOperationResult &operationResult,
        qint64 operationElapsedMs);

    bool validateResponseSchema(
        const QJsonObject &object,
        const char *responseName,
        bool reportStatus);
    bool applyLibraryObject(
        const QJsonObject &object,
        quint64 activityRevision);
    bool applyActivityObject(
        const QJsonObject &object,
        quint64 activityRevision);
    bool applyActivityQueries(
        const QJsonObject &object,
        quint64 activityRevision);
    bool applyLatestMediaQueries(
        const QJsonObject &object,
        qint64 fetchedAtMs);
    void markLatestMediaQueriesStale();

    bool loadLibraryCache();
    void saveLibraryCache() const;
    void clearLibraryCache();
    void updateLibraryCachePath();

    RustBridgeRuntime &m_runtime;
    QThreadPool &m_navigationPool;
    SessionStateProvider m_sessionStateProvider;
    StatusSink &m_statusSink;
    CapabilitySink m_capabilitySink;
    std::unique_ptr<MediaStore> m_mediaStore;
    RequestCoordinator m_requests;

    std::array<NavigationLane, 2> m_navigationLanes;
    std::optional<NavigationRequest> m_queuedLibraryNavigation;
    std::optional<NavigationRequest> m_queuedCollectionNavigation;
    QFutureWatcher<YanamiOperationResult> m_activityWatcher;
    QFutureWatcher<YanamiOperationResult> m_favoritesWatcher;
    QElapsedTimer m_activityTimer;
    RequestCommitToken m_activityRequest;
    RequestCommitToken m_favoritesRequest;
    quint64 m_activitySessionGeneration = 0;
    quint64 m_activityRequestRevision = 0;
    quint64 m_activityRevisionIssued = 0;
    quint64 m_activityRevisionCommitted = 0;
    quint64 m_favoritesSessionGeneration = 0;

    CatalogSessionState m_committedSession;
    CatalogUserCapabilities m_capabilities;
    QString m_cacheDirectory;
    QString m_cacheScope;
    QString m_cachePath;
    QString m_collectionTargetId;
    QString m_collectionDisplayedId;
    QString m_collectionErrorId;
    QSet<QString> m_latestMediaScopeIds;
    qint64 m_lastFullLibraryRefreshMs = 0;
    qint64 m_lastFavoritesRefreshMs = 0;
    qint64 m_activityRetryAfterMs = 0;
    int m_activityConsecutiveFailures = 0;
    bool m_activityRefreshQueued = false;
    bool m_favoritesRefreshQueued = false;
    bool m_activityRefreshing = false;
    bool m_activityLoadFailed = false;
    bool m_favoritesRefreshing = false;
    bool m_favoritesLoadFailed = false;
    bool m_libraryLoadFailed = false;
    bool m_initialized = false;
    bool m_sessionFenced = true;
    bool m_shuttingDown = false;
};
