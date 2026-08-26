#pragma once

#include "BackendPorts.hpp"
#include "AsyncOperationState.hpp"
#include "AsyncResourceState.hpp"
#include "ImageEditorViewModel.hpp"
#include "MediaTargetFlowViewModel.hpp"
#include "MetadataEditorViewModel.hpp"
#include "MediaStore.hpp"
#include "UpdateChecker.hpp"
#include "UpscalingViewModel.hpp"

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class SessionViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(quint64 generation READ generation NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY stateChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY stateChanged)
    Q_PROPERTY(QString userName READ userName NOTIFY stateChanged)
    Q_PROPERTY(QString serverDomain READ serverDomain NOTIFY stateChanged)
    Q_PROPERTY(bool administrator READ administrator NOTIFY stateChanged)
    Q_PROPERTY(bool canDownload READ canDownload NOTIFY stateChanged)
    Q_PROPERTY(bool canDelete READ canDelete NOTIFY stateChanged)
    Q_PROPERTY(AsyncOperationState *operation READ operation CONSTANT)

public:
    explicit SessionViewModel(
        SessionPort *port,
        QObject *parent = nullptr);

    bool connected() const;
    quint64 generation() const;
    bool busy() const;
    QString displayName() const;
    QString serverUrl() const;
    QString userName() const;
    QString serverDomain() const;
    bool administrator() const;
    bool canDownload() const;
    bool canDelete() const;
    AsyncOperationState *operation() const { return m_operation; }

    Q_INVOKABLE void login(
        const QString &serverName,
        const QString &serverUrl,
        const QString &userName,
        const QString &password,
        bool allowInsecureHttp = false);
    Q_INVOKABLE void logout();

signals:
    void stateChanged();

private:
    void handleOperationCompleted(
        const QString &requestId,
        SessionPort::Operation operation);
    void handleOperationFailed(
        const QString &requestId,
        SessionPort::Operation operation,
        const QString &message);

    QPointer<SessionPort> m_port;
    AsyncOperationState *m_operation = nullptr;
    SessionPort::Operation m_pendingOperation = SessionPort::Operation::Login;
    bool m_hasPendingOperation = false;
    quint64 m_operationSequence = 0;
};

class HomeViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MediaStore *mediaStore READ mediaStore CONSTANT)
    Q_PROPERTY(bool libraryRefreshing READ libraryRefreshing NOTIFY stateChanged)
    Q_PROPERTY(bool activityRefreshing READ activityRefreshing NOTIFY stateChanged)
    Q_PROPERTY(bool collectionLoading READ collectionLoading NOTIFY stateChanged)
    Q_PROPERTY(bool collectionFetching READ collectionFetching NOTIFY stateChanged)
    Q_PROPERTY(bool libraryLoadFailed READ libraryLoadFailed NOTIFY stateChanged)
    Q_PROPERTY(bool activityLoadFailed READ activityLoadFailed NOTIFY stateChanged)
    Q_PROPERTY(QString collectionDisplayedId READ collectionDisplayedId NOTIFY stateChanged)
    Q_PROPERTY(QString collectionTargetId READ collectionTargetId NOTIFY stateChanged)
    Q_PROPERTY(QString collectionErrorId READ collectionErrorId NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap collectionParent READ collectionParent NOTIFY stateChanged)
    Q_PROPERTY(AsyncResourceState *libraryState READ libraryState CONSTANT)
    Q_PROPERTY(AsyncResourceState *activityState READ activityState CONSTANT)
    Q_PROPERTY(AsyncResourceState *collectionState READ collectionState CONSTANT)

