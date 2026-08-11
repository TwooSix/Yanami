#include "BackendController.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrentRun>

namespace {

constexpr qint64 collectionCacheTtlMs = 5 * 60 * 1000;
constexpr qint64 homeCacheFreshMs = 5 * 60 * 1000;

QString bridgeFileName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("yanami_desktop_bridge.dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("libyanami_desktop_bridge.dylib");
#else
    return QStringLiteral("libyanami_desktop_bridge.so");
#endif
}

} // namespace

BackendController::BackendController(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<YanamiOperationResult>::finished,
            this, &BackendController::finishOperation);
    connect(&m_activityWatcher, &QFutureWatcher<YanamiOperationResult>::finished,
            this, &BackendController::finishActivityRefresh);
    connect(&m_playbackReportWatcher, &QFutureWatcher<YanamiOperationResult>::finished,
            this, &BackendController::finishPlaybackReport);

    m_library.setFileName(QDir(QCoreApplication::applicationDirPath()).filePath(bridgeFileName()));
    if (!m_library.load()) {
        setStatus(tr("Unable to load the Rust backend: %1").arg(m_library.errorString()), true);
        return;
    }

    m_new = resolve<BackendNew>("yanami_backend_new");
    m_free = resolve<BackendFree>("yanami_backend_free");
    m_stringFree = resolve<StringFree>("yanami_string_free");
    m_danmakuCredentialSourceStatus =
        resolve<Status>("yanami_backend_dandanplay_credential_source");
    m_embyStatus = resolve<Status>("yanami_backend_emby_connected");
    m_configureDanmaku = resolve<ConfigureDanmaku>("yanami_backend_configure_dandanplay");
    m_clearDanmaku = resolve<Status>("yanami_backend_clear_dandanplay");
    m_loginEmby = resolve<LoginEmby>("yanami_backend_login_emby");
    m_logoutEmby = resolve<Status>("yanami_backend_logout_emby");
    m_libraryJson = resolve<JsonOperation>("yanami_backend_library_json");
    m_activityJson = resolve<JsonOperation>("yanami_backend_activity_json");
    m_collectionJson = resolve<ItemJsonOperation>("yanami_backend_collection_json");
    m_playbackJson = resolve<ItemJsonOperation>("yanami_backend_playback_json");
    m_reportPlayback = resolve<ReportPlayback>("yanami_backend_report_playback");
    if (!m_new || !m_free || !m_stringFree || !m_danmakuCredentialSourceStatus || !m_embyStatus || !m_configureDanmaku
        || !m_clearDanmaku || !m_loginEmby || !m_logoutEmby || !m_libraryJson || !m_activityJson
        || !m_collectionJson || !m_playbackJson || !m_reportPlayback) {
        setStatus(tr("The Rust backend has an incompatible API."), true);
        return;
    }

    const QString dataLocation = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(dataLocation)) {
        setStatus(tr("Unable to create the application data folder."), true);
        return;
    }
    const QString cacheDirectory = QDir(dataLocation).filePath(QStringLiteral("cache"));
    if (!QDir().mkpath(cacheDirectory)) {
        setStatus(tr("Unable to create the application data folder."), true);
        return;
    }
    m_libraryCachePath = QDir(cacheDirectory).filePath(QStringLiteral("home-library.json"));
    const QByteArray encodedPath = QFile::encodeName(dataLocation);
    char *error = nullptr;
    m_backend = m_new(encodedPath.constData(), &error);
    if (!m_backend) {
        setStatus(takeString(error), true);
        return;
    }
    emit readyChanged();
    refreshDanmakuStatus();
    error = nullptr;
    const int embyStatus = m_embyStatus(m_backend, &error);
    if (embyStatus < 0) {
        setStatus(takeString(error), true);
    } else if (embyStatus == 1) {
        m_embyConnected = true;
        emit embyConnectedChanged();
        const bool cacheLoaded = loadLibraryCache();
        const bool cacheIsFresh = cacheLoaded
            && QDateTime::currentMSecsSinceEpoch() - m_lastFullLibraryRefreshMs < homeCacheFreshMs;
        if (cacheIsFresh)
            QTimer::singleShot(0, this, &BackendController::refreshActivity);
        else
            QTimer::singleShot(0, this, &BackendController::loadLibrary);
    }
}

