#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class MediaStore;
class MediaQueryModel;
class QAbstractItemModel;

class SessionPort : public QObject
{
    Q_OBJECT

public:
    enum class Operation {
        Login,
        Logout,
    };
    Q_ENUM(Operation)

    using QObject::QObject;

    virtual bool connected() const = 0;
    virtual quint64 generation() const = 0;
    virtual bool busy() const = 0;
    virtual QString displayName() const = 0;
    virtual QString serverUrl() const = 0;
    virtual QString userName() const = 0;
    virtual QString serverDomain() const = 0;
    virtual bool administrator() const = 0;
    virtual bool canDownload() const = 0;
    virtual bool canDelete() const = 0;

    virtual void login(
        const QString &requestId,
        const QString &serverName,
        const QString &serverUrl,
        const QString &userName,
        const QString &password,
        bool allowInsecureHttp) = 0;
    virtual void logout(const QString &requestId) = 0;

signals:
    void stateChanged();
    void operationCompleted(
        const QString &requestId,
        SessionPort::Operation operation);
    void operationFailed(
        const QString &requestId,
        SessionPort::Operation operation,
        const QString &message);
};

class CatalogPort : public QObject
{
    Q_OBJECT

public:
    enum class RequestDisposition {
        Accepted,
        AlreadyCurrent,
        Rejected,
    };
    Q_ENUM(RequestDisposition)

    using QObject::QObject;

    virtual MediaStore *mediaStore() const = 0;
    virtual bool libraryRefreshing() const = 0;
    virtual bool activityRefreshing() const = 0;
    virtual bool collectionLoading() const = 0;
    virtual bool collectionFetching() const = 0;
    virtual bool libraryLoadFailed() const = 0;
    virtual bool activityLoadFailed() const = 0;
    virtual bool favoritesRefreshing() const = 0;
    virtual bool favoritesLoadFailed() const = 0;
    virtual QString collectionDisplayedId() const = 0;
    virtual QString collectionTargetId() const = 0;
    virtual QString collectionErrorId() const = 0;
    virtual QVariantMap collectionParent() const = 0;

    virtual RequestDisposition loadLibrary() = 0;
    virtual void invalidateActivity() = 0;
    virtual void invalidateSeriesContinue(const QString &seriesId) = 0;
    virtual RequestDisposition ensureActivityFresh() = 0;
    virtual RequestDisposition refreshActivity() = 0;
    virtual RequestDisposition loadFavorites() = 0;
    virtual RequestDisposition refreshFavorites() = 0;
    virtual RequestDisposition loadCollection(const QString &parentId) = 0;
    virtual RequestDisposition refreshCollection(const QString &parentId) = 0;

signals:
    void stateChanged();
};

class SearchPort : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual MediaQueryModel *resultsModel() const = 0;
    virtual MediaQueryModel *titleResultsModel() const = 0;
    virtual MediaQueryModel *episodeResultsModel() const = 0;
    virtual QAbstractItemModel *resultRowsModel() const = 0;
    virtual QString query() const = 0;
    virtual bool searching() const = 0;
    virtual bool syncing() const = 0;
    virtual bool complete() const = 0;
    virtual qint64 cachedCount() const = 0;
    virtual qint64 totalCount() const = 0;
    virtual qint64 totalMatches() const = 0;
    virtual bool hasMore() const = 0;
    virtual QString error() const = 0;

    virtual void inputPending() = 0;
    virtual void requestSearch(const QString &query) = 0;
    virtual void refresh() = 0;

signals:
    void stateChanged();
};

class PlaybackPort : public QObject
{
    Q_OBJECT

public:
    enum class Event {
        Started,
        Progress,
        Stopped,
    };
    Q_ENUM(Event)

    struct Snapshot {
        QString reportSessionId;
        double positionSeconds = 0;
        bool paused = false;
        bool muted = false;
        double volume = 100;
        double rate = 1;
        int audioStreamIndex = -1;
        int subtitleStreamIndex = -1;
        bool seekable = false;
    };

    using QObject::QObject;

    virtual void prepare(
        const QString &requestId,
        const QString &itemId) = 0;
    virtual void prepareInContext(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &context) = 0;
    virtual void cancelPreparation() = 0;
    virtual void switchTo(
        const QString &requestId,
        const QString &itemId,
        double positionSeconds,
        bool paused) = 0;
    virtual void switchToInContext(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &context,
        double positionSeconds,
        bool paused) = 0;
    virtual void report(Event event, const Snapshot &snapshot) = 0;

signals:
    void ready(const QString &requestId, const QVariantMap &descriptor);
    void failed(
        const QString &requestId,
        const QString &itemId,
        const QString &message);
    void stoppedReported();
};

class DanmakuPort : public QObject
{
    Q_OBJECT

public:
    enum class Operation {
        Search,
        AutomaticLoad,
        ApplyMatch,
    };
    Q_ENUM(Operation)

    enum class ConfigurationOperation {
        Configure,
        Clear,
    };
    Q_ENUM(ConfigurationOperation)

    using QObject::QObject;

