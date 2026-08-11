#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonObject>
#include <QLibrary>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

struct YanamiOperationResult
{
    int status = 1;
    QByteArray payload;
    QString error;
};

struct YanamiPlaybackReportRequest
{
    QByteArray event;
    quint64 positionTicks = 0;
    bool paused = false;
};

class BackendController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool blockingBusy READ blockingBusy NOTIFY busyChanged)
    Q_PROPERTY(bool danmakuConfigured READ danmakuConfigured NOTIFY danmakuConfiguredChanged)
    Q_PROPERTY(int danmakuCredentialSource READ danmakuCredentialSource NOTIFY danmakuConfiguredChanged)
    Q_PROPERTY(bool embyConnected READ embyConnected NOTIFY embyConnectedChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusMessageChanged)
    Q_PROPERTY(QVariantList mediaItems READ mediaItems NOTIFY mediaItemsChanged)
    Q_PROPERTY(QVariantList libraryViews READ libraryViews NOTIFY libraryViewsChanged)
    Q_PROPERTY(QVariantList resumeItems READ resumeItems NOTIFY resumeItemsChanged)
    Q_PROPERTY(QVariantList recentItems READ recentItems NOTIFY recentItemsChanged)
    Q_PROPERTY(QVariantList collectionItems READ collectionItems NOTIFY collectionItemsChanged)
    Q_PROPERTY(QVariantMap collectionParent READ collectionParent NOTIFY collectionItemsChanged)

public:
    explicit BackendController(QObject *parent = nullptr);
    ~BackendController() override;

    bool ready() const { return m_backend != nullptr; }
    bool busy() const { return m_busy; }
    bool blockingBusy() const { return m_busy && m_blockingBusy; }
    bool danmakuConfigured() const { return m_danmakuConfigured; }
    int danmakuCredentialSource() const { return m_danmakuCredentialSource; }
    bool embyConnected() const { return m_embyConnected; }
    QString statusMessage() const { return m_statusMessage; }
    bool statusIsError() const { return m_statusIsError; }
    QVariantList mediaItems() const { return m_mediaItems; }
    QVariantList libraryViews() const { return m_libraryViews; }
    QVariantList resumeItems() const { return m_resumeItems; }
    QVariantList recentItems() const { return m_recentItems; }
    QVariantList collectionItems() const { return m_collectionItems; }
    QVariantMap collectionParent() const { return m_collectionParent; }

    Q_INVOKABLE void configureDandanplay(const QString &appId, const QString &appSecret);
    Q_INVOKABLE void clearDandanplay();
    Q_INVOKABLE void loginEmby(
        const QString &serverName,
        const QString &serverUrl,
        const QString &username,
        const QString &password);
    Q_INVOKABLE void logoutEmby();
    Q_INVOKABLE void loadLibrary();
    Q_INVOKABLE void refreshActivity();
    Q_INVOKABLE void loadCollection(const QString &parentId);
    Q_INVOKABLE void preparePlayback(const QString &itemId);
    Q_INVOKABLE void switchPlayback(
        const QString &itemId,
        double positionSeconds,
        bool paused);
    Q_INVOKABLE void reportPlayback(const QString &event, double positionSeconds, bool paused);
    Q_INVOKABLE void clearStatus();

signals:
    void readyChanged();
    void busyChanged();
    void danmakuConfiguredChanged();
    void embyConnectedChanged();
    void statusMessageChanged();
    void mediaItemsChanged();
    void libraryViewsChanged();
    void resumeItemsChanged();
    void recentItemsChanged();
    void collectionItemsChanged();
    void playbackStoppedReported();
    void playbackReady(
        const QUrl &mediaUrl,
        const QVariantMap &headers,
        qint64 resumeTicks,
        const QString &title,
        const QVariantMap &previousItem,
        const QVariantMap &nextItem,
        const QVariantList &externalSubtitles,
        qint64 introStartTicks,
        qint64 introEndTicks,
        const QUrl &danmakuFile);

