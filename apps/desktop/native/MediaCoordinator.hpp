#pragma once

#include "BackendPorts.hpp"
#include "CatalogCoordinator.hpp"
#include "KeyedMutationScheduler.hpp"
#include "RequestCoordinator.hpp"
#include "RustBridgeRuntime.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <array>
#include <functional>
#include <optional>

class StatusSink;

// Media needs only the authentication fence. Account identity, capabilities,
// catalog navigation and cache ownership stay behind their feature boundaries.
struct MediaSessionState
{
    quint64 generation = 0;
    bool connected = false;
};

// Owns every typed media read/mutation and the Emby refresh-notification
// lifecycle. The only presentation writes leave through CatalogMutationSink;
// this coordinator never reaches into MediaStore or catalog request state.
class MediaCoordinator final : public MediaPort
{
    Q_OBJECT

public:
    using SessionStateProvider = std::function<MediaSessionState()>;

    MediaCoordinator(
        RustBridgeRuntime &runtime,
        QThreadPool &readPool,
        QThreadPool &mutationPool,
        SessionStateProvider sessionStateProvider,
        StatusSink &statusSink,
        CatalogMutationSink &catalogSink,
        QObject *parent = nullptr);
    ~MediaCoordinator() override;

    bool initializeFromSession();
    void sessionTransitionStarted(
        const char *reason = "session_transition");
    void sessionTransitionAborted();
    void sessionCommitted();
    void shutdown();
    void drain();

    QVariantMap libraryScanProgress() const override
    { return m_libraryScanProgress; }

    void loadMetadata(
        const QString &requestId,
        const QString &itemId,
        quint64 viewGeneration) override;
    void updateMetadata(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &changes) override;
    void loadImages(
        const QString &requestId,
        const QString &itemId,
        quint64 viewGeneration) override;
    void loadImageProviders(
        const QString &requestId,
        const QString &itemId,
        quint64 viewGeneration) override;
    void searchImages(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QString &providerName,
        bool includeAllLanguages,
        bool enableSeriesImages,
        quint64 viewGeneration,
        quint64 searchGeneration,
        int startIndex,
        int limit) override;
    void applyRemoteImage(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QUrl &imageUrl,
        const QString &providerName,
        const QVariant &imageIndex) override;
    void uploadImage(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QUrl &fileUrl,
        const QVariant &imageIndex) override;
    void removeImage(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QVariant &imageIndex) override;
    void refreshMetadata(
        const QString &requestId,
        const QString &itemId,
        const QString &mode,
        bool replaceImages,
        const QString &source) override;
    void loadPlaylistTargets(
        const QString &requestId,
        const QString &itemId) override;
    void addToPlaylist(
        const QString &requestId,
        const QString &itemId,
        const QString &targetId) override;
    void createPlaylistAndAdd(
        const QString &requestId,
        const QString &itemId,
        const QString &newName) override;
    void removeFromPlaylist(
        const QString &requestId,
        const QString &itemId,
        const QString &playlistId,
        const QString &entryId) override;
    void setPlayed(
        const QString &requestId,
        const QString &itemId,
        bool played) override;
    void setFavorite(
        const QString &requestId,
        const QString &itemId,
        bool favorite) override;
    void scanLibraryFiles(
        const QString &requestId,
        const QString &itemId) override;
    void deleteItem(
        const QString &requestId,
        const QString &itemId) override;

private:
    struct MediaRequest
    {
        QString clientRequestId;
        QString itemId;
        MediaPort::Operation operation = MediaPort::Operation::LoadMetadata;
        QVariantMap payload;
        quint64 submissionSequence = 0;
        quint64 sessionGeneration = 0;
        qint64 enqueuedAtMs = 0;
    };

    struct InteractiveRequest : MediaRequest
    {
        LatestRequestToken token;
    };

    struct InteractiveLane
    {
        QString key;
        QFutureWatcher<YanamiOperationResult> watcher;
        std::optional<InteractiveRequest> active;
        std::optional<InteractiveRequest> queued;
        QElapsedTimer timer;
    };

    struct MutationRequest : MediaRequest
    {
        QString optimisticMutationId;
    };

    struct MutationLane
    {
        QFutureWatcher<YanamiOperationResult> watcher;
        std::optional<MutationRequest> active;
        QElapsedTimer timer;
    };