BackendController::~BackendController()
{
    m_playbackReportQueue.clear();
    if (m_activityWatcher.isRunning())
        m_activityWatcher.waitForFinished();
    if (m_playbackReportWatcher.isRunning())
        m_playbackReportWatcher.waitForFinished();
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
    if (m_backend && m_free)
        m_free(m_backend);
    m_backend = nullptr;
    m_library.unload();
}

void BackendController::configureDandanplay(const QString &appId, const QString &appSecret)
{
    if (!ready() || m_busy)
        return;
    QByteArray id = appId.trimmed().toUtf8();
    QByteArray secret = appSecret.toUtf8();
    if (id.isEmpty() || secret.isEmpty()) {
        setStatus(tr("AppId and AppSecret are required."), true);
        return;
    }
    start(Operation::ConfigureDanmaku, [this, id = std::move(id), secret = std::move(secret)]() mutable {
        char *error = nullptr;
        const int status = m_configureDanmaku(m_backend, id.constData(), secret.constData(), &error);
        secret.fill('\0');
        return result(status, nullptr, error);
    });
}

void BackendController::clearDandanplay()
{
    if (!ready() || m_busy)
        return;
    start(Operation::ClearDanmaku, [this] {
        char *error = nullptr;
        return result(m_clearDanmaku(m_backend, &error), nullptr, error);
    });
}

void BackendController::loginEmby(
    const QString &serverName,
    const QString &serverUrl,
    const QString &username,
    const QString &password)
{
    if (!ready() || m_busy)
        return;
    QByteArray name = serverName.trimmed().toUtf8();
    QByteArray url = serverUrl.trimmed().toUtf8();
    QByteArray user = username.trimmed().toUtf8();
    QByteArray secret = password.toUtf8();
    if (name.isEmpty() || url.isEmpty() || user.isEmpty()) {
        setStatus(tr("Server name, URL, and username are required."), true);
        return;
    }
    start(Operation::LoginEmby,
          [this, name = std::move(name), url = std::move(url), user = std::move(user),
           secret = std::move(secret)]() mutable {
              char *error = nullptr;
              const int status = m_loginEmby(
                  m_backend, name.constData(), url.constData(), user.constData(),
                  secret.constData(), &error);
              secret.fill('\0');
              return result(status, nullptr, error);
          });
}

void BackendController::logoutEmby()
{
    if (!ready() || m_busy || !m_embyConnected)
        return;
    start(Operation::LogoutEmby, [this] {
        char *error = nullptr;
        return result(m_logoutEmby(m_backend, &error), nullptr, error);
    });
}

void BackendController::loadLibrary()
{
    if (!ready() || m_busy || !m_embyConnected)
        return;
    const bool blocking = m_libraryViews.isEmpty() && m_mediaItems.isEmpty();
    start(Operation::LoadLibrary, [this] {
        char *payload = nullptr;
        char *error = nullptr;
        const int status = m_libraryJson(m_backend, &payload, &error);
        return result(status, payload, error);
    }, blocking);
}

void BackendController::refreshActivity()
{
    if (!ready() || !m_embyConnected || !m_activityJson)
        return;
    if (m_activityWatcher.isRunning()) {
        m_activityRefreshQueued = true;
        return;
    }
    if (m_busy && (m_operation == Operation::LoginEmby || m_operation == Operation::LogoutEmby))
        return;
    m_activityRefreshQueued = false;
    m_activityWatcher.setFuture(QtConcurrent::run([this] {
        char *payload = nullptr;
        char *error = nullptr;
        const int status = m_activityJson(m_backend, &payload, &error);
        return result(status, payload, error);
    }));
}