public:
    explicit HomeViewModel(
        CatalogPort *port,
        QObject *parent = nullptr);

    MediaStore *mediaStore() const;
    bool libraryRefreshing() const;
    bool activityRefreshing() const;
    bool collectionLoading() const;
    bool collectionFetching() const;
    bool libraryLoadFailed() const;
    bool activityLoadFailed() const;
    QString collectionDisplayedId() const;
    QString collectionTargetId() const;
    QString collectionErrorId() const;
    QVariantMap collectionParent() const;
    AsyncResourceState *libraryState() const { return m_libraryState; }
    AsyncResourceState *activityState() const { return m_activityState; }
    AsyncResourceState *collectionState() const { return m_collectionState; }

    Q_INVOKABLE void loadLibrary();
    Q_INVOKABLE void ensureActivityFresh();
    Q_INVOKABLE void refreshActivity();
    Q_INVOKABLE void loadCollection(const QString &parentId);
    Q_INVOKABLE void refreshCollection(const QString &parentId);

signals:
    void stateChanged();

private:
    void settleResources();

    QPointer<CatalogPort> m_port;
    AsyncResourceState *m_libraryState = nullptr;
    AsyncResourceState *m_activityState = nullptr;
    AsyncResourceState *m_collectionState = nullptr;
};

class SearchViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MediaQueryModel *results READ results CONSTANT)
    Q_PROPERTY(MediaQueryModel *titleResults READ titleResults CONSTANT)
    Q_PROPERTY(MediaQueryModel *episodeResults READ episodeResults CONSTANT)
    Q_PROPERTY(QAbstractItemModel *resultRows READ resultRows CONSTANT)
    Q_PROPERTY(QString query READ query NOTIFY stateChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY stateChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY stateChanged)
    Q_PROPERTY(bool complete READ complete NOTIFY stateChanged)
    Q_PROPERTY(qint64 cachedCount READ cachedCount NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalCount READ totalCount NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalMatches READ totalMatches NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    explicit SearchViewModel(
        SearchPort *port,
        QObject *parent = nullptr);

    MediaQueryModel *results() const;
    MediaQueryModel *titleResults() const;
    MediaQueryModel *episodeResults() const;
    QAbstractItemModel *resultRows() const;
    QString query() const;
    bool searching() const;
    bool syncing() const;
    bool complete() const;
    qint64 cachedCount() const;
    qint64 totalCount() const;
    qint64 totalMatches() const;
    bool hasMore() const;
    QString error() const;

    Q_INVOKABLE void inputPending();
    Q_INVOKABLE void submit(const QString &query);
    Q_INVOKABLE void refresh();

signals:
    void stateChanged();

private:
    QPointer<SearchPort> m_port;
};

class FavoritesViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY stateChanged)
    Q_PROPERTY(bool loadFailed READ loadFailed NOTIFY stateChanged)
    Q_PROPERTY(AsyncResourceState *resourceState READ resourceState CONSTANT)

public:
    explicit FavoritesViewModel(
        CatalogPort *port,
        QObject *parent = nullptr);

    bool refreshing() const;
    bool loadFailed() const;
    AsyncResourceState *resourceState() const { return m_resourceState; }

    Q_INVOKABLE void load();
    Q_INVOKABLE void refresh();

signals:
    void stateChanged();

private:
    QPointer<CatalogPort> m_port;
    AsyncResourceState *m_resourceState = nullptr;
};

class PlaybackViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(AsyncResourceState *preparationState READ preparationState CONSTANT)

public:
    explicit PlaybackViewModel(
        PlaybackPort *port,
        PlaybackReporterPort *reporter = nullptr,
        QObject *parent = nullptr);

    Q_INVOKABLE void prepare(const QString &itemId);
    Q_INVOKABLE void prepareInContext(
        const QString &itemId,
        const QVariantMap &context);
    Q_INVOKABLE void cancelPreparation();
    Q_INVOKABLE void switchTo(
        const QString &itemId,
        double positionSeconds,
        bool paused);
    Q_INVOKABLE void switchToInContext(
        const QString &itemId,
        const QVariantMap &context,
        double positionSeconds,
        bool paused);
    Q_INVOKABLE bool attachPlayer(QObject *player);
    Q_INVOKABLE bool beginSession(
        const QString &reportSessionId,
        const QVariantList &embyTracks);
    Q_INVOKABLE void stopSession();
    void invalidateSession();
    AsyncResourceState *preparationState() const { return m_preparationState; }