    MediaSessionState currentSession() const;
    bool activeSession() const;
    bool acceptsSession(quint64 generation) const;
    void submit(
        const QString &requestId,
        const QString &itemId,
        MediaPort::Operation operation,
        QVariantMap payload);
    void rejectImmediately(
        const QString &requestId,
        const QString &itemId,
        MediaPort::Operation operation,
        const QString &message);

    InteractiveLane *interactiveLane(MediaPort::Operation operation);
    std::array<InteractiveLane *, 5> interactiveLanes();
    void submitInteractive(InteractiveRequest request);
    void startInteractive(
        InteractiveLane &lane,
        InteractiveRequest request);
    void finishInteractive(InteractiveLane &lane);
    void pumpInteractive(InteractiveLane &lane);
    void pumpInteractiveReads();
    void settleSuperseded(InteractiveRequest request);
    void fenceInteractiveReads(
        const MutationRequest &mutation,
        const char *reason);
    void resetInteractiveReads(const char *reason);
    bool blockedByMutation(const InteractiveRequest &request) const;
    bool conflicts(
        const InteractiveRequest &read,
        const MutationRequest &mutation) const;

    void submitMutation(MutationRequest request);
    void dispatchMutations();
    void startMutation(MutationLane &lane, MutationRequest request);
    void finishMutation(MutationLane &lane);
    void applyMutationResult(
        MutationRequest &request,
        const YanamiOperationResult &operationResult,
        qint64 elapsedMs);
    void resetMutations(const char *reason);

    void pollRefreshProgress();
    void finishRefreshProgress();
    void applyRefreshProgress(
        const YanamiOperationResult &operationResult);
    void trackLibraryScan(
        const QString &itemId,
        qint64 operationElapsedMs);
    void trackMetadataRefresh(
        const QString &itemId,
        qint64 operationElapsedMs);
    void reconcileCompletedLibraryScans(const QSet<QString> &itemIds);
    void reconcileCompletedMetadataRefreshes(const QSet<QString> &itemIds);
    void pollMetadataRefreshFallbacks();
    void resetRefreshTracking();

    bool validateResponseSchema(
        const QJsonObject &object,
        const char *responseName,
        bool publishError);
    void complete(
        const MediaRequest &request,
        const QVariantMap &result);
    void fail(
        const MediaRequest &request,
        const QString &message,
        bool publishStatus);

    RustBridgeRuntime &m_runtime;
    QThreadPool &m_readPool;
    QThreadPool &m_mutationPool;
    SessionStateProvider m_sessionStateProvider;
    StatusSink &m_statusSink;
    CatalogMutationSink &m_catalogSink;
    RequestCoordinator m_requests;
    KeyedMutationScheduler<MutationRequest> m_mutations;

    InteractiveLane m_metadataLane;
    InteractiveLane m_playlistTargetsLane;
    InteractiveLane m_imageListLane;
    InteractiveLane m_imageProvidersLane;
    InteractiveLane m_imageSearchLane;
    std::array<MutationLane, 2> m_mutationLanes;
    std::optional<MutationRequest> m_settlingMutation;
    QFutureWatcher<YanamiOperationResult> m_refreshProgressWatcher;
    QTimer m_refreshProgressTimer;
    QTimer m_metadataRefreshFallbackTimer;

    QVariantMap m_libraryScanProgress;
    QSet<QString> m_requestedLibraryScans;
    QHash<QString, qint64> m_libraryScanRequestedAtMs;
    QSet<QString> m_hiddenLibraryScanIndicators;
    QSet<QString> m_completedLibraryScans;
    QSet<QString> m_requestedMetadataRefreshes;
    QSet<QString> m_refreshProtectedItems;
    QHash<QString, qint64> m_metadataRefreshRequestedAtMs;
    QHash<QString, int> m_metadataRefreshReconcileAttempts;
    QHash<QString, qint64> m_refreshProtectionReleaseAtMs;
    QHash<QString, qint64> m_recentRefreshCompletionsMs;
    QSet<quint64> m_settledInteractiveSequences;
    QSet<quint64> m_settledMutationSequences;
    QSet<quint64> m_speculativeMutationSequences;
    QSet<QString> m_completedSpeculativeMutationItems;
    quint64 m_nextSubmissionSequence = 0;
    quint64 m_refreshProgressSessionGeneration = 0;
    quint64 m_bufferedRefreshProgressSessionGeneration = 0;
    std::optional<YanamiOperationResult> m_bufferedRefreshProgress;
    bool m_initialized = false;
    bool m_sessionFenced = true;
    bool m_shuttingDown = false;
    bool m_dispatchingMutations = false;
};
