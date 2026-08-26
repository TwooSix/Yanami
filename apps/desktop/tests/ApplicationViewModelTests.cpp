#include "ApplicationViewModel.hpp"
#include "CatalogFreshnessPolicy.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

namespace {

class FakeSessionPort final : public SessionPort
{
public:
    using SessionPort::SessionPort;

    bool connected() const override { return isConnected; }
    quint64 generation() const override { return sessionGeneration; }
    bool busy() const override { return isBusy; }
    QString displayName() const override { return name; }
    QString serverUrl() const override { return url; }
    QString userName() const override { return user; }
    QString serverDomain() const override { return domain; }
    bool administrator() const override { return isAdministrator; }
    bool canDownload() const override { return downloadAllowed; }
    bool canDelete() const override { return deleteAllowed; }

    void login(const QString &requestId,
        const QString &serverName, const QString &serverUrl,
        const QString &loginUser, const QString &password,
        bool allowInsecureHttp) override
    {
        lastRequestId = requestId;
        loginArguments = {
            {QStringLiteral("serverName"), serverName},
            {QStringLiteral("serverUrl"), serverUrl},
            {QStringLiteral("userName"), loginUser},
            {QStringLiteral("password"), password},
            {QStringLiteral("allowInsecureHttp"), allowInsecureHttp},
        };
    }

    void logout(const QString &requestId) override
    { lastRequestId = requestId; logoutCalled = true; }

    bool isConnected = false;
    quint64 sessionGeneration = 1;
    bool isBusy = false;
    QString name;
    QString url;
    QString user;
    QString domain;
    bool isAdministrator = false;
    bool downloadAllowed = false;
    bool deleteAllowed = false;
    QVariantMap loginArguments;
    QString lastRequestId;
    bool logoutCalled = false;
};

class FakeCatalogPort final : public CatalogPort
{
public:
    using CatalogPort::CatalogPort;

    MediaStore *mediaStore() const override { return nullptr; }
    bool libraryRefreshing() const override { return libraryBusy; }
    bool activityRefreshing() const override { return activityBusy; }
    bool collectionLoading() const override { return collectionLoadBusy; }
    bool collectionFetching() const override { return collectionFetchBusy; }
    bool libraryLoadFailed() const override { return libraryFailed; }
    bool activityLoadFailed() const override { return activityFailed; }
    bool favoritesRefreshing() const override { return favoritesBusy; }
    bool favoritesLoadFailed() const override { return favoritesFailed; }
    QString collectionDisplayedId() const override { return displayedId; }
    QString collectionTargetId() const override { return targetId; }
    QString collectionErrorId() const override { return errorId; }
    QVariantMap collectionParent() const override { return parentItem; }
    RequestDisposition loadLibrary() override { ++loadLibraryCalls; return libraryDisposition; }
    void invalidateActivity() override { ++invalidateActivityCalls; }
    RequestDisposition ensureActivityFresh() override { ++ensureActivityFreshCalls; return activityDisposition; }
    RequestDisposition refreshActivity() override { ++refreshActivityCalls; return activityDisposition; }
    RequestDisposition loadFavorites() override { ++loadFavoritesCalls; return favoritesDisposition; }
    RequestDisposition refreshFavorites() override { ++refreshFavoritesCalls; return favoritesDisposition; }
    RequestDisposition loadCollection(const QString &parentId) override { loadedCollection = parentId; return collectionDisposition; }
    RequestDisposition refreshCollection(const QString &parentId) override { refreshedCollection = parentId; return collectionDisposition; }

    bool libraryBusy = false;
    bool activityBusy = false;
    bool collectionLoadBusy = false;
    bool collectionFetchBusy = false;
    bool libraryFailed = false;
    bool activityFailed = false;
    bool favoritesBusy = false;
    bool favoritesFailed = false;
    QString displayedId;
    QString targetId;
    QString errorId;
    QVariantMap parentItem;
    int loadLibraryCalls = 0;
    int invalidateActivityCalls = 0;
    int ensureActivityFreshCalls = 0;
    int refreshActivityCalls = 0;
    int loadFavoritesCalls = 0;
    int refreshFavoritesCalls = 0;
    QString loadedCollection;
    QString refreshedCollection;
    RequestDisposition libraryDisposition = RequestDisposition::Accepted;
    RequestDisposition activityDisposition = RequestDisposition::Accepted;
    RequestDisposition favoritesDisposition = RequestDisposition::Accepted;
    RequestDisposition collectionDisposition = RequestDisposition::Accepted;
};

class FakeSearchPort final : public SearchPort
{
public:
    FakeSearchPort()
        : model(store.queryModel(QStringLiteral("search")))
        , titleModel(store.queryModel(QStringLiteral("search-titles")))
        , episodeModel(store.queryModel(QStringLiteral("search-episodes")))
    {
    }

    MediaQueryModel *resultsModel() const override { return model; }
    MediaQueryModel *titleResultsModel() const override { return titleModel; }
    MediaQueryModel *episodeResultsModel() const override { return episodeModel; }
    QAbstractItemModel *resultRowsModel() const override { return nullptr; }
    QString query() const override { return currentQuery; }
    bool searching() const override { return isSearching; }
    bool syncing() const override { return isSyncing; }
    bool complete() const override { return isComplete; }
    qint64 cachedCount() const override { return cached; }
    qint64 totalCount() const override { return total; }
    qint64 totalMatches() const override { return matches; }
    bool hasMore() const override { return more; }
    QString error() const override { return currentError; }
    void inputPending() override { ++inputPendingCalls; }
    void requestSearch(const QString &query) override
    {
        currentQuery = query;
        ++searchCalls;
    }
    void refresh() override { ++refreshCalls; }

    MediaStore store;
    MediaQueryModel *model = nullptr;
    MediaQueryModel *titleModel = nullptr;
    MediaQueryModel *episodeModel = nullptr;
    QString currentQuery;
    QString currentError;
    qint64 cached = 0;
    qint64 total = -1;
    qint64 matches = 0;
    int inputPendingCalls = 0;
    int searchCalls = 0;
    int refreshCalls = 0;
    bool isSearching = false;
    bool isSyncing = false;
    bool isComplete = false;
    bool more = false;
};

class FakePlaybackPort final : public PlaybackPort
{
public:
    using PlaybackPort::PlaybackPort;

    void prepare(const QString &requestId, const QString &itemId) override
    { preparedRequestId = requestId; preparedItemId = itemId; }
    void prepareInContext(const QString &requestId, const QString &itemId,
        const QVariantMap &context) override
    { preparedRequestId = requestId; preparedItemId = itemId; preparedContext = context; }
    void cancelPreparation() override { cancelled = true; }
    void switchTo(const QString &requestId, const QString &itemId,
        double positionSeconds, bool paused) override
    { switchedRequestId = requestId; switchedItemId = itemId; switchedPosition = positionSeconds; switchedPaused = paused; }
    void switchToInContext(const QString &requestId, const QString &itemId,
        const QVariantMap &context,
        double positionSeconds, bool paused) override
    { switchedRequestId = requestId; switchedItemId = itemId; switchedContext = context; switchedPosition = positionSeconds; switchedPaused = paused; }
    void report(Event event, const Snapshot &snapshot) override
    { reportedEvent = event; reportedSnapshot = snapshot; }

    QString preparedRequestId;
    QString preparedItemId;
    QVariantMap preparedContext;
    bool cancelled = false;
    QString switchedRequestId;
    QString switchedItemId;
    QVariantMap switchedContext;
    double switchedPosition = 0;
    bool switchedPaused = false;
    Event reportedEvent = Event::Progress;
    Snapshot reportedSnapshot;
};

class FakePlaybackReporter final : public PlaybackReporterPort
{
public:
    bool attachPlayer(QObject *player) override
    {
        attachedPlayer = player;
        return attachResult;
    }

    bool beginSession(
        const QString &reportSessionId,
        const QVariantList &embyTracks) override
    {
        startedSessionId = reportSessionId;
        startedTracks = embyTracks;
        return beginResult;
    }

    void stopSession() override { ++stopCalls; }

    QObject *attachedPlayer = nullptr;
    QString startedSessionId;
    QVariantList startedTracks;
    bool attachResult = true;
    bool beginResult = true;
    int stopCalls = 0;
};

class FakeDanmakuPort final : public DanmakuPort
{
public:
    using DanmakuPort::DanmakuPort;