signals:
    // A map keeps this facade stable when the transport descriptor gains a
    // field, while still replacing the backend's positional signal in QML.
    void ready(const QVariantMap &descriptor);
    void failed(const QString &itemId, const QString &message);
    void stoppedReported();

private:
    QString beginPreparation(const QString &itemId);

    QPointer<PlaybackPort> m_port;
    PlaybackReporterPort *m_reporter = nullptr;
    AsyncResourceState *m_preparationState = nullptr;
    QString m_preparingItemId;
    QString m_pendingRequestId;
    quint64 m_requestSequence = 0;
};

class DanmakuViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool configured READ configured NOTIFY configuredChanged)
    Q_PROPERTY(int credentialSource READ credentialSource NOTIFY configuredChanged)
    Q_PROPERTY(bool working READ working NOTIFY workingChanged)
    Q_PROPERTY(bool configurationBusy READ configurationBusy NOTIFY configurationBusyChanged)
    Q_PROPERTY(AsyncResourceState *searchState READ searchState CONSTANT)
    Q_PROPERTY(AsyncResourceState *automaticLoadState READ automaticLoadState CONSTANT)
    Q_PROPERTY(AsyncOperationState *applyOperation READ applyOperation CONSTANT)
    Q_PROPERTY(AsyncOperationState *configurationOperation READ configurationOperation CONSTANT)

public:
    explicit DanmakuViewModel(
        DanmakuPort *port,
        QObject *parent = nullptr);

    bool configured() const;
    int credentialSource() const;
    bool working() const;
    bool configurationBusy() const;
    AsyncResourceState *searchState() const { return m_searchState; }
    AsyncResourceState *automaticLoadState() const { return m_automaticLoadState; }
    AsyncOperationState *applyOperation() const { return m_applyOperation; }
    AsyncOperationState *configurationOperation() const { return m_configurationOperation; }

    Q_INVOKABLE void search(
        const QString &itemId,
        const QString &anime);
    Q_INVOKABLE void loadAutomatically(const QString &itemId);
    Q_INVOKABLE void applyMatch(
        const QString &itemId,
        const QVariantMap &match,
        const QVariantMap &style);
    Q_INVOKABLE void configure(
        const QString &appId,
        const QString &appSecret);
    Q_INVOKABLE void clearConfiguration();
    void invalidateSession();

signals:
    void configuredChanged();
    void workingChanged();
    void configurationBusyChanged();

    void searchCompleted(const QString &itemId, const QVariantMap &result);
    void automaticLoadCompleted(
        const QString &itemId, const QVariantMap &result);
    void matchApplied(const QString &itemId, const QVariantMap &result);
    void searchFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void automaticLoadFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void matchApplyFailed(
        const QString &itemId, const QString &message, bool nonModal);

private:
    void setPending(DanmakuPort::Operation operation, bool pending);
    void handleCompleted(
        const QString &requestId,
        const QString &itemId,
        DanmakuPort::Operation operation,
        const QVariantMap &result);
    void handleFailed(
        const QString &requestId,
        const QString &itemId,
        DanmakuPort::Operation operation,
        const QString &message,
        bool nonModal);
    void handleConfigurationCompleted(
        const QString &requestId,
        DanmakuPort::ConfigurationOperation operation);
    void handleConfigurationFailed(
        const QString &requestId,
        DanmakuPort::ConfigurationOperation operation,
        const QString &message);

    QPointer<DanmakuPort> m_port;
    AsyncResourceState *m_searchState = nullptr;
    AsyncResourceState *m_automaticLoadState = nullptr;
    AsyncOperationState *m_applyOperation = nullptr;
    AsyncOperationState *m_configurationOperation = nullptr;
    DanmakuPort::ConfigurationOperation m_pendingConfigurationOperation =
        DanmakuPort::ConfigurationOperation::Configure;
    bool m_hasPendingConfigurationOperation = false;
    quint64 m_operationSequence = 0;
    QString m_searchRequestId;
    QString m_automaticLoadRequestId;
    QString m_applyRequestId;
    QString m_applyItemId;
    bool m_searchPending = false;
    bool m_automaticLoadPending = false;
    bool m_applyMatchPending = false;
};

class MediaActionsViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit MediaActionsViewModel(
        MediaPort *port,
        QObject *parent = nullptr);

    Q_PROPERTY(QVariantMap libraryScanProgress READ libraryScanProgress
            NOTIFY libraryScanProgressChanged)

    QVariantMap libraryScanProgress() const;

    Q_INVOKABLE void refreshMetadata(
        const QString &itemId,
        const QString &mode,
        bool replaceImages,
        const QString &source = {});
    Q_INVOKABLE void removeFromPlaylist(
        const QString &itemId,
        const QString &playlistId,
        const QString &entryId);
    Q_INVOKABLE void setPlayed(const QString &itemId, bool played);
    Q_INVOKABLE void setFavorite(const QString &itemId, bool favorite);
    Q_INVOKABLE void scanLibraryFiles(const QString &itemId);
    Q_INVOKABLE void deleteItem(const QString &itemId);

signals:
    void metadataRefreshed(const QString &itemId, const QVariantMap &result);
    void metadataRefreshFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void removedFromPlaylist(const QString &itemId, const QVariantMap &result);
    void removeFromPlaylistFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void playedChanged(
        const QString &itemId, bool played, const QVariantMap &result);
    void playStateChangeFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void favoriteChanged(
        const QString &itemId, bool favorite, const QVariantMap &result);
    void favoriteChangeFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void libraryFilesScanStarted(const QString &itemId, const QVariantMap &result);
    void libraryFilesScanFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void itemDeleted(const QString &itemId, const QVariantMap &result);
    void itemDeleteFailed(
        const QString &itemId, const QString &message, bool nonModal);
    void libraryScanProgressChanged();

private:
    struct PendingOperation {
        QString requestId;
        QString itemId;
        MediaPort::Operation operation = MediaPort::Operation::UpdateMetadata;
        std::function<void(const QString &)> dispatch;
    };

    void submitOperation(
        const QString &itemId,
        MediaPort::Operation operation,
        std::function<void(const QString &)> dispatch);
    void dispatchNextOperation(const QString &key);
    QString operationKey(
        const QString &itemId,
        MediaPort::Operation operation) const;
    void handleCompleted(
        const QString &requestId,
        const QString &itemId,
        MediaPort::Operation operation,
        const QVariantMap &result);
    void handleFailed(
        const QString &requestId,
        const QString &itemId,
        MediaPort::Operation operation,
        const QString &message,
        bool nonModal);

    QPointer<MediaPort> m_port;
    QHash<QString, AsyncOperationState *> m_operationStates;
    QHash<QString, QQueue<PendingOperation>> m_pendingOperations;
    QHash<QString, QString> m_knownRequests;
    quint64 m_operationSequence = 0;
};

class PreferencesViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap danmakuStyle READ danmakuStyle
            NOTIFY danmakuStyleChanged)
    Q_PROPERTY(QVariantMap upscalingSettings READ upscalingSettings
            NOTIFY upscalingSettingsChanged)
    Q_PROPERTY(int librarySortMode READ librarySortMode
            WRITE setLibrarySortMode NOTIFY librarySortModeChanged)

public:
    explicit PreferencesViewModel(QObject *parent = nullptr);

    QVariantMap danmakuStyle() const;
    QVariantMap upscalingSettings() const { return m_upscalingSettings; }
    int librarySortMode() const { return m_librarySortMode; }
    Q_INVOKABLE void saveDanmakuStyle(const QVariantMap &style);
    Q_INVOKABLE void saveUpscalingSettings(const QVariantMap &settings);
    void setLibrarySortMode(int sortMode);