private:
    enum class Operation {
        None,
        ConfigureDanmaku,
        ClearDanmaku,
        LoginEmby,
        LogoutEmby,
        LoadLibrary,
        LoadCollection,
        PreparePlayback,
    };

    using BackendNew = void *(*)(const char *, char **);
    using BackendFree = void (*)(void *);
    using StringFree = void (*)(char *);
    using Status = int (*)(void *, char **);
    using ConfigureDanmaku = int (*)(void *, const char *, const char *, char **);
    using LoginEmby = int (*)(void *, const char *, const char *, const char *, const char *, char **);
    using JsonOperation = int (*)(void *, char **, char **);
    using ItemJsonOperation = int (*)(void *, const char *, char **, char **);
    using ReportPlayback = int (*)(void *, const char *, quint64, int, char **);

    template<typename Function>
    Function resolve(const char *name)
    {
        return reinterpret_cast<Function>(m_library.resolve(name));
    }

    struct CachedCollection {
        QVariantList items;
        QVariantMap parent;
        qint64 loadedAtMs = 0;
    };

    void start(
        Operation operation,
        std::function<YanamiOperationResult()> work,
        bool blocking = true);
    void finishOperation();
    void finishActivityRefresh();
    void startNextPlaybackReport();
    void finishPlaybackReport();
    bool applyLibraryObject(const QJsonObject &object);
    bool applyActivityObject(const QJsonObject &object);
    bool loadLibraryCache();
    void saveLibraryCache() const;
    void clearLibraryCache();
    void startQueuedUserOperation();
    YanamiOperationResult result(int status, char *payload, char *error) const;
    QString takeString(char *value) const;
    void setStatus(const QString &message, bool error = false);
    void refreshDanmakuStatus();

    QLibrary m_library;
    void *m_backend = nullptr;
    BackendNew m_new = nullptr;
    BackendFree m_free = nullptr;
    StringFree m_stringFree = nullptr;
    Status m_danmakuCredentialSourceStatus = nullptr;
    Status m_embyStatus = nullptr;
    ConfigureDanmaku m_configureDanmaku = nullptr;
    Status m_clearDanmaku = nullptr;
    LoginEmby m_loginEmby = nullptr;
    Status m_logoutEmby = nullptr;
    JsonOperation m_libraryJson = nullptr;
    JsonOperation m_activityJson = nullptr;
    ItemJsonOperation m_collectionJson = nullptr;
    ItemJsonOperation m_playbackJson = nullptr;
    ReportPlayback m_reportPlayback = nullptr;

    QFutureWatcher<YanamiOperationResult> m_watcher;
    QFutureWatcher<YanamiOperationResult> m_activityWatcher;
    QFutureWatcher<YanamiOperationResult> m_playbackReportWatcher;
    QQueue<YanamiPlaybackReportRequest> m_playbackReportQueue;
    YanamiPlaybackReportRequest m_activePlaybackReport;
    bool m_playbackReportInFlight = false;
    Operation m_operation = Operation::None;
    bool m_busy = false;
    bool m_blockingBusy = false;
    bool m_activityRefreshQueued = false;
    qint64 m_lastFullLibraryRefreshMs = 0;
    bool m_danmakuConfigured = false;
    int m_danmakuCredentialSource = 0;
    bool m_embyConnected = false;
    bool m_statusIsError = false;
    QString m_statusMessage;
    QVariantList m_mediaItems;
    QVariantList m_libraryViews;
    QVariantList m_resumeItems;
    QVariantList m_recentItems;
    QVariantList m_collectionItems;
    QVariantMap m_collectionParent;
    QHash<QString, CachedCollection> m_collectionCache;
    QString m_pendingCollectionId;
    QString m_queuedCollectionId;
    QString m_queuedPlaybackId;
    QString m_playbackAfterStopId;
    QString m_libraryCachePath;
};
