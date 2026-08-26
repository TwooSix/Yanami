#include "RustBridgeRuntime.hpp"

#include "PlaybackTelemetry.hpp"
#include "RustBridgeError.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace {

constexpr quint32 expectedAbiVersion = 3;

QByteArray compactJson(const QVariantMap &value)
{
    return QJsonDocument(QJsonObject::fromVariantMap(value))
        .toJson(QJsonDocument::Compact);
}

} // namespace

RustBridgeRuntime::~RustBridgeRuntime()
{
    close();
}

bool RustBridgeRuntime::load(
    const QString &libraryPath,
    QString *errorMessage)
{
    close();
    m_library.setFileName(libraryPath);
    if (!m_library.load()) {
        if (errorMessage)
            *errorMessage = m_library.errorString();
        return false;
    }

    const BackendAbiVersion abiVersion =
        resolve<BackendAbiVersion>("yanami_backend_abi_version");
    if (!abiVersion || abiVersion() != expectedAbiVersion) {
        if (errorMessage)
            *errorMessage = QStringLiteral("incompatible ABI version");
        m_library.unload();
        return false;
    }

    m_new = resolve<BackendNew>("yanami_backend_new");
    m_free = resolve<BackendFree>("yanami_backend_free");
    m_cancelAll = resolve<BackendCancelAll>("yanami_backend_cancel_all");
    m_stringFree = resolve<StringFree>("yanami_string_free");
    m_dandanplayCredentialSource = resolve<Status>(
        "yanami_backend_dandanplay_credential_source");
    m_embyConnected = resolve<Status>("yanami_backend_emby_connected");
    m_configureDandanplay = resolve<ConfigureDanmaku>(
        "yanami_backend_configure_dandanplay");
    m_clearDandanplay = resolve<Status>(
        "yanami_backend_clear_dandanplay");
    m_loginEmby = resolve<LoginEmby>("yanami_backend_login_emby");
    m_logoutEmby = resolve<Status>("yanami_backend_logout_emby");
    m_embySettings = resolve<JsonOperation>(
        "yanami_backend_emby_settings_json");
    m_refreshProgress = resolve<JsonOperation>(
        "yanami_backend_refresh_progress_json");
    m_libraryJson = resolve<JsonOperation>("yanami_backend_library_json");
    m_activity = resolve<JsonOperation>("yanami_backend_activity_json");
    m_favorites = resolve<JsonOperation>("yanami_backend_favorites_json");
    m_collection = resolve<ItemJsonOperation>(
        "yanami_backend_collection_json");
    m_catalogSearch = resolve<ItemJsonOperation>(
        "yanami_backend_catalog_search_json");
    m_catalogSearchHydrateImages = resolve<ItemStatusOperation>(
        "yanami_backend_catalog_search_hydrate_images");
    m_metadata = resolve<ItemJsonOperation>("yanami_backend_metadata_json");
    m_updateMetadata = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_update_metadata_json");
    m_playlistTargets = resolve<ItemJsonOperation>(
        "yanami_backend_playlist_targets_json");
    m_addToPlaylist = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_add_to_playlist_json");
    m_removeFromPlaylist = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_remove_from_playlist_json");
    m_imageEditor = resolve<ItemJsonOperation>(
        "yanami_backend_image_editor_json");
    m_imageProviders = resolve<ItemJsonOperation>(
        "yanami_backend_image_providers_json");
    m_imageSearch = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_image_search_json");
    m_imageApply = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_image_apply_json");
    m_imageUpload = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_image_upload_json");
    m_imageDelete = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_image_delete_json");
    m_refreshMetadata = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_refresh_metadata_json");
    m_setPlayed = resolve<ItemBoolJsonOperation>(
        "yanami_backend_set_played_json");
    m_setFavorite = resolve<ItemBoolJsonOperation>(
        "yanami_backend_set_favorite_json");
    m_scanLibraryFiles = resolve<ItemJsonOperation>(
        "yanami_backend_scan_library_files_json");
    m_deleteItem = resolve<ItemJsonOperation>(
        "yanami_backend_delete_item_json");
    m_danmakuSearch = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_danmaku_search_json");
    m_danmakuAuto = resolve<ItemJsonOperation>(
        "yanami_backend_danmaku_auto_json");
    m_danmakuApply = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_danmaku_apply_json");
    m_playbackRequest = resolve<ItemPayloadJsonOperation>(
        "yanami_backend_playback_request_json");
    m_reportPlayback = resolve<ReportPlaybackJson>(
        "yanami_backend_report_playback_json");

    const bool complete = m_new && m_free && m_cancelAll && m_stringFree
        && m_dandanplayCredentialSource && m_embyConnected
        && m_configureDandanplay && m_clearDandanplay && m_loginEmby
        && m_logoutEmby && m_embySettings && m_refreshProgress
        && m_libraryJson && m_activity && m_favorites && m_collection
        && m_catalogSearch && m_catalogSearchHydrateImages
        && m_metadata && m_updateMetadata
        && m_playlistTargets && m_addToPlaylist && m_removeFromPlaylist
        && m_imageEditor && m_imageProviders && m_imageSearch
        && m_imageApply && m_imageUpload && m_imageDelete
        && m_refreshMetadata && m_setPlayed && m_setFavorite
        && m_scanLibraryFiles && m_deleteItem && m_danmakuSearch
        && m_danmakuAuto && m_danmakuApply && m_playbackRequest
        && m_reportPlayback;
    if (!complete) {
        if (errorMessage)
            *errorMessage = QStringLiteral("incomplete ABI v3 symbol set");
        close();
        return false;
    }
    return true;
}