void BackendController::loadCollection(const QString &parentId)
{
    if (!ready() || !m_embyConnected || parentId.isEmpty())
        return;
    if (m_busy) {
        if (!m_blockingBusy) {
            m_queuedCollectionId = parentId;
            m_queuedPlaybackId.clear();
        }
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto cached = m_collectionCache.constFind(parentId);
    const bool hasCachedCollection = cached != m_collectionCache.cend();
    if (hasCachedCollection) {
        m_collectionItems = cached->items;
        m_collectionParent = cached->parent;
        emit collectionItemsChanged();
        if (now - cached->loadedAtMs < collectionCacheTtlMs)
            return;
    }
    const QByteArray id = parentId.toUtf8();
    m_pendingCollectionId = parentId;
    if (!hasCachedCollection) {
        m_collectionItems.clear();
        m_collectionParent.clear();
        emit collectionItemsChanged();
    }
    start(Operation::LoadCollection, [this, id] {
        char *payload = nullptr;
        char *error = nullptr;
        const int status = m_collectionJson(m_backend, id.constData(), &payload, &error);
        return result(status, payload, error);
    }, !hasCachedCollection);
}

void BackendController::preparePlayback(const QString &itemId)
{
    if (!ready() || itemId.isEmpty())
        return;
    if (m_busy) {
        if (!m_blockingBusy) {
            m_queuedPlaybackId = itemId;
            m_queuedCollectionId.clear();
        }
        return;
    }
    const QByteArray id = itemId.toUtf8();
    start(Operation::PreparePlayback, [this, id] {
        char *payload = nullptr;
        char *error = nullptr;
        const int status = m_playbackJson(m_backend, id.constData(), &payload, &error);
        return result(status, payload, error);
    });
}

void BackendController::switchPlayback(
    const QString &itemId,
    double positionSeconds,
    bool paused)
{
    if (!ready() || itemId.isEmpty())
        return;
    m_playbackAfterStopId = itemId;
    reportPlayback(QStringLiteral("stopped"), positionSeconds, paused);
}

void BackendController::reportPlayback(const QString &event, double positionSeconds, bool paused)
{
    if (!ready() || !m_embyConnected || event.isEmpty())
        return;
    YanamiPlaybackReportRequest request{
        event.toUtf8(),
        static_cast<quint64>(qMax(0.0, positionSeconds) * 10000000.0),
        paused,
    };
    if (request.event == QByteArrayLiteral("progress") && !m_playbackReportQueue.isEmpty()
        && m_playbackReportQueue.back().event == QByteArrayLiteral("progress")) {
        m_playbackReportQueue.back() = request;
    } else {
        m_playbackReportQueue.enqueue(request);
    }
    startNextPlaybackReport();
}

void BackendController::startNextPlaybackReport()
{
    if (m_playbackReportInFlight || m_playbackReportQueue.isEmpty()
        || !m_backend || !m_reportPlayback) {
        return;
    }
    m_activePlaybackReport = m_playbackReportQueue.dequeue();
    const YanamiPlaybackReportRequest request = m_activePlaybackReport;
    m_playbackReportInFlight = true;
    m_playbackReportWatcher.setFuture(QtConcurrent::run([this, request] {
        char *error = nullptr;
        const int status = m_reportPlayback(
            m_backend,
            request.event.constData(),
            request.positionTicks,
            request.paused ? 1 : 0,
            &error);
        return result(status, nullptr, error);
    }));
}

void BackendController::finishPlaybackReport()
{
    const YanamiOperationResult operationResult = m_playbackReportWatcher.result();
    const bool stopped = m_activePlaybackReport.event == QByteArrayLiteral("stopped");
    m_playbackReportInFlight = false;
    if (operationResult.status != 0) {
        setStatus(
            operationResult.error.isEmpty()
                ? tr("Unable to synchronize playback progress with Emby.")
                : operationResult.error,
            true);
    } else if (stopped) {
        setStatus(tr("Playback position synchronized with Emby."));
        emit playbackStoppedReported();
    }
    const QString playbackAfterStop = stopped ? m_playbackAfterStopId : QString();
    if (stopped)
        m_playbackAfterStopId.clear();
    m_activePlaybackReport = {};
    startNextPlaybackReport();
    if (!playbackAfterStop.isEmpty()) {
        QTimer::singleShot(0, this, [this, playbackAfterStop] {
            preparePlayback(playbackAfterStop);
        });
    }
}

void BackendController::start(
    Operation operation,
    std::function<YanamiOperationResult()> work,
    bool blocking)
{
    m_operation = operation;
    m_blockingBusy = blocking;
    m_busy = true;
    emit busyChanged();
    if (blocking)
        setStatus(tr("Working…"));
    m_watcher.setFuture(QtConcurrent::run(std::move(work)));
}

void BackendController::finishOperation()
{
    const YanamiOperationResult operationResult = m_watcher.result();
    const Operation completed = m_operation;
    m_operation = Operation::None;
    m_busy = false;
    m_blockingBusy = false;
    emit busyChanged();
    QTimer::singleShot(0, this, &BackendController::startQueuedUserOperation);

    if (operationResult.status != 0) {
        setStatus(operationResult.error.isEmpty() ? tr("The operation failed.") : operationResult.error, true);
        return;
    }

    switch (completed) {
    case Operation::ConfigureDanmaku:
        setStatus(tr("DanDanPlay credentials validated and saved securely."));
        refreshDanmakuStatus();
        break;
    case Operation::ClearDanmaku:
        setStatus(tr("DanDanPlay credentials removed."));
        refreshDanmakuStatus();
        break;
    case Operation::LoginEmby:
        m_embyConnected = true;
        emit embyConnectedChanged();
        setStatus(tr("Connected to Emby."));
        QTimer::singleShot(0, this, &BackendController::loadLibrary);
        break;
    case Operation::LogoutEmby:
        m_embyConnected = false;
        m_mediaItems.clear();
        m_libraryViews.clear();
        m_resumeItems.clear();
        m_recentItems.clear();
        m_collectionItems.clear();
        m_collectionParent.clear();
        m_collectionCache.clear();
        clearLibraryCache();
        emit embyConnectedChanged();
        emit mediaItemsChanged();
        emit libraryViewsChanged();
        emit resumeItemsChanged();
        emit recentItemsChanged();
        emit collectionItemsChanged();
        setStatus(tr("Disconnected from Emby and removed the saved token."));
        break;
    case Operation::LoadLibrary: {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(operationResult.payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            setStatus(tr("The media library response was invalid."), true);
            return;
        }
        if (!applyLibraryObject(document.object())) {
            setStatus(tr("The media library response was invalid."), true);
            return;
        }
        m_lastFullLibraryRefreshMs = QDateTime::currentMSecsSinceEpoch();
        saveLibraryCache();
        setStatus(tr("Loaded %1 libraries, %2 titles and %3 recent episodes.")
                      .arg(m_libraryViews.size())
                      .arg(m_mediaItems.size())
                      .arg(m_recentItems.size()));
        break;
    }
    case Operation::LoadCollection: {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(operationResult.payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            setStatus(tr("The collection response was invalid."), true);
            return;
        }
        const QJsonObject object = document.object();
        m_collectionParent = object.value(QStringLiteral("parent")).toObject().toVariantMap();
        m_collectionItems = object.value(QStringLiteral("items")).toArray().toVariantList();
        if (!m_pendingCollectionId.isEmpty()) {
            m_collectionCache.insert(m_pendingCollectionId, CachedCollection{
                m_collectionItems,
                m_collectionParent,
                QDateTime::currentMSecsSinceEpoch(),
            });
        }
        m_pendingCollectionId.clear();
        emit collectionItemsChanged();
        setStatus(tr("Loaded %1 items.").arg(m_collectionItems.size()));
        break;
    }
    case Operation::PreparePlayback: {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(operationResult.payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            setStatus(tr("The playback response was invalid."), true);
            return;
        }
        const QJsonObject object = document.object();
        const QUrl mediaUrl(object.value(QStringLiteral("url")).toString());
        if (!mediaUrl.isValid()) {
            setStatus(tr("Emby returned an invalid playback URL."), true);
            return;
        }
        const QString warning = object.value(QStringLiteral("danmakuWarning")).toString();
        setStatus(warning.isEmpty() ? tr("Playback ready.")
                                    : tr("Playback ready; danmaku unavailable: %1").arg(warning),
                  false);
        const QString danmakuPath = object.value(QStringLiteral("danmakuFile")).toString();
        const QJsonValue introStartValue = object.value(QStringLiteral("introStartTicks"));
        const QJsonValue introEndValue = object.value(QStringLiteral("introEndTicks"));
        emit playbackReady(
            mediaUrl,
            object.value(QStringLiteral("headers")).toObject().toVariantMap(),
            object.value(QStringLiteral("resumeTicks")).toVariant().toLongLong(),
            object.value(QStringLiteral("title")).toString(),
            object.value(QStringLiteral("previousItem")).toObject().toVariantMap(),
            object.value(QStringLiteral("nextItem")).toObject().toVariantMap(),
            object.value(QStringLiteral("externalSubtitles")).toArray().toVariantList(),
            introStartValue.isDouble() ? introStartValue.toVariant().toLongLong() : -1,
            introEndValue.isDouble() ? introEndValue.toVariant().toLongLong() : -1,
            danmakuPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(danmakuPath));
        break;
    }
    case Operation::None:
        break;
    }
}

void BackendController::finishActivityRefresh()
{
    const YanamiOperationResult operationResult = m_activityWatcher.result();
    if (operationResult.status == 0) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(operationResult.payload, &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()
            && applyActivityObject(document.object())) {
            saveLibraryCache();
        }
    }
    if (m_activityRefreshQueued) {
        m_activityRefreshQueued = false;
        QTimer::singleShot(0, this, &BackendController::refreshActivity);
    }
}

