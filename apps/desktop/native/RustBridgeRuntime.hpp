#pragma once

#include "BackendPorts.hpp"

#include <QByteArray>
#include <QLibrary>
#include <QString>
#include <QVariantMap>

struct YanamiOperationResult
{
    int status = 1;
    QByteArray payload;
    QString errorCode;
    QString error;
};

struct YanamiStatusResult
{
    int value = -1;
    QString errorCode;
    QString error;
};

// The only C++ class that knows the Rust dynamic-library ABI. Feature code
// invokes named, typed methods and never resolves symbols or owns C strings.
class RustBridgeRuntime final
{
public:
    enum class CatalogQuery {
        Library,
        Activity,
        Favorites,
        Collection,
    };

    RustBridgeRuntime() = default;
    ~RustBridgeRuntime();

    RustBridgeRuntime(const RustBridgeRuntime &) = delete;
    RustBridgeRuntime &operator=(const RustBridgeRuntime &) = delete;

    bool load(const QString &libraryPath, QString *errorMessage);
    YanamiStatusResult open(const QString &dataDirectory);
    bool ready() const { return m_backend != nullptr; }
    void cancelAll();
    void close();

    YanamiStatusResult dandanplayCredentialSource() const;
    YanamiStatusResult embyConnected() const;
    YanamiOperationResult configureDandanplay(
        const QString &appId,
        const QString &appSecret) const;
    YanamiOperationResult clearDandanplay() const;
    YanamiOperationResult loginEmby(
        const QString &serverName,
        const QString &serverUrl,
        const QString &userName,
        const QString &password,
        bool allowInsecureHttp) const;
    YanamiOperationResult logoutEmby() const;
    YanamiOperationResult embySettings() const;
    YanamiOperationResult refreshProgress() const;
    YanamiOperationResult catalog(
        CatalogQuery query,
        const QString &parentId = {}) const;
    YanamiOperationResult searchCatalog(const QString &query) const;
    YanamiOperationResult hydrateCatalogSearchImages(
        const QVariantMap &request) const;
    YanamiOperationResult media(
        MediaPort::Operation operation,
        const QString &itemId,
        const QVariantMap &payload = {}) const;
    YanamiOperationResult danmaku(
        DanmakuPort::Operation operation,
        const QString &itemId,
        const QVariantMap &payload = {}) const;
    YanamiOperationResult playbackRequest(
        const QString &itemId,
        const QVariantMap &context) const;
    YanamiOperationResult reportPlayback(
        PlaybackPort::Event event,
        const PlaybackPort::Snapshot &snapshot) const;

private:
    using BackendAbiVersion = quint32 (*)();
    using BackendNew = void *(*)(const char *, char **);
    using BackendFree = void (*)(void *);
    using BackendCancelAll = void (*)(void *);
    using StringFree = void (*)(char *);
    using Status = int (*)(void *, char **);
    using ConfigureDanmaku = int (*)(
        void *, const char *, const char *, char **);
    using LoginEmby = int (*)(void *, const char *, const char *,
        const char *, const char *, int, char **, char **);
    using JsonOperation = int (*)(void *, char **, char **);
    using ItemJsonOperation = int (*)(
        void *, const char *, char **, char **);
    using ItemPayloadJsonOperation = int (*)(
        void *, const char *, const char *, char **, char **);
    using ItemBoolJsonOperation = int (*)(
        void *, const char *, int, char **, char **);
    using ItemStatusOperation = int (*)(void *, const char *, char **);
    using ReportPlaybackJson = int (*)(void *, const char *, char **);

    template<typename Function>
    Function resolve(const char *name)
    {
        return reinterpret_cast<Function>(m_library.resolve(name));
    }

    YanamiOperationResult json(JsonOperation operation) const;
    YanamiOperationResult itemJson(
        ItemJsonOperation operation,
        const QString &itemId) const;
    YanamiOperationResult itemPayloadJson(
        ItemPayloadJsonOperation operation,
        const QString &itemId,
        const QVariantMap &payload) const;
    YanamiOperationResult itemBoolJson(
        ItemBoolJsonOperation operation,
        const QString &itemId,
        bool value) const;
    YanamiOperationResult finish(
        int status,
        char *payload,
        char *error) const;
    QString takeString(char *value) const;
    QString takeError(char *value, QString *code = nullptr) const;
    void resetResolvedSymbols();

    QLibrary m_library;
    void *m_backend = nullptr;
    BackendNew m_new = nullptr;
    BackendFree m_free = nullptr;
    BackendCancelAll m_cancelAll = nullptr;
    StringFree m_stringFree = nullptr;
    Status m_dandanplayCredentialSource = nullptr;
    Status m_embyConnected = nullptr;
    ConfigureDanmaku m_configureDandanplay = nullptr;
    Status m_clearDandanplay = nullptr;
    LoginEmby m_loginEmby = nullptr;
    Status m_logoutEmby = nullptr;
    JsonOperation m_embySettings = nullptr;
    JsonOperation m_refreshProgress = nullptr;
    JsonOperation m_libraryJson = nullptr;
    JsonOperation m_activity = nullptr;
    JsonOperation m_favorites = nullptr;
    ItemJsonOperation m_collection = nullptr;
    ItemJsonOperation m_catalogSearch = nullptr;
    ItemStatusOperation m_catalogSearchHydrateImages = nullptr;
    ItemJsonOperation m_metadata = nullptr;
    ItemPayloadJsonOperation m_updateMetadata = nullptr;
    ItemJsonOperation m_playlistTargets = nullptr;
    ItemPayloadJsonOperation m_addToPlaylist = nullptr;
    ItemPayloadJsonOperation m_removeFromPlaylist = nullptr;
    ItemJsonOperation m_imageEditor = nullptr;
    ItemJsonOperation m_imageProviders = nullptr;
    ItemPayloadJsonOperation m_imageSearch = nullptr;
    ItemPayloadJsonOperation m_imageApply = nullptr;
    ItemPayloadJsonOperation m_imageUpload = nullptr;
    ItemPayloadJsonOperation m_imageDelete = nullptr;
    ItemPayloadJsonOperation m_refreshMetadata = nullptr;
    ItemBoolJsonOperation m_setPlayed = nullptr;
    ItemBoolJsonOperation m_setFavorite = nullptr;
    ItemJsonOperation m_scanLibraryFiles = nullptr;
    ItemJsonOperation m_deleteItem = nullptr;
    ItemPayloadJsonOperation m_danmakuSearch = nullptr;
    ItemJsonOperation m_danmakuAuto = nullptr;
    ItemPayloadJsonOperation m_danmakuApply = nullptr;
    ItemPayloadJsonOperation m_playbackRequest = nullptr;
    ReportPlaybackJson m_reportPlayback = nullptr;
};