YanamiStatusResult RustBridgeRuntime::open(const QString &dataDirectory)
{
    YanamiStatusResult result;
    if (m_backend) {
        result.errorCode = QStringLiteral("bridge_already_open");
        result.error = QStringLiteral("The Rust backend is already open.");
        return result;
    }
    if (!m_new) {
        result.errorCode = QStringLiteral("bridge_unavailable");
        result.error = QStringLiteral("The Rust backend is not loaded.");
        return result;
    }
    char *error = nullptr;
    const QByteArray encodedPath = dataDirectory.toUtf8();
    m_backend = m_new(encodedPath.constData(), &error);
    if (m_backend) {
        result.value = 1;
        return result;
    }
    result.error = takeError(error, &result.errorCode);
    return result;
}

void RustBridgeRuntime::cancelAll()
{
    if (m_backend && m_cancelAll)
        m_cancelAll(m_backend);
}

void RustBridgeRuntime::close()
{
    if (m_backend && m_free)
        m_free(m_backend);
    m_backend = nullptr;
    if (m_library.isLoaded())
        m_library.unload();
    resetResolvedSymbols();
}

void RustBridgeRuntime::resetResolvedSymbols()
{
    m_new = nullptr;
    m_free = nullptr;
    m_cancelAll = nullptr;
    m_stringFree = nullptr;
    m_dandanplayCredentialSource = nullptr;
    m_embyConnected = nullptr;
    m_configureDandanplay = nullptr;
    m_clearDandanplay = nullptr;
    m_loginEmby = nullptr;
    m_logoutEmby = nullptr;
    m_embySettings = nullptr;
    m_refreshProgress = nullptr;
    m_libraryJson = nullptr;
    m_activity = nullptr;
    m_favorites = nullptr;
    m_collection = nullptr;
    m_catalogSearch = nullptr;
    m_catalogSearchHydrateImages = nullptr;
    m_metadata = nullptr;
    m_updateMetadata = nullptr;
    m_playlistTargets = nullptr;
    m_addToPlaylist = nullptr;
    m_removeFromPlaylist = nullptr;
    m_imageEditor = nullptr;
    m_imageProviders = nullptr;
    m_imageSearch = nullptr;
    m_imageApply = nullptr;
    m_imageUpload = nullptr;
    m_imageDelete = nullptr;
    m_refreshMetadata = nullptr;
    m_setPlayed = nullptr;
    m_setFavorite = nullptr;
    m_scanLibraryFiles = nullptr;
    m_deleteItem = nullptr;
    m_danmakuSearch = nullptr;
    m_danmakuAuto = nullptr;
    m_danmakuApply = nullptr;
    m_playbackRequest = nullptr;
    m_reportPlayback = nullptr;
}

YanamiStatusResult RustBridgeRuntime::dandanplayCredentialSource() const
{
    char *error = nullptr;
    YanamiStatusResult result;
    result.value = m_dandanplayCredentialSource
        ? m_dandanplayCredentialSource(m_backend, &error) : -1;
    if (result.value < 0)
        result.error = takeError(error, &result.errorCode);
    else if (error)
        takeString(error);
    return result;
}