    bool configured() const override { return isConfigured; }
    int credentialSource() const override { return source; }
    bool configurationBusy() const override { return configBusy; }
    void search(const QString &requestId, const QString &itemId,
        const QString &anime) override
    { lastRequestId = requestId; lastOperation = Operation::Search; lastItemId = itemId; searchAnime = anime; }
    void loadAutomatically(const QString &requestId, const QString &itemId) override
    { lastRequestId = requestId; lastOperation = Operation::AutomaticLoad; lastItemId = itemId; }
    void applyMatch(const QString &requestId, const QString &itemId,
        const QVariantMap &match,
        const QVariantMap &style) override
    { lastRequestId = requestId; lastOperation = Operation::ApplyMatch; lastItemId = itemId; appliedMatch = match; appliedStyle = style; }
    void configure(const QString &requestId,
        const QString &appId, const QString &appSecret) override
    { configurationRequestId = requestId; configuredAppId = appId; configuredSecret = appSecret; }
    void clearConfiguration(const QString &requestId) override
    { configurationRequestId = requestId; clearCalled = true; }

    bool isConfigured = false;
    int source = 0;
    bool configBusy = false;
    Operation lastOperation = Operation::Search;
    QString lastRequestId;
    QString lastItemId;
    QString searchAnime;
    QVariantMap appliedMatch;
    QVariantMap appliedStyle;
    QString configuredAppId;
    QString configuredSecret;
    QString configurationRequestId;
    bool clearCalled = false;
};

struct MediaCall
{
    QString requestId;
    MediaPort::Operation operation;
    QString itemId;
    QVariantMap arguments;
};

class FakeMediaPort final : public MediaPort
{
public:
    using MediaPort::MediaPort;

    QVariantMap libraryScanProgress() const override { return scanProgress; }
    void loadMetadata(const QString &requestId, const QString &itemId, quint64 viewGeneration) override
    { add(requestId, Operation::LoadMetadata, itemId, {{QStringLiteral("viewGeneration"), viewGeneration}}); }
    void updateMetadata(const QString &requestId, const QString &itemId, const QVariantMap &changes) override
    { add(requestId, Operation::UpdateMetadata, itemId, changes); }
    void loadImages(const QString &requestId, const QString &itemId, quint64 viewGeneration) override
    { add(requestId, Operation::LoadImages, itemId, {{QStringLiteral("viewGeneration"), viewGeneration}}); }
    void loadImageProviders(const QString &requestId, const QString &itemId, quint64 viewGeneration) override
    { add(requestId, Operation::LoadImageProviders, itemId, {{QStringLiteral("viewGeneration"), viewGeneration}}); }
    void searchImages(const QString &requestId, const QString &itemId, const QString &imageType,
        const QString &providerName, bool includeAllLanguages,
        bool enableSeriesImages, quint64 viewGeneration,
        quint64 searchGeneration, int startIndex, int limit) override
    {
        add(requestId, Operation::SearchImages, itemId, {
            {QStringLiteral("imageType"), imageType},
            {QStringLiteral("providerName"), providerName},
            {QStringLiteral("includeAllLanguages"), includeAllLanguages},
            {QStringLiteral("enableSeriesImages"), enableSeriesImages},
            {QStringLiteral("viewGeneration"), viewGeneration},
            {QStringLiteral("searchGeneration"), searchGeneration},
            {QStringLiteral("startIndex"), startIndex},
            {QStringLiteral("limit"), limit},
        });
    }
    void applyRemoteImage(const QString &requestId, const QString &itemId, const QString &imageType,
        const QUrl &imageUrl, const QString &providerName,
        const QVariant &imageIndex) override
    { add(requestId, Operation::ApplyRemoteImage, itemId, {{QStringLiteral("imageType"), imageType}, {QStringLiteral("imageUrl"), imageUrl}, {QStringLiteral("providerName"), providerName}, {QStringLiteral("imageIndex"), imageIndex}}); }
    void uploadImage(const QString &requestId, const QString &itemId, const QString &imageType,
        const QUrl &fileUrl, const QVariant &imageIndex) override
    { add(requestId, Operation::UploadImage, itemId, {{QStringLiteral("imageType"), imageType}, {QStringLiteral("fileUrl"), fileUrl}, {QStringLiteral("imageIndex"), imageIndex}}); }
    void removeImage(const QString &requestId, const QString &itemId, const QString &imageType,
        const QVariant &imageIndex) override
    { add(requestId, Operation::RemoveImage, itemId, {{QStringLiteral("imageType"), imageType}, {QStringLiteral("imageIndex"), imageIndex}}); }
    void refreshMetadata(const QString &requestId, const QString &itemId, const QString &mode,
        bool replaceImages, const QString &source) override
    { add(requestId, Operation::RefreshMetadata, itemId, {{QStringLiteral("mode"), mode}, {QStringLiteral("replaceImages"), replaceImages}, {QStringLiteral("source"), source}}); }
    void loadPlaylistTargets(const QString &requestId, const QString &itemId) override
    { add(requestId, Operation::LoadPlaylistTargets, itemId); }
    void addToPlaylist(const QString &requestId, const QString &itemId, const QString &targetId) override
    { add(requestId, Operation::AddToPlaylist, itemId, {{QStringLiteral("targetId"), targetId}}); }
    void createPlaylistAndAdd(const QString &requestId, const QString &itemId, const QString &newName) override
    { add(requestId, Operation::AddToPlaylist, itemId, {{QStringLiteral("newName"), newName}}); }
    void removeFromPlaylist(const QString &requestId, const QString &itemId, const QString &playlistId,
        const QString &entryId) override
    { add(requestId, Operation::RemoveFromPlaylist, itemId, {{QStringLiteral("playlistId"), playlistId}, {QStringLiteral("entryId"), entryId}}); }
    void setPlayed(const QString &requestId, const QString &itemId, bool played) override
    { add(requestId, Operation::SetPlayed, itemId, {{QStringLiteral("played"), played}}); }
    void setFavorite(const QString &requestId, const QString &itemId, bool favorite) override
    { add(requestId, Operation::SetFavorite, itemId, {{QStringLiteral("favorite"), favorite}}); }
    void scanLibraryFiles(const QString &requestId, const QString &itemId) override { add(requestId, Operation::ScanLibraryFiles, itemId); }
    void deleteItem(const QString &requestId, const QString &itemId) override { add(requestId, Operation::DeleteItem, itemId); }

    void add(const QString &requestId, Operation operation, const QString &itemId, const QVariantMap &arguments = {})
    {
        const MediaCall call{requestId, operation, itemId, arguments};
        calls.push_back(call);
        if (onCall)
            onCall(call);
    }

    QVariantMap scanProgress;
    QList<MediaCall> calls;
    std::function<void(const MediaCall &)> onCall;
};

class FakeStatusPort final : public ApplicationStatusPort
{
public:
    using ApplicationStatusPort::ApplicationStatusPort;
    bool ready() const override { return isReady; }
    QString message() const override { return currentMessage; }
    bool error() const override { return isError; }
    void clear() override { currentMessage.clear(); isError = false; emit stateChanged(); }

    bool isReady = true;
    QString currentMessage;
    bool isError = false;
};

struct Fixture
{
    FakeSessionPort session;
    FakeCatalogPort catalog;
    FakeSearchPort search;
    FakePlaybackPort playback;
    FakePlaybackReporter playbackReporter;
    FakeDanmakuPort danmaku;
    FakeMediaPort media;
    FakeStatusPort status;

    BackendPortSet ports()
    {
        return {&session, &catalog, &search, &playback, &playbackReporter,
            &danmaku, &media, &status};
    }
};

} // namespace

class ApplicationViewModelTests final : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_settingsDirectory;