bool BackendController::applyLibraryObject(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("library")).isArray()
        || !object.value(QStringLiteral("views")).isArray()
        || !object.value(QStringLiteral("resume")).isArray()
        || !object.value(QStringLiteral("recent")).isArray()) {
        return false;
    }
    const QVariantList mediaItems = object.value(QStringLiteral("library")).toArray().toVariantList();
    const QVariantList libraryViews = object.value(QStringLiteral("views")).toArray().toVariantList();
    if (m_mediaItems != mediaItems) {
        m_mediaItems = mediaItems;
        emit mediaItemsChanged();
    }
    if (m_libraryViews != libraryViews) {
        m_libraryViews = libraryViews;
        emit libraryViewsChanged();
    }
    return applyActivityObject(object);
}

bool BackendController::applyActivityObject(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("resume")).isArray()
        || !object.value(QStringLiteral("recent")).isArray()) {
        return false;
    }
    const QVariantList resumeItems = object.value(QStringLiteral("resume")).toArray().toVariantList();
    const QVariantList recentItems = object.value(QStringLiteral("recent")).toArray().toVariantList();
    if (m_resumeItems != resumeItems) {
        m_resumeItems = resumeItems;
        emit resumeItemsChanged();
    }
    if (m_recentItems != recentItems) {
        m_recentItems = recentItems;
        emit recentItemsChanged();
    }
    return true;
}