YanamiStatusResult RustBridgeRuntime::embyConnected() const
{
    char *error = nullptr;
    YanamiStatusResult result;
    result.value = m_embyConnected
        ? m_embyConnected(m_backend, &error) : -1;
    if (result.value < 0)
        result.error = takeError(error, &result.errorCode);
    else if (error)
        takeString(error);
    return result;
}

YanamiOperationResult RustBridgeRuntime::configureDandanplay(
    const QString &appId,
    const QString &appSecret) const
{
    const QByteArray id = appId.toUtf8();
    QByteArray secret = appSecret.toUtf8();
    char *error = nullptr;
    const int status = m_configureDandanplay(
        m_backend, id.constData(), secret.constData(), &error);
    secret.fill('\0');
    return finish(status, nullptr, error);
}

YanamiOperationResult RustBridgeRuntime::clearDandanplay() const
{
    char *error = nullptr;
    return finish(m_clearDandanplay(m_backend, &error), nullptr, error);
}

YanamiOperationResult RustBridgeRuntime::loginEmby(
    const QString &serverName,
    const QString &serverUrl,
    const QString &userName,
    const QString &password,
    bool allowInsecureHttp) const
{
    const QByteArray name = serverName.toUtf8();
    const QByteArray url = serverUrl.toUtf8();
    const QByteArray user = userName.toUtf8();
    QByteArray secret = password.toUtf8();
    char *payload = nullptr;
    char *error = nullptr;
    const int status = m_loginEmby(m_backend, name.constData(),
        url.constData(), user.constData(), secret.constData(),
        allowInsecureHttp ? 1 : 0, &payload, &error);
    secret.fill('\0');
    return finish(status, payload, error);
}

YanamiOperationResult RustBridgeRuntime::logoutEmby() const
{
    char *error = nullptr;
    return finish(m_logoutEmby(m_backend, &error), nullptr, error);
}

YanamiOperationResult RustBridgeRuntime::embySettings() const
{
    return json(m_embySettings);
}

YanamiOperationResult RustBridgeRuntime::refreshProgress() const
{
    return json(m_refreshProgress);
}

YanamiOperationResult RustBridgeRuntime::catalog(
    CatalogQuery query,
    const QString &parentId) const
{
    switch (query) {
    case CatalogQuery::Library:
        return json(m_libraryJson);
    case CatalogQuery::Activity:
        return json(m_activity);
    case CatalogQuery::Favorites:
        return json(m_favorites);
    case CatalogQuery::Collection:
        return itemJson(m_collection, parentId);
    }
    return {};
}

YanamiOperationResult RustBridgeRuntime::searchCatalog(
    const QString &query) const
{
    return itemJson(m_catalogSearch, query);
}

YanamiOperationResult RustBridgeRuntime::hydrateCatalogSearchImages(
    const QVariantMap &request) const
{
    const QByteArray encodedRequest = compactJson(request);
    char *error = nullptr;
    const int status = m_catalogSearchHydrateImages(
        m_backend, encodedRequest.constData(), &error);
    return finish(status, nullptr, error);
}

YanamiOperationResult RustBridgeRuntime::media(
    MediaPort::Operation operation,
    const QString &itemId,
    const QVariantMap &payload) const
{
    switch (operation) {
    case MediaPort::Operation::LoadMetadata:
        return itemJson(m_metadata, itemId);
    case MediaPort::Operation::UpdateMetadata:
        return itemPayloadJson(m_updateMetadata, itemId, payload);
    case MediaPort::Operation::LoadImages:
        return itemJson(m_imageEditor, itemId);
    case MediaPort::Operation::LoadImageProviders:
        return itemJson(m_imageProviders, itemId);
    case MediaPort::Operation::SearchImages:
        return itemPayloadJson(m_imageSearch, itemId, payload);
    case MediaPort::Operation::ApplyRemoteImage:
        return itemPayloadJson(m_imageApply, itemId, payload);
    case MediaPort::Operation::UploadImage:
        return itemPayloadJson(m_imageUpload, itemId, payload);
    case MediaPort::Operation::RemoveImage:
        return itemPayloadJson(m_imageDelete, itemId, payload);
    case MediaPort::Operation::RefreshMetadata:
        return itemPayloadJson(m_refreshMetadata, itemId, payload);
    case MediaPort::Operation::LoadPlaylistTargets:
        return itemJson(m_playlistTargets, itemId);
    case MediaPort::Operation::AddToPlaylist:
        return itemPayloadJson(m_addToPlaylist, itemId, payload);
    case MediaPort::Operation::RemoveFromPlaylist:
        return itemPayloadJson(m_removeFromPlaylist, itemId, payload);
    case MediaPort::Operation::SetPlayed:
        return itemBoolJson(m_setPlayed, itemId,
            payload.value(QStringLiteral("played")).toBool());
    case MediaPort::Operation::SetFavorite:
        return itemBoolJson(m_setFavorite, itemId,
            payload.value(QStringLiteral("favorite")).toBool());
    case MediaPort::Operation::ScanLibraryFiles:
        return itemJson(m_scanLibraryFiles, itemId);
    case MediaPort::Operation::DeleteItem:
        return itemJson(m_deleteItem, itemId);
    }
    return {};
}