private slots:
    void initTestCase()
    {
        QVERIFY(m_settingsDirectory.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("YanamiTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("ApplicationViewModelTests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
            m_settingsDirectory.path());
    }

    void cleanup()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void ownsDistinctFeatureModels()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());

        QVERIFY(viewModel.session());
        QVERIFY(viewModel.home());
        QVERIFY(viewModel.search());
        QVERIFY(viewModel.favorites());
        QVERIFY(viewModel.playback());
        QVERIFY(viewModel.danmaku());
        QVERIFY(viewModel.mediaActions());
        QVERIFY(viewModel.imageEditor());
        QVERIFY(viewModel.preferences());
        QVERIFY(viewModel.upscaling());
        QVERIFY(viewModel.status());
        QVERIFY(viewModel.updates());
        QCOMPARE(viewModel.session()->parent(), &viewModel);
        QCOMPARE(viewModel.mediaActions()->parent(), &viewModel);
        QCOMPARE(viewModel.upscaling()->parent(), &viewModel);
        QVERIFY(viewModel.upscaling()->metaObject()->indexOfProperty(
            "resolvedPresetId") >= 0);
    }

    void librarySortPreferencePersistsAndRejectsInvalidValues()
    {
        PreferencesViewModel preferences;
        QCOMPARE(preferences.librarySortMode(), 1);
        QSignalSpy changed(
            &preferences, &PreferencesViewModel::librarySortModeChanged);

        preferences.setLibrarySortMode(3);
        QCOMPARE(preferences.librarySortMode(), 3);
        QCOMPARE(changed.count(), 1);
        preferences.setLibrarySortMode(3);
        QCOMPARE(changed.count(), 1);

        PreferencesViewModel restored;
        QCOMPARE(restored.librarySortMode(), 3);

        restored.setLibrarySortMode(-1);
        QCOMPARE(restored.librarySortMode(), 3);
        QSettings().setValue(QStringLiteral("library/sortMode"), 99);
        PreferencesViewModel invalidStoredValue;
        QCOMPARE(invalidStoredValue.librarySortMode(), 1);
    }

    void upscalingPreferencesPersistStableIdsAndSanitizeInput()
    {
        PreferencesViewModel preferences;
        const QVariantMap defaults = preferences.upscalingSettings();
        QCOMPARE(defaults.value(QStringLiteral("enabled")).toBool(), false);
        QCOMPARE(defaults.value(QStringLiteral("providerId")).toString(),
            QStringLiteral("anime4k"));
        QCOMPARE(defaults.value(QStringLiteral("schema")).toInt(), 3);
        QCOMPARE(defaults.value(QStringLiteral("presetId")).toString(),
            QStringLiteral("balanced"));
        QVERIFY(!defaults.contains(QStringLiteral("presetExplicit")));

        QSignalSpy changed(
            &preferences, &PreferencesViewModel::upscalingSettingsChanged);
        preferences.saveUpscalingSettings({
            {QStringLiteral("enabled"), true},
        });
        QCOMPARE(preferences.upscalingSettings()
                     .value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(preferences.upscalingSettings()
                     .value(QStringLiteral("presetId")).toString(),
            QStringLiteral("balanced"));
        QCOMPARE(changed.count(), 1);

        preferences.saveUpscalingSettings({
            {QStringLiteral("enabled"), true},
            {QStringLiteral("providerId"), QStringLiteral("anime4k")},
            {QStringLiteral("presetId"), QStringLiteral("custom")},
            {QStringLiteral("anime4kMode"), QStringLiteral("c")},
            {QStringLiteral("anime4kModelSize"), QStringLiteral("ul")},
            {QStringLiteral("anime4kRestorePasses"), 2},
            {QStringLiteral("anime4kAutoDownscale"), false},
            {QStringLiteral("autoHeadroom"), 35},
        });
        QCOMPARE(changed.count(), 2);

        PreferencesViewModel restored;
        const QVariantMap saved = restored.upscalingSettings();
        QCOMPARE(saved.value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(saved.value(QStringLiteral("providerId")).toString(),
            QStringLiteral("anime4k"));
        QVERIFY(!saved.contains(QStringLiteral("presetExplicit")));
        QCOMPARE(saved.value(QStringLiteral("anime4kMode")).toString(),
            QStringLiteral("c"));
        QCOMPARE(saved.value(QStringLiteral("anime4kModelSize")).toString(),
            QStringLiteral("ul"));
        QCOMPARE(saved.value(QStringLiteral("anime4kRestorePasses")).toInt(), 2);
        QCOMPARE(saved.value(QStringLiteral("anime4kAutoDownscale")).toBool(), false);
        QVERIFY(!saved.contains(QStringLiteral("artcnnModel")));
        QVERIFY(!saved.contains(QStringLiteral("rtxScaleFactor")));

        restored.saveUpscalingSettings({
            {QStringLiteral("providerId"), QStringLiteral("../unsafe")},
            {QStringLiteral("presetId"), QStringLiteral("unknown")},
            {QStringLiteral("anime4kModelSize"), QStringLiteral("xxl")},
            {QStringLiteral("autoHeadroom"), 99},
        });
        const QVariantMap sanitized = restored.upscalingSettings();
        QCOMPARE(sanitized.value(QStringLiteral("providerId")).toString(),
            QStringLiteral("anime4k"));
        QCOMPARE(sanitized.value(QStringLiteral("enabled")).toBool(), false);
        QCOMPARE(sanitized.value(QStringLiteral("presetId")).toString(),
            QStringLiteral("balanced"));
        QCOMPARE(sanitized.value(QStringLiteral("anime4kModelSize")).toString(),
            QStringLiteral("vl"));
        QCOMPARE(sanitized.value(QStringLiteral("autoHeadroom")).toInt(), 20);
    }

    void upscalingSchemaTwoImplicitPresetMigratesToBalanced()
    {
        const auto restore = [](bool presetExplicit, const QString &presetId) {
            QSettings settings;
            settings.clear();
            const QVariantMap previous {
                // The top-level QSettings schema is authoritative. A stale or
                // inconsistent inner value must not bypass migration.
                {QStringLiteral("schema"), 99},
                {QStringLiteral("enabled"), true},
                {QStringLiteral("providerId"), QStringLiteral("anime4k")},
                {QStringLiteral("presetId"), presetId},
                {QStringLiteral("presetExplicit"), presetExplicit},
            };
            settings.setValue(QStringLiteral("playback/upscaling/schema"), 2);
            settings.setValue(
                QStringLiteral("playback/upscaling/settingsJson"),
                QJsonDocument::fromVariant(previous).toJson(
                    QJsonDocument::Compact));
            settings.sync();
            return std::make_unique<PreferencesViewModel>();
        };

        auto automatic = restore(false, QStringLiteral("quality"));
        QCOMPARE(automatic->upscalingSettings()
                     .value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(automatic->upscalingSettings()
                     .value(QStringLiteral("presetId")).toString(),
            QStringLiteral("balanced"));
        QCOMPARE(automatic->upscalingSettings()
                     .value(QStringLiteral("schema")).toInt(), 3);
        QVERIFY(!automatic->upscalingSettings().contains(
            QStringLiteral("presetExplicit")));

        auto explicitPreset = restore(true, QStringLiteral("quality"));
        QCOMPARE(explicitPreset->upscalingSettings()
                     .value(QStringLiteral("presetId")).toString(),
            QStringLiteral("quality"));
        QVERIFY(!explicitPreset->upscalingSettings().contains(
            QStringLiteral("presetExplicit")));
    }

    void upscalingCurrentSchemaPreservesManualPresetAcrossToggle()
    {
        QSettings settings;
        const QVariantMap current {
            // An old inner marker is ignored once the top-level document has
            // already reached the current schema.
            {QStringLiteral("schema"), 2},
            {QStringLiteral("enabled"), false},
            {QStringLiteral("providerId"), QStringLiteral("anime4k")},
            {QStringLiteral("presetId"), QStringLiteral("quality")},
            {QStringLiteral("presetExplicit"), false},
        };
        settings.setValue(QStringLiteral("playback/upscaling/schema"), 3);
        settings.setValue(
            QStringLiteral("playback/upscaling/settingsJson"),
            QJsonDocument::fromVariant(current).toJson(
                QJsonDocument::Compact));
        settings.sync();

        PreferencesViewModel preferences;
        QCOMPARE(preferences.upscalingSettings()
                     .value(QStringLiteral("presetId")).toString(),
            QStringLiteral("quality"));
        QVERIFY(!preferences.upscalingSettings().contains(
            QStringLiteral("presetExplicit")));

        preferences.saveUpscalingSettings({
            {QStringLiteral("enabled"), true},
        });
        QCOMPARE(preferences.upscalingSettings()
                     .value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(preferences.upscalingSettings()
                     .value(QStringLiteral("presetId")).toString(),
            QStringLiteral("quality"));

        preferences.saveUpscalingSettings({
            {QStringLiteral("enabled"), false},
        });
        QCOMPARE(preferences.upscalingSettings()
                     .value(QStringLiteral("presetId")).toString(),
            QStringLiteral("quality"));
    }

    void upscalingSchemaOneProvidersMigrateOnceToAnime4k()
    {
        const QStringList providers {
            QStringLiteral("anime4k"),
            QStringLiteral("auto"),
            QStringLiteral("artcnn"),
            QStringLiteral("rtx"),
        };
        for (const QString &providerId : providers) {
            QSettings settings;
            settings.clear();
            const QVariantMap legacy {
                {QStringLiteral("schema"), 1},
                {QStringLiteral("enabled"), true},
                {QStringLiteral("providerId"), providerId},
                {QStringLiteral("presetId"), QStringLiteral("custom")},
                {QStringLiteral("anime4kMode"), QStringLiteral("c")},
                {QStringLiteral("anime4kModelSize"), QStringLiteral("ul")},
                {QStringLiteral("anime4kRestorePasses"), 2},
                {QStringLiteral("anime4kAutoDownscale"), false},
                {QStringLiteral("artcnnModel"), QStringLiteral("c4f16")},
                {QStringLiteral("rtxScaleFactor"), 4.0},
            };
            settings.setValue(QStringLiteral("playback/upscaling/schema"), 1);
            settings.setValue(
                QStringLiteral("playback/upscaling/settingsJson"),
                QJsonDocument::fromVariant(legacy).toJson(
                    QJsonDocument::Compact));
            settings.sync();

            PreferencesViewModel migrated;
            const QVariantMap result = migrated.upscalingSettings();
            QCOMPARE(result.value(QStringLiteral("schema")).toInt(), 3);
            QCOMPARE(result.value(QStringLiteral("providerId")).toString(),
                QStringLiteral("anime4k"));
            const bool compatible = providerId == QLatin1String("anime4k")
                || providerId == QLatin1String("auto");
            QCOMPARE(result.value(QStringLiteral("enabled")).toBool(),
                compatible);
            const bool explicitPreset = providerId
                == QLatin1String("anime4k");
            QVERIFY(!result.contains(QStringLiteral("presetExplicit")));
            QCOMPARE(result.value(QStringLiteral("presetId")).toString(),
                explicitPreset ? QStringLiteral("custom")
                               : QStringLiteral("balanced"));
            QVERIFY(!result.contains(QStringLiteral("artcnnModel")));
            QVERIFY(!result.contains(QStringLiteral("rtxScaleFactor")));

            QSettings persisted;
            QCOMPARE(persisted.value(
                         QStringLiteral("playback/upscaling/schema")).toInt(),
                3);
            const QJsonDocument persistedDocument = QJsonDocument::fromJson(
                persisted.value(
                    QStringLiteral("playback/upscaling/settingsJson"))
                    .toByteArray());
            QCOMPARE(persistedDocument.object().toVariantMap(), result);
        }
    }

    void upscalingPreferencesRejectCorruptOrUnknownSchema()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/upscaling/schema"), 99);
        settings.setValue(QStringLiteral("playback/upscaling/settingsJson"),
            QByteArrayLiteral("{not-json"));

        PreferencesViewModel preferences;
        const QVariantMap restored = preferences.upscalingSettings();
        QCOMPARE(restored.value(QStringLiteral("enabled")).toBool(), false);
        QCOMPARE(restored.value(QStringLiteral("providerId")).toString(),
            QStringLiteral("anime4k"));
    }

    void upscalingRebuildCoalescesSynchronousReentry()
    {
        PreferencesViewModel preferences;
        UpscalingViewModel viewModel(
            &preferences, nullptr, nullptr);

        int emissionDepth = 0;
        int maximumEmissionDepth = 0;
        int emissionCount = 0;
        connect(&viewModel, &UpscalingViewModel::stateChanged,
            &viewModel, [&] {
                ++emissionDepth;
                maximumEmissionDepth = std::max(
                    maximumEmissionDepth, emissionDepth);
                ++emissionCount;
                if (emissionCount == 1) {
                    QVariantMap nested = preferences.upscalingSettings();
                    nested.insert(
                        QStringLiteral("presetId"),
                        QStringLiteral("quality"));
                    preferences.saveUpscalingSettings(nested);
                }
                --emissionDepth;
            });

        QVariantMap initial = preferences.upscalingSettings();
        initial.insert(
            QStringLiteral("enabled"), true);
        preferences.saveUpscalingSettings(initial);

        QCOMPARE(maximumEmissionDepth, 1);
        QTRY_COMPARE(emissionCount, 2);
        QCOMPARE(maximumEmissionDepth, 1);
        QCOMPARE(preferences.upscalingSettings()
                     .value(QStringLiteral("presetId")).toString(),
            QStringLiteral("quality"));
    }

    void sessionAndCatalogUseTypedPorts()
    {
        Fixture fixture;
        fixture.session.name = QStringLiteral("Home");
        fixture.session.domain = QStringLiteral("emby.example.test");
        fixture.catalog.displayedId = QStringLiteral("collection-A");
        ApplicationViewModel viewModel(fixture.ports());

        QCOMPARE(viewModel.session()->displayName(), QStringLiteral("Home"));
        QCOMPARE(viewModel.session()->serverDomain(), QStringLiteral("emby.example.test"));
        QCOMPARE(viewModel.home()->collectionDisplayedId(), QStringLiteral("collection-A"));
        viewModel.session()->login(QStringLiteral("Home"),
            QStringLiteral("https://emby.example.test"), QStringLiteral("user"),
            QStringLiteral("secret"), false);
        QCOMPARE(fixture.session.loginArguments.value(QStringLiteral("userName")).toString(),
            QStringLiteral("user"));
        viewModel.home()->loadCollection(QStringLiteral("collection-B"));
        QCOMPARE(fixture.catalog.loadedCollection, QStringLiteral("collection-B"));
        viewModel.favorites()->refresh();
        QCOMPARE(fixture.catalog.refreshFavoritesCalls, 1);
    }

    void searchUsesItsOwnTypedPortAndBoundedModel()
    {
        Fixture fixture;
        fixture.search.cached = 110000;
        fixture.search.total = 110000;
        fixture.search.matches = 73;
        fixture.search.isComplete = true;
        QVariantList items;
        for (int index = 0; index < 50; ++index) {
            items.push_back(QVariantMap {
                {QStringLiteral("id"), QStringLiteral("episode-%1").arg(index)},
                {QStringLiteral("title"), QStringLiteral("Episode %1").arg(index)},
                {QStringLiteral("itemType"), QStringLiteral("Episode")},
            });
        }
        fixture.search.store.setQuery(QStringLiteral("search"), {}, items);
        fixture.search.store.setQuery(QStringLiteral("search-titles"), {}, {});
        fixture.search.store.setQuery(QStringLiteral("search-episodes"), {}, items);
        fixture.search.more = true;

        ApplicationViewModel viewModel(fixture.ports());
        QCOMPARE(viewModel.search()->results()->rowCount(), 50);
        QCOMPARE(viewModel.search()->titleResults()->rowCount(), 0);
        QCOMPARE(viewModel.search()->episodeResults()->rowCount(), 50);
        QCOMPARE(viewModel.search()->cachedCount(), 110000);
        QCOMPARE(viewModel.search()->totalMatches(), 73);
        QVERIFY(viewModel.search()->hasMore());
        QVERIFY(viewModel.search()->complete());

        viewModel.search()->inputPending();
        QCOMPARE(fixture.search.inputPendingCalls, 1);
        viewModel.search()->submit(QStringLiteral("S02E03"));
        QCOMPARE(fixture.search.currentQuery, QStringLiteral("S02E03"));
        QCOMPARE(fixture.search.searchCalls, 1);
        viewModel.search()->refresh();
        QCOMPARE(fixture.search.refreshCalls, 1);
    }

    void sessionOperationsRequireMatchingExplicitTerminal()
    {
        FakeSessionPort port;
        SessionViewModel session(&port);

        session.login(QStringLiteral("Home"),
            QStringLiteral("https://emby.example.test"),
            QStringLiteral("user"), QStringLiteral("secret"), false);
        const QString loginRequest = port.lastRequestId;
        QVERIFY(!loginRequest.isEmpty());
        QVERIFY(session.operation()->busy());
        session.logout();
        QCOMPARE(port.lastRequestId, loginRequest);
        QVERIFY(!port.logoutCalled);

        emit port.operationFailed(QStringLiteral("foreign"),
            SessionPort::Operation::Login, QStringLiteral("stale"));
        port.isConnected = true;
        ++port.sessionGeneration;
        emit port.stateChanged();
        QVERIFY(session.operation()->busy());
        QCOMPARE(session.generation(), port.sessionGeneration);

        emit port.operationCompleted(
            loginRequest, SessionPort::Operation::Login);
        QCOMPARE(session.operation()->phase(),
            AsyncOperationState::Phase::Succeeded);
        emit port.operationFailed(loginRequest,
            SessionPort::Operation::Login, QStringLiteral("late"));
        QCOMPARE(session.operation()->phase(),
            AsyncOperationState::Phase::Succeeded);

        session.logout();
        const QString logoutRequest = port.lastRequestId;
        QVERIFY(logoutRequest != loginRequest);
        emit port.operationFailed(logoutRequest,
            SessionPort::Operation::Logout, QStringLiteral("offline"));
        QCOMPARE(session.operation()->phase(),
            AsyncOperationState::Phase::Failed);
        QCOMPARE(session.operation()->errorMessage(), QStringLiteral("offline"));
    }

    void catalogNoOpAndRejectedDispositionsSettleResources()
    {
        FakeCatalogPort port;
        HomeViewModel home(&port);
        FavoritesViewModel favorites(&port);

        port.favoritesDisposition =
            CatalogPort::RequestDisposition::AlreadyCurrent;
        favorites.load();
        QCOMPARE(favorites.resourceState()->phase(),
            AsyncResourceState::Phase::Ready);

        port.displayedId = QStringLiteral("collection-A");
        port.parentItem = {
            {QStringLiteral("id"), QStringLiteral("collection-A")},
        };
        port.collectionDisposition =
            CatalogPort::RequestDisposition::AlreadyCurrent;
        home.loadCollection(QStringLiteral("collection-A"));
        QCOMPARE(home.collectionState()->phase(),
            AsyncResourceState::Phase::Ready);

        port.libraryDisposition = CatalogPort::RequestDisposition::Rejected;
        home.loadLibrary();
        QCOMPARE(home.libraryState()->phase(),
            AsyncResourceState::Phase::Error);
        port.activityDisposition = CatalogPort::RequestDisposition::Rejected;
        home.refreshActivity();
        QCOMPARE(home.activityState()->phase(),
            AsyncResourceState::Phase::Error);

        port.activityDisposition =
            CatalogPort::RequestDisposition::AlreadyCurrent;
        home.ensureActivityFresh();
        QCOMPARE(port.ensureActivityFreshCalls, 1);
        QCOMPARE(home.activityState()->phase(),
            AsyncResourceState::Phase::Ready);

        port.activityFailed = true;
        home.ensureActivityFresh();
        QCOMPARE(home.activityState()->phase(),
            AsyncResourceState::Phase::Ready);
        QVERIFY(home.activityState()->stale());
        QVERIFY(!home.activityState()->errorMessage().isEmpty());

        FakeCatalogPort coldPort;
        coldPort.activityFailed = true;
        coldPort.activityDisposition =
            CatalogPort::RequestDisposition::AlreadyCurrent;
        HomeViewModel coldHome(&coldPort);
        coldHome.ensureActivityFresh();
        QCOMPARE(coldHome.activityState()->phase(),
            AsyncResourceState::Phase::Error);
    }

    void activityFreshnessPolicyRequiresTwoCurrentQueries()
    {
        constexpr qint64 now = 100000;
        const CatalogQueryFreshness fresh {
            .available = true,
            .stale = false,
            .fetchedAtMs = now - 1000,
        };
        QVERIFY(CatalogFreshnessPolicy::activityIsFresh(
            fresh, fresh, now));

        CatalogQueryFreshness old = fresh;
        old.fetchedAtMs = now
            - CatalogFreshnessPolicy::activityRefreshAdmissionMs;
        QVERIFY(!CatalogFreshnessPolicy::activityIsFresh(
            old, fresh, now));

        CatalogQueryFreshness stale = fresh;
        stale.stale = true;
        QVERIFY(!CatalogFreshnessPolicy::activityIsFresh(
            fresh, stale, now));

        CatalogQueryFreshness future = fresh;
        future.fetchedAtMs = now + 1;
        QVERIFY(!CatalogFreshnessPolicy::activityIsFresh(
            future, fresh, now));

        CatalogQueryFreshness missing = fresh;
        missing.available = false;
        QVERIFY(!CatalogFreshnessPolicy::activityIsFresh(
            fresh, missing, now));
    }

    void activitySnapshotPolicyRejectsLateOlderLibraryResponse()
    {
        QVERIFY(CatalogFreshnessPolicy::activitySnapshotMayCommit(2, 1));
        QVERIFY(CatalogFreshnessPolicy::activitySnapshotMayCommit(2, 2));
        QVERIFY(!CatalogFreshnessPolicy::activitySnapshotMayCommit(1, 2));
        QVERIFY(CatalogFreshnessPolicy::activityResultMayAffectState(
            true, 2, 1));
        QVERIFY(!CatalogFreshnessPolicy::activityResultMayAffectState(
            false, 2, 1));
        QVERIFY(!CatalogFreshnessPolicy::activityResultMayAffectState(
            true, 1, 2));
    }

    void playbackUsesTypedPortAndTypedResultSignal()
    {
        FakePlaybackPort port;
        FakePlaybackReporter reporter;
        PlaybackViewModel playback(&port, &reporter);
        QSignalSpy readySpy(&playback, &PlaybackViewModel::ready);
        QSignalSpy failedSpy(&playback, &PlaybackViewModel::failed);

        const QVariantMap context {{QStringLiteral("seriesId"), QStringLiteral("series")}};
        playback.prepareInContext(QStringLiteral("series-P"), context);
        QCOMPARE(port.preparedItemId, QStringLiteral("series-P"));
        QCOMPARE(port.preparedContext, context);
        QVERIFY(!port.preparedRequestId.isEmpty());

        // A container request can legitimately resolve to a playable episode.
        const QVariantMap descriptor {
            {QStringLiteral("itemId"), QStringLiteral("episode-P1")},
            {QStringLiteral("reportSessionId"), QStringLiteral("session-P1")},
        };
        emit port.ready(port.preparedRequestId, descriptor);
        emit port.failed(port.preparedRequestId, QStringLiteral("series-P"),
            QStringLiteral("late failure"));
        QCOMPARE(readySpy.count(), 1);
        QCOMPARE(readySpy.at(0).at(0).toMap(), descriptor);
        QCOMPARE(failedSpy.count(), 0);

        playback.switchToInContext(QStringLiteral("item-N"), context, 17.5, true);
        QCOMPARE(port.switchedPosition, 17.5);
        QVERIFY(!port.switchedRequestId.isEmpty());
        QVERIFY(port.switchedRequestId != port.preparedRequestId);
        QCOMPARE(playback.preparationState()->phase(),
            AsyncResourceState::Phase::Loading);
        emit port.failed(port.switchedRequestId, QStringLiteral("item-N"),
            QStringLiteral("offline"));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(playback.preparationState()->phase(),
            AsyncResourceState::Phase::Error);
        QObject player;
        const QVariantList tracks {
            QVariantMap {{QStringLiteral("streamIndex"), 2}},
        };
        QVERIFY(playback.attachPlayer(&player));
        QVERIFY(playback.beginSession(QStringLiteral("session-1"), tracks));
        playback.stopSession();
        QCOMPARE(reporter.attachedPlayer, &player);
        QCOMPARE(reporter.startedSessionId, QStringLiteral("session-1"));
        QCOMPARE(reporter.startedTracks, tracks);
        // Switching stops the old session before registering the next item;
        // the explicit stop below remains idempotent at the real reporter.
        QCOMPARE(reporter.stopCalls, 2);

    }

    void playbackRejectsCancelledAndReopenedLateResponses()
    {
        FakePlaybackPort port;
        PlaybackViewModel playback(&port);
        QSignalSpy readySpy(&playback, &PlaybackViewModel::ready);
        QSignalSpy failedSpy(&playback, &PlaybackViewModel::failed);

        playback.prepare(QStringLiteral("item-A"));
        const QString firstRequest = port.preparedRequestId;
        playback.cancelPreparation();
        emit port.ready(firstRequest,
            {{QStringLiteral("itemId"), QStringLiteral("item-A")}});
        emit port.failed(firstRequest, QStringLiteral("item-A"),
            QStringLiteral("late"));
        QCOMPARE(readySpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);

        playback.prepare(QStringLiteral("item-A"));
        const QString secondRequest = port.preparedRequestId;
        QVERIFY(secondRequest != firstRequest);
        emit port.ready(firstRequest,
            {{QStringLiteral("itemId"), QStringLiteral("item-A")}});
        QCOMPARE(readySpy.count(), 0);
        emit port.ready(secondRequest,
            {{QStringLiteral("itemId"), QStringLiteral("item-A")}});
        QCOMPARE(readySpy.count(), 1);
        emit port.failed(secondRequest, QStringLiteral("item-A"),
            QStringLiteral("late after ready"));
        QCOMPARE(failedSpy.count(), 0);
    }

    void danmakuUsesTypedOperations()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());
        auto *danmaku = viewModel.danmaku();
        QSignalSpy searchSpy(danmaku, &DanmakuViewModel::searchCompleted);
        QSignalSpy applySpy(danmaku, &DanmakuViewModel::matchApplied);

        danmaku->search(QStringLiteral("item-D"), QStringLiteral("query"));
        QCOMPARE(fixture.danmaku.lastOperation, DanmakuPort::Operation::Search);
        QCOMPARE(fixture.danmaku.searchAnime, QStringLiteral("query"));
        QVERIFY(danmaku->working());
        emit fixture.danmaku.operationCompleted(fixture.danmaku.lastRequestId,
            QStringLiteral("item-D"),
            DanmakuPort::Operation::Search, {{QStringLiteral("status"), QStringLiteral("choice-required")}});
        QCOMPARE(searchSpy.count(), 1);
        QVERIFY(!danmaku->working());

        const QVariantMap match {{QStringLiteral("episodeId"), 197}};
        const QVariantMap style {{QStringLiteral("fontSize"), 42}};
        danmaku->applyMatch(QStringLiteral("item-D"), match, style);
        QCOMPARE(fixture.danmaku.appliedMatch, match);
        QCOMPARE(fixture.danmaku.appliedStyle, style);
        emit fixture.danmaku.operationCompleted(fixture.danmaku.lastRequestId,
            QStringLiteral("item-D"),
            DanmakuPort::Operation::ApplyMatch, {{QStringLiteral("status"), QStringLiteral("loaded")}});
        QCOMPARE(applySpy.count(), 1);
    }

    void danmakuRejectsStaleAndDuplicateTerminals()
    {
        FakeDanmakuPort port;
        DanmakuViewModel danmaku(&port);
        QSignalSpy completed(&danmaku, &DanmakuViewModel::searchCompleted);
        QSignalSpy failed(&danmaku, &DanmakuViewModel::searchFailed);

        danmaku.search(QStringLiteral("item-A"), QStringLiteral("A"));
        const QString firstRequest = port.lastRequestId;
        danmaku.search(QStringLiteral("item-B"), QStringLiteral("B"));
        const QString secondRequest = port.lastRequestId;
        QVERIFY(secondRequest != firstRequest);
        emit port.operationCompleted(firstRequest, QStringLiteral("item-A"),
            DanmakuPort::Operation::Search, {});
        QCOMPARE(completed.count(), 0);
        QVERIFY(danmaku.working());
        emit port.operationCompleted(secondRequest, QStringLiteral("item-B"),
            DanmakuPort::Operation::Search, {});
        QCOMPARE(completed.count(), 1);
        QVERIFY(!danmaku.working());
        emit port.operationFailed(secondRequest, QStringLiteral("item-B"),
            DanmakuPort::Operation::Search, QStringLiteral("late"), false);
        QCOMPARE(failed.count(), 0);
    }

    void automaticDanmakuFailureOnlyReachesTheCurrentPlaybackRequest()
    {
        FakeDanmakuPort port;
        DanmakuViewModel danmaku(&port);
        QSignalSpy failed(&danmaku,
            &DanmakuViewModel::automaticLoadFailed);

        danmaku.loadAutomatically(QStringLiteral("item-A"));
        const QString firstRequest = port.lastRequestId;
        danmaku.loadAutomatically(QStringLiteral("item-B"));
        const QString secondRequest = port.lastRequestId;
        QVERIFY(secondRequest != firstRequest);

        emit port.operationFailed(firstRequest, QStringLiteral("item-A"),
            DanmakuPort::Operation::AutomaticLoad,
            QStringLiteral("stale failure"), true);
        QCOMPARE(failed.count(), 0);
        QVERIFY(danmaku.working());

        const QString currentMessage =
            QStringLiteral("Could not connect to the danmaku service.");
        emit port.operationFailed(secondRequest, QStringLiteral("item-B"),
            DanmakuPort::Operation::AutomaticLoad, currentMessage, true);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("item-B"));
        QCOMPARE(failed.at(0).at(1).toString(), currentMessage);
        QVERIFY(failed.at(0).at(2).toBool());
        QVERIFY(!danmaku.working());
    }

    void danmakuConfigurationRequiresMatchingExplicitTerminal()
    {
        FakeDanmakuPort port;
        DanmakuViewModel danmaku(&port);

        danmaku.configure(QStringLiteral("app"), QStringLiteral("secret"));
        const QString configureRequest = port.configurationRequestId;
        QVERIFY(!configureRequest.isEmpty());
        emit port.stateChanged();
        QVERIFY(danmaku.configurationOperation()->busy());
        emit port.configurationFailed(QStringLiteral("foreign"),
            DanmakuPort::ConfigurationOperation::Configure,
            QStringLiteral("stale"));
        QVERIFY(danmaku.configurationOperation()->busy());
        danmaku.clearConfiguration();
        QCOMPARE(port.configurationRequestId, configureRequest);
        QVERIFY(!port.clearCalled);

        port.isConfigured = true;
        emit port.configurationCompleted(configureRequest,
            DanmakuPort::ConfigurationOperation::Configure);
        QCOMPARE(danmaku.configurationOperation()->phase(),
            AsyncOperationState::Phase::Succeeded);
        emit port.configurationFailed(configureRequest,
            DanmakuPort::ConfigurationOperation::Configure,
            QStringLiteral("late"));
        QCOMPARE(danmaku.configurationOperation()->phase(),
            AsyncOperationState::Phase::Succeeded);

        danmaku.clearConfiguration();
        const QString clearRequest = port.configurationRequestId;
        emit port.configurationFailed(clearRequest,
            DanmakuPort::ConfigurationOperation::Clear,
            QStringLiteral("invalid"));
        QCOMPARE(danmaku.configurationOperation()->phase(),
            AsyncOperationState::Phase::Failed);
    }

    void mediaMutationsWithSameKeyDispatchSerially()
    {
        FakeMediaPort port;
        MediaActionsViewModel actions(&port);
        QSignalSpy playedSpy(&actions, &MediaActionsViewModel::playedChanged);

        actions.setPlayed(QStringLiteral("item-Q"), true);
        actions.setPlayed(QStringLiteral("item-Q"), false);
        QCOMPARE(port.calls.size(), 1);
        const QString firstRequest = port.calls.constFirst().requestId;

        emit port.operationCompleted(firstRequest, QStringLiteral("item-Q"),
            MediaPort::Operation::SetPlayed,
            {{QStringLiteral("requestedPlayed"), true}});
        QCOMPARE(port.calls.size(), 2);
        const QString secondRequest = port.calls.constLast().requestId;
        QVERIFY(secondRequest != firstRequest);

        // A duplicate terminal from the first request cannot settle or
        // broadcast for the second queued mutation.
        emit port.operationFailed(firstRequest, QStringLiteral("item-Q"),
            MediaPort::Operation::SetPlayed, QStringLiteral("late"), false);
        QCOMPARE(playedSpy.count(), 1);
        emit port.operationCompleted(secondRequest, QStringLiteral("item-Q"),
            MediaPort::Operation::SetPlayed,
            {{QStringLiteral("requestedPlayed"), false}});
        QCOMPARE(playedSpy.count(), 2);
    }

    void statusAndScanProgressAreExposedWithoutBackendAliases()
    {
        Fixture fixture;
        fixture.status.currentMessage = QStringLiteral("offline");
        fixture.status.isError = true;
        fixture.media.scanProgress = {{QStringLiteral("item-1"), 42}};
        ApplicationViewModel viewModel(fixture.ports());
        QSignalSpy statusSpy(viewModel.status(), &ApplicationStatusViewModel::stateChanged);
        QSignalSpy progressSpy(viewModel.mediaActions(),
            &MediaActionsViewModel::libraryScanProgressChanged);

        QCOMPARE(viewModel.status()->message(), QStringLiteral("offline"));
        QVERIFY(viewModel.status()->error());
        QCOMPARE(viewModel.mediaActions()->libraryScanProgress()
            .value(QStringLiteral("item-1")).toInt(), 42);
        emit fixture.status.stateChanged();
        emit fixture.media.scanProgressChanged();
        QCOMPARE(statusSpy.count(), 1);
        QCOMPARE(progressSpy.count(), 1);
        viewModel.status()->clear();
        QVERIFY(viewModel.status()->message().isEmpty());
    }

    void metadataSaveCompletesBeforeBackgroundReconciliation()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());
        MetadataEditorViewModel *editor = viewModel.metadataEditor();
        const quint64 generation = editor->open({
            {QStringLiteral("id"), QStringLiteral("item-meta")},
            {QStringLiteral("itemType"), QStringLiteral("Series")},
        });
        QCOMPARE(fixture.media.calls.size(), 1);
        const MediaCall load = fixture.media.calls.takeFirst();
        emit fixture.media.operationCompleted(load.requestId,
            QStringLiteral("item-meta"), MediaPort::Operation::LoadMetadata, {
                {QStringLiteral("id"), QStringLiteral("item-meta")},
                {QStringLiteral("clientViewGeneration"), generation},
            });
        QCOMPARE(editor->resourceState()->phase(), AsyncResourceState::Phase::Ready);

        QSignalSpy saved(editor, &MetadataEditorViewModel::saveCompleted);
        QVERIFY(editor->save({{QStringLiteral("title"), QStringLiteral("Renamed")}}));
        const MediaCall update = fixture.media.calls.takeLast();
        emit fixture.media.operationCompleted(update.requestId,
            QStringLiteral("item-meta"), MediaPort::Operation::UpdateMetadata, {
                {QStringLiteral("providerIdsChanged"), true},
            });
        QCOMPARE(saved.count(), 1);
        QCOMPARE(editor->saveOperation()->phase(), AsyncOperationState::Phase::Succeeded);
        QVERIFY(editor->reconciliationOperation()->busy());
        const MediaCall refresh = fixture.media.calls.takeLast();
        QCOMPARE(refresh.operation, MediaPort::Operation::RefreshMetadata);
        emit fixture.media.operationCompleted(refresh.requestId,
            QStringLiteral("item-meta"), MediaPort::Operation::RefreshMetadata, {});
        QCOMPARE(editor->reconciliationOperation()->phase(),
            AsyncOperationState::Phase::Succeeded);
    }

    void metadataBackgroundReconciliationOutlivesDialog()
    {
        FakeMediaPort port;
        MetadataEditorViewModel editor(&port);
        const quint64 generation = editor.open({
            {QStringLiteral("id"), QStringLiteral("item-bg")},
        });
        const MediaCall load = port.calls.takeLast();
        emit port.operationCompleted(load.requestId, QStringLiteral("item-bg"),
            MediaPort::Operation::LoadMetadata, {
                {QStringLiteral("id"), QStringLiteral("item-bg")},
                {QStringLiteral("clientViewGeneration"), generation},
            });
        QVERIFY(editor.save({{QStringLiteral("title"), QStringLiteral("new")}}));
        const MediaCall save = port.calls.takeLast();
        emit port.operationCompleted(save.requestId, QStringLiteral("item-bg"),
            MediaPort::Operation::UpdateMetadata,
            {{QStringLiteral("providerIdsChanged"), true}});
        const MediaCall reconcile = port.calls.takeLast();
        QSignalSpy failedSpy(&editor,
            &MetadataEditorViewModel::reconciliationFailed);
        editor.dismiss();
        QVERIFY(editor.reconciliationOperation()->busy());
        emit port.operationFailed(reconcile.requestId,
            QStringLiteral("item-bg"), MediaPort::Operation::RefreshMetadata,
            QStringLiteral("offline"), true);
        QCOMPARE(editor.reconciliationOperation()->phase(),
            AsyncOperationState::Phase::Failed);
        QCOMPARE(failedSpy.count(), 1);
        emit port.operationCompleted(reconcile.requestId,
            QStringLiteral("item-bg"), MediaPort::Operation::RefreshMetadata, {});
        QCOMPARE(failedSpy.count(), 1);
    }

    void imageMutationFailureRestartsFencedReconciliation()
    {
        FakeMediaPort port;
        ImageEditorViewModel editor(&port, nullptr);
        editor.setSessionGeneration(4);
        editor.open({{QStringLiteral("id"), QStringLiteral("item-image")}});
        const auto initialIterator = std::find_if(port.calls.cbegin(), port.calls.cend(),
            [](const MediaCall &call) {
                return call.operation == MediaPort::Operation::LoadImages;
            });
        QVERIFY(initialIterator != port.calls.cend());
        const MediaCall initial = *initialIterator;
        emit port.operationCompleted(initial.requestId,
            QStringLiteral("item-image"), MediaPort::Operation::LoadImages, {
                {QStringLiteral("id"), QStringLiteral("item-image")},
                {QStringLiteral("clientViewGeneration"), editor.viewGeneration()},
                {QStringLiteral("images"), QVariantList{}},
            });

        const QVariantMap primary {
            {QStringLiteral("imageType"), QStringLiteral("Primary")},
            {QStringLiteral("mode"), QStringLiteral("add")},
        };
        editor.applyRemote(primary, {
            {QStringLiteral("imageUrl"), QStringLiteral("https://example/primary.jpg")},
            {QStringLiteral("providerName"), QStringLiteral("provider")},
        });
        const MediaCall firstMutation = port.calls.constLast();
        emit port.operationCompleted(firstMutation.requestId,
            QStringLiteral("item-image"), MediaPort::Operation::ApplyRemoteImage, {});
        const MediaCall firstReconcile = port.calls.constLast();
        QCOMPARE(firstReconcile.operation, MediaPort::Operation::LoadImages);

        const QVariantMap logo {
            {QStringLiteral("imageType"), QStringLiteral("Logo")},
            {QStringLiteral("mode"), QStringLiteral("add")},
        };
        editor.applyRemote(logo, {
            {QStringLiteral("imageUrl"), QStringLiteral("https://example/logo.png")},
            {QStringLiteral("providerName"), QStringLiteral("provider")},
        });
        const MediaCall secondMutation = port.calls.constLast();
        emit port.operationFailed(secondMutation.requestId,
            QStringLiteral("item-image"), MediaPort::Operation::ApplyRemoteImage,
            QStringLiteral("rejected"), false);
        const MediaCall retriedReconcile = port.calls.constLast();
        QCOMPARE(retriedReconcile.operation, MediaPort::Operation::LoadImages);
        QVERIFY(retriedReconcile.requestId != firstReconcile.requestId);
    }

    void imageEditorPreservesProvidersAndRejectsSameViewRetryStaleResult()
    {
        FakeMediaPort port;
        ImageEditorViewModel editor(&port, nullptr);
        editor.setSessionGeneration(7);
        editor.open({{QStringLiteral("id"), QStringLiteral("item-image")}});
        const MediaCall firstImages = *std::find_if(
            port.calls.cbegin(), port.calls.cend(), [](const MediaCall &call) {
                return call.operation == MediaPort::Operation::LoadImages;
            });
        const MediaCall firstProviders = *std::find_if(
            port.calls.cbegin(), port.calls.cend(), [](const MediaCall &call) {
                return call.operation == MediaPort::Operation::LoadImageProviders;
            });

        const QVariantList providers{
            QVariantMap{{QStringLiteral("name"), QStringLiteral("Provider A")}},
        };
        emit port.operationCompleted(firstProviders.requestId,
            QStringLiteral("item-image"),
            MediaPort::Operation::LoadImageProviders,
            {{QStringLiteral("clientViewGeneration"), editor.viewGeneration()},
             {QStringLiteral("providers"), providers}});
        emit port.operationCompleted(firstImages.requestId,
            QStringLiteral("item-image"), MediaPort::Operation::LoadImages, {
                {QStringLiteral("id"), QStringLiteral("item-image")},
                {QStringLiteral("clientViewGeneration"), editor.viewGeneration()},
                {QStringLiteral("images"), QVariantList{}},
            });
        QCOMPARE(editor.editor().value(QStringLiteral("providers")).toList(),
            providers);

        editor.retry();
        const QList<MediaCall> providerCalls = [&port] {
            QList<MediaCall> result;
            for (const MediaCall &call : port.calls) {
                if (call.operation == MediaPort::Operation::LoadImageProviders)
                    result.append(call);
            }
            return result;
        }();
        QCOMPARE(providerCalls.size(), 2);
        QVERIFY(providerCalls.at(0).requestId != providerCalls.at(1).requestId);
        emit port.operationFailed(providerCalls.at(0).requestId,
            QStringLiteral("item-image"),
            MediaPort::Operation::LoadImageProviders,
            QStringLiteral("stale"), false);
        QCOMPARE(editor.editor().value(QStringLiteral("providers")).toList(),
            providers);
        const QVariantList newerProviders{
            QVariantMap{{QStringLiteral("name"), QStringLiteral("Provider B")}},
        };
        emit port.operationCompleted(providerCalls.at(1).requestId,
            QStringLiteral("item-image"),
            MediaPort::Operation::LoadImageProviders,
            {{QStringLiteral("clientViewGeneration"), editor.viewGeneration()},
             {QStringLiteral("providers"), newerProviders}});
        QCOMPARE(editor.editor().value(QStringLiteral("providers")).toList(),
            newerProviders);
    }

    void featureViewModelsDeferSynchronousPortTerminals()
    {
        FakeMediaPort imagePort;
        imagePort.onCall = [&imagePort](const MediaCall &call) {
            if (call.operation == MediaPort::Operation::LoadImages) {
                emit imagePort.operationCompleted(call.requestId, call.itemId,
                    call.operation, {
                        {QStringLiteral("id"), call.itemId},
                        {QStringLiteral("clientViewGeneration"),
                            call.arguments.value(QStringLiteral("viewGeneration"))},
                        {QStringLiteral("images"), QVariantList{}},
                    });
            } else if (call.operation == MediaPort::Operation::LoadImageProviders) {
                emit imagePort.operationCompleted(call.requestId, call.itemId,
                    call.operation, {
                        {QStringLiteral("clientViewGeneration"),
                            call.arguments.value(QStringLiteral("viewGeneration"))},
                        {QStringLiteral("providers"), QVariantList{}},
                    });
            }
        };
        ImageEditorViewModel images(&imagePort, nullptr);
        QSignalSpy imageReady(&images, &ImageEditorViewModel::editorReady);
        const QVariantMap imageContext = images.open(
            {{QStringLiteral("id"), QStringLiteral("sync-image")}});
        QVERIFY(!imageContext.isEmpty());
        QCOMPARE(imageReady.count(), 0);
        QTRY_COMPARE(imageReady.count(), 1);

        FakeMediaPort metadataPort;
        metadataPort.onCall = [&metadataPort](const MediaCall &call) {
            if (call.operation != MediaPort::Operation::LoadMetadata)
                return;
            emit metadataPort.operationCompleted(call.requestId, call.itemId,
                call.operation, {
                    {QStringLiteral("id"), call.itemId},
                    {QStringLiteral("clientViewGeneration"),
                        call.arguments.value(QStringLiteral("viewGeneration"))},
                });
        };
        MetadataEditorViewModel metadata(&metadataPort);
        QSignalSpy metadataReady(
            &metadata, &MetadataEditorViewModel::metadataReady);
        const quint64 metadataGeneration = metadata.open(
            {{QStringLiteral("id"), QStringLiteral("sync-metadata")}});
        QVERIFY(metadataGeneration > 0);
        QCOMPARE(metadataReady.count(), 0);
        QTRY_COMPARE(metadataReady.count(), 1);

        FakeMediaPort targetPort;
        targetPort.onCall = [&targetPort](const MediaCall &call) {
            if (call.operation == MediaPort::Operation::LoadPlaylistTargets) {
                emit targetPort.operationCompleted(call.requestId, call.itemId,
                    call.operation, {{QStringLiteral("options"), QVariantList{}}});
            }
        };
        MediaTargetFlowViewModel targets(&targetPort);
        QSignalSpy targetsReady(
            &targets, &MediaTargetFlowViewModel::targetsReady);
        QVERIFY(targets.load(
            {{QStringLiteral("id"), QStringLiteral("sync-target")}}));
        QCOMPARE(targetsReady.count(), 0);
        QTRY_COMPARE(targetsReady.count(), 1);
    }

    void editorRequestIdentityRejectsCloseAndReopenLateResponses()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());
        MetadataEditorViewModel *metadata = viewModel.metadataEditor();
        metadata->open({{QStringLiteral("id"), QStringLiteral("same")}});
        const MediaCall first = fixture.media.calls.takeLast();
        metadata->dismiss();
        QVERIFY(!metadata->saveOperation()->busy());
        const quint64 secondGeneration = metadata->open(
            {{QStringLiteral("id"), QStringLiteral("same")}});
        const MediaCall second = fixture.media.calls.takeLast();
        emit fixture.media.operationCompleted(first.requestId,
            QStringLiteral("same"), MediaPort::Operation::LoadMetadata, {
                {QStringLiteral("id"), QStringLiteral("same")},
                {QStringLiteral("clientViewGeneration"), secondGeneration},
                {QStringLiteral("title"), QStringLiteral("stale")},
            });
        QVERIFY(metadata->metadata().isEmpty());
        emit fixture.media.operationCompleted(second.requestId,
            QStringLiteral("same"), MediaPort::Operation::LoadMetadata, {
                {QStringLiteral("id"), QStringLiteral("same")},
                {QStringLiteral("clientViewGeneration"), secondGeneration},
                {QStringLiteral("title"), QStringLiteral("current")},
            });
        QCOMPARE(metadata->metadata().value(QStringLiteral("title")).toString(),
            QStringLiteral("current"));

        MediaTargetFlowViewModel *targets = viewModel.mediaTarget();
        QVERIFY(targets->load({{QStringLiteral("id"), QStringLiteral("same")}}));
        const MediaCall targetFirst = fixture.media.calls.takeLast();
        targets->cancel();
        QVERIFY(!targets->submitOperation()->busy());
        QVERIFY(targets->load({{QStringLiteral("id"), QStringLiteral("same")}}));
        const MediaCall targetSecond = fixture.media.calls.takeLast();
        emit fixture.media.operationCompleted(targetFirst.requestId,
            QStringLiteral("same"), MediaPort::Operation::LoadPlaylistTargets, {
                {QStringLiteral("options"), QVariantList{
                    QVariantMap{{QStringLiteral("id"), QStringLiteral("stale")}}}},
            });
        QVERIFY(targets->options().isEmpty());
        emit fixture.media.operationCompleted(targetSecond.requestId,
            QStringLiteral("same"), MediaPort::Operation::LoadPlaylistTargets, {
                {QStringLiteral("options"), QVariantList{
                    QVariantMap{{QStringLiteral("id"), QStringLiteral("current")}}}},
            });
        QCOMPARE(targets->options().first().toMap()
            .value(QStringLiteral("id")).toString(), QStringLiteral("current"));
    }

    void mediaActionsIgnoreForeignFeatureRequestIdentity()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());
        MediaActionsViewModel *actions = viewModel.mediaActions();
        QSignalSpy completed(actions, &MediaActionsViewModel::metadataRefreshed);
        actions->refreshMetadata(
            QStringLiteral("same"), QStringLiteral("all"), true);
        const MediaCall own = fixture.media.calls.takeLast();
        emit fixture.media.operationCompleted(
            QStringLiteral("metadata-reconcile.same.foreign"),
            QStringLiteral("same"), MediaPort::Operation::RefreshMetadata, {});
        QCOMPARE(completed.count(), 0);
        emit fixture.media.operationCompleted(
            own.requestId, QStringLiteral("same"),
            MediaPort::Operation::RefreshMetadata, {});
        QCOMPARE(completed.count(), 1);
    }

    void sessionChangeDetachesOpenEditorResources()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());
        ImageEditorViewModel *images = viewModel.imageEditor();
        images->open({{QStringLiteral("id"), QStringLiteral("same")}});
        QVERIFY(images->opened());
        const quint64 generation = images->sessionGeneration();

        fixture.session.isConnected = true;
        ++fixture.session.sessionGeneration;
        emit fixture.session.stateChanged();
        QVERIFY(!images->opened());
        QVERIFY(images->sessionGeneration() > generation);
        QCOMPARE(images->initial()->phase(), AsyncResourceState::Phase::Idle);
    }

    void delayedReconciliationDoesNotCrossSessionGeneration()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());

        emit fixture.playback.stoppedReported();
        QCOMPARE(fixture.catalog.invalidateActivityCalls, 1);
        viewModel.mediaActions()->setPlayed(QStringLiteral("item-old"), true);
        const MediaCall mutation = fixture.media.calls.constLast();
        emit fixture.media.operationCompleted(mutation.requestId,
            QStringLiteral("item-old"), MediaPort::Operation::SetPlayed, {
                {QStringLiteral("requestedPlayed"), true},
                {QStringLiteral("reconcileComplete"), false},
            });

        ++fixture.session.sessionGeneration;
        emit fixture.session.stateChanged();
        QTest::qWait(3300);
        QCOMPARE(fixture.catalog.refreshActivityCalls, 0);
        QCOMPARE(fixture.catalog.loadLibraryCalls, 0);
        QVERIFY(fixture.catalog.refreshedCollection.isEmpty());
    }

    void playbackActivityReconciliationIsDebouncedAndTwoPhase()
    {
        Fixture fixture;
        ApplicationViewModel viewModel(fixture.ports());

        emit fixture.playback.stoppedReported();
        QTest::qWait(100);
        emit fixture.playback.stoppedReported();
        QCOMPARE(fixture.catalog.invalidateActivityCalls, 2);

        QTest::qWait(850);
        QCOMPARE(fixture.catalog.ensureActivityFreshCalls, 1);
        QCOMPARE(fixture.catalog.refreshActivityCalls, 0);

        QTest::qWait(2400);
        QCOMPARE(fixture.catalog.ensureActivityFreshCalls, 1);
        QCOMPARE(fixture.catalog.refreshActivityCalls, 1);
    }
};

QTEST_MAIN(ApplicationViewModelTests)
#include "ApplicationViewModelTests.moc"