signals:
    void danmakuStyleChanged();
    void upscalingSettingsChanged();
    void librarySortModeChanged();

private:
    QVariantMap m_upscalingSettings;
    int m_librarySortMode = 1;
};

class ApplicationStatusViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(bool error READ error NOTIFY stateChanged)

public:
    explicit ApplicationStatusViewModel(
        ApplicationStatusPort *port,
        QObject *parent = nullptr);

    bool ready() const;
    QString message() const;
    bool error() const;
    Q_INVOKABLE void clear();

signals:
    void stateChanged();

private:
    QPointer<ApplicationStatusPort> m_port;
};

class ApplicationViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(SessionViewModel *session READ session CONSTANT)
    Q_PROPERTY(HomeViewModel *home READ home CONSTANT)
    Q_PROPERTY(SearchViewModel *search READ search CONSTANT)
    Q_PROPERTY(FavoritesViewModel *favorites READ favorites CONSTANT)
    Q_PROPERTY(PlaybackViewModel *playback READ playback CONSTANT)
    Q_PROPERTY(DanmakuViewModel *danmaku READ danmaku CONSTANT)
    Q_PROPERTY(MediaActionsViewModel *mediaActions READ mediaActions CONSTANT)
    Q_PROPERTY(ImageEditorViewModel *imageEditor READ imageEditor CONSTANT)
    Q_PROPERTY(MetadataEditorViewModel *metadataEditor READ metadataEditor CONSTANT)
    Q_PROPERTY(MediaTargetFlowViewModel *mediaTarget READ mediaTarget CONSTANT)
    Q_PROPERTY(PreferencesViewModel *preferences READ preferences CONSTANT)
    Q_PROPERTY(UpscalingViewModel *upscaling READ upscaling CONSTANT)
    Q_PROPERTY(ApplicationStatusViewModel *status READ status CONSTANT)
    Q_PROPERTY(UpdateChecker *updates READ updates CONSTANT)

public:
    explicit ApplicationViewModel(
        const BackendPortSet &ports,
        QObject *parent = nullptr);

    SessionViewModel *session() const { return m_session; }
    HomeViewModel *home() const { return m_home; }
    SearchViewModel *search() const { return m_search; }
    FavoritesViewModel *favorites() const { return m_favorites; }
    PlaybackViewModel *playback() const { return m_playback; }
    DanmakuViewModel *danmaku() const { return m_danmaku; }
    MediaActionsViewModel *mediaActions() const { return m_mediaActions; }
    ImageEditorViewModel *imageEditor() const { return m_imageEditor; }
    MetadataEditorViewModel *metadataEditor() const { return m_metadataEditor; }
    MediaTargetFlowViewModel *mediaTarget() const { return m_mediaTarget; }
    PreferencesViewModel *preferences() const { return m_preferences; }
    UpscalingViewModel *upscaling() const { return m_upscaling; }
    ApplicationStatusViewModel *status() const { return m_status; }
    UpdateChecker *updates() const { return m_updates; }

private:
    void initialize(const BackendPortSet &ports);

    SessionViewModel *m_session = nullptr;
    HomeViewModel *m_home = nullptr;
    SearchViewModel *m_search = nullptr;
    FavoritesViewModel *m_favorites = nullptr;
    PlaybackViewModel *m_playback = nullptr;
    DanmakuViewModel *m_danmaku = nullptr;
    MediaActionsViewModel *m_mediaActions = nullptr;
    ImageEditorViewModel *m_imageEditor = nullptr;
    MetadataEditorViewModel *m_metadataEditor = nullptr;
    MediaTargetFlowViewModel *m_mediaTarget = nullptr;
    PreferencesViewModel *m_preferences = nullptr;
    UpscalingViewModel *m_upscaling = nullptr;
    ApplicationStatusViewModel *m_status = nullptr;
    UpdateChecker *m_updates = nullptr;
    quint64 m_sessionGeneration = 0;
    quint64 m_playbackActivityReconcileRevision = 0;
};