YanamiOperationResult RustBridgeRuntime::danmaku(
    DanmakuPort::Operation operation,
    const QString &itemId,
    const QVariantMap &payload) const
{
    switch (operation) {
    case DanmakuPort::Operation::Search:
        return itemPayloadJson(m_danmakuSearch, itemId, payload);
    case DanmakuPort::Operation::AutomaticLoad:
        return itemJson(m_danmakuAuto, itemId);
    case DanmakuPort::Operation::ApplyMatch:
        return itemPayloadJson(m_danmakuApply, itemId, payload);
    }
    return {};
}

YanamiOperationResult RustBridgeRuntime::playbackRequest(
    const QString &itemId,
    const QVariantMap &context) const
{
    return itemPayloadJson(m_playbackRequest, itemId, context);
}

YanamiOperationResult RustBridgeRuntime::reportPlayback(
    PlaybackPort::Event event,
    const PlaybackPort::Snapshot &snapshot) const
{
    const QVariantMap telemetry = YanamiPlayback::telemetryPayload(
        event, snapshot);
    const QByteArray request = compactJson(telemetry);
    char *error = nullptr;
    const int status = m_reportPlayback(
        m_backend, request.constData(), &error);
    return finish(status, nullptr, error);
}

YanamiOperationResult RustBridgeRuntime::json(
    JsonOperation operation) const
{
    char *payload = nullptr;
    char *error = nullptr;
    const int status = operation(m_backend, &payload, &error);
    return finish(status, payload, error);
}

YanamiOperationResult RustBridgeRuntime::itemJson(
    ItemJsonOperation operation,
    const QString &itemId) const
{
    const QByteArray id = itemId.toUtf8();
    char *payload = nullptr;
    char *error = nullptr;
    const int status = operation(
        m_backend, id.constData(), &payload, &error);
    return finish(status, payload, error);
}

YanamiOperationResult RustBridgeRuntime::itemPayloadJson(
    ItemPayloadJsonOperation operation,
    const QString &itemId,
    const QVariantMap &payload) const
{
    const QByteArray id = itemId.toUtf8();
    const QByteArray request = compactJson(payload);
    char *resultPayload = nullptr;
    char *error = nullptr;
    const int status = operation(m_backend, id.constData(),
        request.constData(), &resultPayload, &error);
    return finish(status, resultPayload, error);
}

YanamiOperationResult RustBridgeRuntime::itemBoolJson(
    ItemBoolJsonOperation operation,
    const QString &itemId,
    bool value) const
{
    const QByteArray id = itemId.toUtf8();
    char *payload = nullptr;
    char *error = nullptr;
    const int status = operation(m_backend, id.constData(), value ? 1 : 0,
        &payload, &error);
    return finish(status, payload, error);
}

YanamiOperationResult RustBridgeRuntime::finish(
    int status,
    char *payload,
    char *error) const
{
    YanamiOperationResult result;
    result.status = status;
    if (payload)
        result.payload = takeString(payload).toUtf8();
    if (error)
        result.error = takeError(error, &result.errorCode);
    return result;
}

QString RustBridgeRuntime::takeString(char *value) const
{
    if (!value)
        return {};
    const QString result = QString::fromUtf8(value);
    if (m_stringFree)
        m_stringFree(value);
    return result;
}

QString RustBridgeRuntime::takeError(char *value, QString *code) const
{
    const YanamiBridgeErrorEnvelope envelope =
        parseRustBridgeError(takeString(value).toUtf8());
    if (code)
        *code = envelope.code;
    return envelope.message;
}