    virtual bool configured() const = 0;
    virtual int credentialSource() const = 0;
    virtual bool configurationBusy() const = 0;
    virtual void search(
        const QString &requestId,
        const QString &itemId,
        const QString &anime) = 0;
    virtual void loadAutomatically(
        const QString &requestId,
        const QString &itemId) = 0;
    virtual void applyMatch(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &match,
        const QVariantMap &style) = 0;
    virtual void configure(
        const QString &requestId,
        const QString &appId,
        const QString &appSecret) = 0;
    virtual void clearConfiguration(const QString &requestId) = 0;

signals:
    void stateChanged();
    void operationCompleted(
        const QString &requestId,
        const QString &itemId,
        DanmakuPort::Operation operation,
        const QVariantMap &result);
    void operationFailed(
        const QString &requestId,
        const QString &itemId,
        DanmakuPort::Operation operation,
        const QString &message,
        bool nonModal);
    void configurationCompleted(
        const QString &requestId,
        DanmakuPort::ConfigurationOperation operation);
    void configurationFailed(
        const QString &requestId,
        DanmakuPort::ConfigurationOperation operation,
        const QString &message);
};

class MediaPort : public QObject
{
    Q_OBJECT

public:
    enum class Operation {
        LoadMetadata,
        UpdateMetadata,
        LoadImages,
        LoadImageProviders,
        SearchImages,
        ApplyRemoteImage,
        UploadImage,
        RemoveImage,
        RefreshMetadata,
        LoadPlaylistTargets,
        AddToPlaylist,
        RemoveFromPlaylist,
        SetPlayed,
        SetFavorite,
        ScanLibraryFiles,
        DeleteItem,
    };
    Q_ENUM(Operation)

    using QObject::QObject;

    virtual QVariantMap libraryScanProgress() const = 0;
    virtual void loadMetadata(
        const QString &requestId,
        const QString &itemId,
        quint64 viewGeneration) = 0;
    virtual void updateMetadata(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &changes) = 0;
    virtual void loadImages(
        const QString &requestId,
        const QString &itemId,
        quint64 viewGeneration) = 0;
    virtual void loadImageProviders(
        const QString &requestId,
        const QString &itemId,
        quint64 viewGeneration) = 0;
    virtual void searchImages(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QString &providerName,
        bool includeAllLanguages,
        bool enableSeriesImages,
        quint64 viewGeneration,
        quint64 searchGeneration,
        int startIndex,
        int limit) = 0;
    virtual void applyRemoteImage(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QUrl &imageUrl,
        const QString &providerName,
        const QVariant &imageIndex) = 0;
    virtual void uploadImage(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QUrl &fileUrl,
        const QVariant &imageIndex) = 0;
    virtual void removeImage(
        const QString &requestId,
        const QString &itemId,
        const QString &imageType,
        const QVariant &imageIndex) = 0;
    virtual void refreshMetadata(
        const QString &requestId,
        const QString &itemId,
        const QString &mode,
        bool replaceImages,
        const QString &source) = 0;
    virtual void loadPlaylistTargets(
        const QString &requestId,
        const QString &itemId) = 0;
    virtual void addToPlaylist(
        const QString &requestId,
        const QString &itemId,
        const QString &targetId) = 0;
    virtual void createPlaylistAndAdd(
        const QString &requestId,
        const QString &itemId,
        const QString &newName) = 0;
    virtual void removeFromPlaylist(
        const QString &requestId,
        const QString &itemId,
        const QString &playlistId,
        const QString &entryId) = 0;
    virtual void setPlayed(
        const QString &requestId,
        const QString &itemId,
        bool played) = 0;
    virtual void setFavorite(
        const QString &requestId,
        const QString &itemId,
        bool favorite) = 0;
    virtual void scanLibraryFiles(
        const QString &requestId,
        const QString &itemId) = 0;
    virtual void deleteItem(
        const QString &requestId,
        const QString &itemId) = 0;

signals:
    void scanProgressChanged();
    void operationCompleted(
        const QString &requestId,
        const QString &itemId,
        MediaPort::Operation operation,
        const QVariantMap &result);
    void operationFailed(
        const QString &requestId,
        const QString &itemId,
        MediaPort::Operation operation,
        const QString &message,
        bool nonModal);
};

class ApplicationStatusPort : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual bool ready() const = 0;
    virtual QString message() const = 0;
    virtual bool error() const = 0;
    virtual void clear() = 0;

signals:
    void stateChanged();
};

class PlaybackReporterPort
{
public:
    virtual ~PlaybackReporterPort() = default;
    virtual bool attachPlayer(QObject *player) = 0;
    virtual bool beginSession(
        const QString &reportSessionId,
        const QVariantList &embyTracks) = 0;
    virtual void stopSession() = 0;
};

struct BackendPortSet
{
    SessionPort *session = nullptr;
    CatalogPort *catalog = nullptr;
    SearchPort *search = nullptr;
    PlaybackPort *playback = nullptr;
    PlaybackReporterPort *playbackReporter = nullptr;
    DanmakuPort *danmaku = nullptr;
    MediaPort *media = nullptr;
    ApplicationStatusPort *status = nullptr;
};