bool BackendController::loadLibraryCache()
{
    QFile cache(m_libraryCachePath);
    if (!cache.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(cache.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject object = document.object();
    m_lastFullLibraryRefreshMs = object.value(QStringLiteral("fullRefreshedAtMs")).toVariant().toLongLong();
    return applyLibraryObject(object);
}

void BackendController::saveLibraryCache() const
{
    if (m_libraryCachePath.isEmpty())
        return;
    QJsonObject object;
    object.insert(QStringLiteral("library"), QJsonArray::fromVariantList(m_mediaItems));
    object.insert(QStringLiteral("views"), QJsonArray::fromVariantList(m_libraryViews));
    object.insert(QStringLiteral("resume"), QJsonArray::fromVariantList(m_resumeItems));
    object.insert(QStringLiteral("recent"), QJsonArray::fromVariantList(m_recentItems));
    object.insert(QStringLiteral("fullRefreshedAtMs"), m_lastFullLibraryRefreshMs);
    QSaveFile cache(m_libraryCachePath);
    if (!cache.open(QIODevice::WriteOnly))
        return;
    cache.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    cache.commit();
}

void BackendController::clearLibraryCache()
{
    if (!m_libraryCachePath.isEmpty())
        QFile::remove(m_libraryCachePath);
}

void BackendController::startQueuedUserOperation()
{
    if (m_busy)
        return;
    if (!m_queuedPlaybackId.isEmpty()) {
        const QString itemId = m_queuedPlaybackId;
        m_queuedPlaybackId.clear();
        preparePlayback(itemId);
        return;
    }
    if (!m_queuedCollectionId.isEmpty()) {
        const QString parentId = m_queuedCollectionId;
        m_queuedCollectionId.clear();
        loadCollection(parentId);
    }
}

YanamiOperationResult BackendController::result(int status, char *payload, char *error) const
{
    YanamiOperationResult value;
    value.status = status;
    if (payload) {
        value.payload = QByteArray(payload);
        m_stringFree(payload);
    }
    value.error = takeString(error);
    return value;
}

QString BackendController::takeString(char *value) const
{
    if (!value)
        return {};
    const QString result = QString::fromUtf8(value);
    m_stringFree(value);
    return result;
}

void BackendController::setStatus(const QString &message, bool error)
{
    if (m_statusMessage == message && m_statusIsError == error)
        return;
    m_statusMessage = message;
    m_statusIsError = error;
    emit statusMessageChanged();
}

void BackendController::clearStatus()
{
    setStatus(QString());
}

void BackendController::refreshDanmakuStatus()
{
    if (!m_backend || !m_danmakuCredentialSourceStatus)
        return;
    char *error = nullptr;
    const int status = m_danmakuCredentialSourceStatus(m_backend, &error);
    if (status < 0) {
        setStatus(takeString(error), true);
        return;
    }
    const bool configured = status > 0;
    if (m_danmakuConfigured != configured || m_danmakuCredentialSource != status) {
        m_danmakuConfigured = configured;
        m_danmakuCredentialSource = status;
        emit danmakuConfiguredChanged();
    }
}
