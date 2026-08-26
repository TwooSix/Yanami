#include "DevelopmentHooks.hpp"
#include "MpvVideoItem.hpp"
#include "OfflineNetworkAccessManager.hpp"
#include "PerformanceTrace.hpp"
#include "UpscalingAssetManager.hpp"
#include "UpscalingCapabilityProbe.hpp"
#include "UpscalingCatalog.hpp"

#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

void printResult(const QJsonObject &result)
{
    fprintf(stdout, "%s\n",
        QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}

bool createY4mFixture(const QString &path, int frameCount = 48)
{
    constexpr int width = 320;
    constexpr int height = 180;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write("YUV4MPEG2 W320 H180 F24:1 Ip A1:1 C420jpeg\n") < 0) {
        return false;
    }

    QByteArray luma(width * height, Qt::Uninitialized);
    QByteArray chromaU(width * height / 4, Qt::Uninitialized);
    QByteArray chromaV(width * height / 4, Qt::Uninitialized);
    chromaU.fill(static_cast<char>(154));
    chromaV.fill(static_cast<char>(188));
    for (int frame = 0; frame < frameCount; ++frame) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool checker = (((x + frame * 2) / 12) + (y / 12)) % 2;
                luma[y * width + x] = static_cast<char>(checker ? 196 : 52);
            }
        }
        if (file.write("FRAME\n") != 6
            || file.write(luma) != luma.size()
            || file.write(chromaU) != chromaU.size()
            || file.write(chromaV) != chromaV.size()) {
            return false;
        }
    }
    return file.flush();
}

QVector<YanamiUpscaling::ShaderArtifact> offlineArtifacts(
    const YanamiUpscaling::ResolvedProfile &profile,
    const QByteArray &payload)
{
    QVector<YanamiUpscaling::ShaderArtifact> artifacts;
    artifacts.reserve(profile.orderedShaderArtifactIds.size());
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
        payload, QCryptographicHash::Sha256).toHex());
    for (qsizetype index = 0;
         index < profile.orderedShaderArtifactIds.size(); ++index) {
        const QString fileName = QStringLiteral("offline-%1.glsl").arg(index);
        artifacts.append({
            .id = profile.orderedShaderArtifactIds.at(index),
            .providerId = profile.providerId,
            .version = profile.version,
            .assetSetId = QStringLiteral("anime4k-offline-mpv-smoke"),
            .fileName = fileName,
            .installRelativePath = QStringLiteral(
                "anime4k/offline-mpv-smoke/%1").arg(fileName),
            .downloadUrl = QStringLiteral(
                "https://raw.githubusercontent.com/bloc97/Anime4K/%1")
                    .arg(fileName),
            .sizeBytes = payload.size(),
            .sha256 = digest,
            .licenseSpdx = QStringLiteral("MIT"),
            .licenseUrl = QStringLiteral(
                "https://github.com/bloc97/Anime4K/blob/master/LICENSE"),
        });
    }
    return artifacts;
}

bool installOfflineArtifacts(
    UpscalingAssetManager &manager,
    const QVector<YanamiUpscaling::ShaderArtifact> &artifacts,
    const QByteArray &payload)
{
    for (const auto &artifact : artifacts) {
        const QString path = manager.absolutePath(artifact);
        if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(payload) != payload.size()) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    YanamiPerformance::PerformanceTrace::initialize(argc, argv);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("YanamiTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("yanami-upscaling-mpv-smoke"));

    const QString provider = argc > 1
        ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("anime4k");
    const QString preset = argc > 2
        ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("performance");
    const QString smokeMode = argc > 3
        ? QString::fromLocal8Bit(argv[3]) : QString{};
    const QString assetMode = argc > 4
        ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("live");
    const bool offline = smokeMode == QStringLiteral("offline")
        || assetMode == QStringLiteral("offline");
    const QString scenario = smokeMode == QStringLiteral("offline")
        ? QString{} : smokeMode;
    const bool performanceProtection = scenario == QStringLiteral("protection");
    const bool qualification = scenario == QStringLiteral("qualification");
    const bool runtimeToggle = scenario == QStringLiteral("toggle");
    const bool toggleBenchmark =
        scenario == QStringLiteral("toggle-benchmark");
    const bool rapidToggle = scenario == QStringLiteral("rapid-toggle");
    const bool rapidDisable = scenario == QStringLiteral("rapid-disable");
    const bool pendingStop = scenario == QStringLiteral("pending-stop");
    const bool stopConfigure =
        scenario == QStringLiteral("stop-configure");
    const bool pendingOpen = scenario == QStringLiteral("pending-open");
    const bool failureRollback =
        scenario == QStringLiteral("failure-rollback");
    const bool originalPlayback = scenario == QStringLiteral("original");
    const bool monitorPerformance = performanceProtection || qualification;
    const auto profile = YanamiUpscaling::UpscalingCatalog::resolve(
        provider, preset);
    if (profile.providerId != provider || profile.requiredArtifacts.isEmpty()) {
        printResult({
            {QStringLiteral("result"), QStringLiteral("catalog-invalid")},
        });
        return 1;
    }
    QTemporaryDir offlineAssetDirectory;
    QTemporaryDir offlineMediaDirectory;
    if (offline
        && (!offlineAssetDirectory.isValid()
            || !offlineMediaDirectory.isValid())) {
        printResult({
            {QStringLiteral("result"),
             QStringLiteral("offline-temporary-directory-failed")},
        });
        return 15;
    }
    const QString assetRoot = offline
        ? offlineAssetDirectory.path()
        : QDir(QStandardPaths::writableLocation(
              QStandardPaths::AppDataLocation))
              .filePath(QStringLiteral("models/upscaling"));
    const QByteArray offlineShader = QByteArrayLiteral(
        "//!HOOK MAIN\n"
        "//!BIND HOOKED\n"
        "vec4 hook(){return HOOKED_tex(HOOKED_pos);}\n");
    const QVector<YanamiUpscaling::ShaderArtifact> artifacts = offline
        ? offlineArtifacts(profile, offlineShader)
        : profile.requiredArtifacts;
    auto *offlineNetwork = offline
        ? new YanamiTest::OfflineNetworkAccessManager(
              {}, false, &application)
        : nullptr;
    auto *assets = offline
        ? new UpscalingAssetManager(offlineNetwork, assetRoot, &application)
        : new UpscalingAssetManager(assetRoot, &application);
    if (offline
        && (artifacts.isEmpty()
            || !installOfflineArtifacts(*assets, artifacts, offlineShader))) {
        printResult({
            {QStringLiteral("result"),
             QStringLiteral("offline-fixture-install-failed")},
        });
        return 15;
    }
    auto *capabilities = new UpscalingCapabilityProbe(&application);
    const QString taskId = QStringLiteral("mpv-smoke-%1-%2")
        .arg(provider, preset);
    auto *window = new QQuickWindow;
    window->setTitle(QStringLiteral("Yanami upscaling smoke"));
    const int requestedOutputWidth = qEnvironmentVariableIntValue(
        "YANAMI_UPSCALING_QUALIFICATION_OUTPUT_WIDTH");
    const int requestedOutputHeight = qEnvironmentVariableIntValue(
        "YANAMI_UPSCALING_QUALIFICATION_OUTPUT_HEIGHT");
    const int outputWidth = qualification
        ? (requestedOutputWidth > 0 ? requestedOutputWidth : 3840)
        : 640;
    const int outputHeight = qualification
        ? (requestedOutputHeight > 0 ? requestedOutputHeight : 2160)
        : 360;
    window->resize(outputWidth, outputHeight);
    QObject::connect(
        capabilities,
        &UpscalingCapabilityProbe::resultChanged,
        &application,
        [capabilities] {
            if (!capabilities->ready())
                return;
            printResult({
                {QStringLiteral("event"),
                 QStringLiteral("graphics-capability")},
                {QStringLiteral("capability"),
                 QJsonObject::fromVariantMap(capabilities->result())},
            });
        });
    if (!offline)
        capabilities->observe(window);

    bool playerStarted = false;
    QObject::connect(
        assets,
        &UpscalingAssetManager::stateChanged,
        &application,
        [&, window](const QString &changedId) {
            if (changedId != taskId || playerStarted)
                return;
            const QVariantMap state = assets->stateFor(taskId);
            const QString phase = state.value(
                QStringLiteral("phase")).toString();
            fprintf(stderr, "asset phase=%s received=%lld total=%lld\n",
                phase.toUtf8().constData(),
                state.value(QStringLiteral("bytesReceived")).toLongLong(),
                state.value(QStringLiteral("bytesTotal")).toLongLong());
            if (phase == QStringLiteral("failed")) {
                printResult({
                    {QStringLiteral("result"), QStringLiteral("download-failed")},
                    {QStringLiteral("errorCode"),
                     state.value(QStringLiteral("errorCode")).toString()},
                });
                application.exit(2);
                return;
            }
            if (phase != QStringLiteral("ready"))
                return;
            playerStarted = true;

            QStringList shaderPaths;
            QHash<QString, QString> pathById;
            for (const auto &artifact : artifacts)
                pathById.insert(artifact.id, assets->absolutePath(artifact));
            for (const QString &id : profile.orderedShaderArtifactIds)
                shaderPaths.append(pathById.value(id));

            const QVariantMap runtime {
                {QStringLiteral("schema"), 1},
                {QStringLiteral("enabled"), true},
                {QStringLiteral("providerId"), profile.providerId},
                {QStringLiteral("profileId"), profile.profileId},
                {QStringLiteral("modelVersion"), profile.version},
                {QStringLiteral("backend"), QVariantMap {
                    {QStringLiteral("kind"),
                     YanamiUpscaling::UpscalingCatalog::backendKindId(
                         profile.backendKind)},
                }},
                {QStringLiteral("orderedShaderPaths"), shaderPaths},
                {QStringLiteral("options"), profile.mpvOptions},
                {QStringLiteral("performanceProtection"),
                 monitorPerformance},
                {QStringLiteral("reservedHeadroomPercent"), 20},
            };

            // The hosted CI path verifies the complete catalog -> validated
            // runtime -> asynchronous libmpv property chain without creating
            // a scene graph or requiring a graphics device.
            if (offline && scenario.isEmpty()) {
                auto *player = new MpvVideoItem(nullptr, assetRoot);
                player->setParent(&application);
                QObject::connect(
                    player,
                    &MpvVideoItem::upscalingConfigurationFinished,
                    &application,
                    [&, player, shaderCount = shaderPaths.size()](
                        bool enabled,
                        bool success,
                        double dispatchCpuMs,
                        double completionMs) {
                        const int networkRequests = offlineNetwork
                            ? offlineNetwork->requestCount() : -1;
                        const bool passed = enabled
                            && success
                            && player->upscalingActive()
                            && !player->upscalingConfigurationPending()
                            && player->effectiveUpscalingProfile()
                                == profile.profileId
                            && networkRequests == 0;
                        printResult({
                            {QStringLiteral("result"), passed
                                ? QStringLiteral("pass")
                                : QStringLiteral("configuration-failed")},
                            {QStringLiteral("active"),
                             player->upscalingActive()},
                            {QStringLiteral("pending"),
                             player->upscalingConfigurationPending()},
                            {QStringLiteral("networkRequests"),
                             networkRequests},
                            {QStringLiteral("shaderCount"),
                             shaderCount},
                            {QStringLiteral("dispatchCpuMs"), dispatchCpuMs},
                            {QStringLiteral("completionMs"), completionMs},
                            {QStringLiteral("profileId"),
                             player->effectiveUpscalingProfile()},
                        });
                        application.exit(passed ? 0 : 16);
                    });
                if (!player->configureUpscaling(runtime)) {
                    printResult({
                        {QStringLiteral("result"),
                         QStringLiteral("configuration-rejected")},
                    });
                    application.exit(16);
                }
                return;
            }

            const QString mediaDirectory = offline
                ? offlineMediaDirectory.path()
                : QDir(QStandardPaths::writableLocation(
                      QStandardPaths::TempLocation))
                      .filePath(QStringLiteral("yanami-upscaling-mpv-smoke"));
            QDir().mkpath(mediaDirectory);
            const QString suppliedQualificationMedia =
                QString::fromLocal8Bit(qgetenv(
                    "YANAMI_UPSCALING_QUALIFICATION_MEDIA")).trimmed();
            const QString mediaPath = qualification
                ? suppliedQualificationMedia
                : QDir(mediaDirectory).filePath(
                      QStringLiteral("fixture-a.y4m"));
            const QString secondMediaPath = QDir(mediaDirectory).filePath(
                QStringLiteral("fixture-b.y4m"));
            if ((qualification
                    && (mediaPath.isEmpty()
                        || !QFileInfo::exists(mediaPath)))
                || (!qualification && !createY4mFixture(mediaPath))
                || (pendingOpen
                    && !createY4mFixture(secondMediaPath, 24))) {
                application.exit(3);
                return;
            }

            auto *player = new MpvVideoItem(window->contentItem(), assetRoot);
            auto benchmarkSamples = std::make_shared<QList<double>>();
            player->setWidth(window->width());
            player->setHeight(window->height());
            const QString badShaderPath = QDir(assetRoot).filePath(
                QStringLiteral("anime4k/smoke-invalid-shader.glsl"));
            QObject::connect(window, &QQuickWindow::widthChanged,
                player, [window, player] { player->setWidth(window->width()); });
            QObject::connect(window, &QQuickWindow::heightChanged,
                player, [window, player] { player->setHeight(window->height()); });
            QObject::connect(
                player,
                &MpvVideoItem::upscalingConfigurationFinished,
                player,
                [player, runtime, benchmarkSamples, toggleBenchmark,
                 &application](bool enabled, bool success,
                               double dispatchCpuMs, double completionMs) {
                    if (toggleBenchmark
                        && player->property(
                            "smokeBenchmarkRunning").toBool()) {
                        if (!success) {
                            printResult({
                                {QStringLiteral("result"),
                                 QStringLiteral("benchmark-apply-failed")},
                                {QStringLiteral("sampleCount"),
                                 benchmarkSamples->size()},
                            });
                            application.exit(16);
                            return;
                        }
                        benchmarkSamples->append(dispatchCpuMs);
                        if (benchmarkSamples->size() >= 100) {
                            QList<double> sorted = *benchmarkSamples;
                            std::sort(sorted.begin(), sorted.end());
                            const qsizetype p95Index = std::max<qsizetype>(
                                0,
                                static_cast<qsizetype>(std::ceil(
                                    sorted.size() * 0.95)) - 1);
                            const double p95 = sorted.at(p95Index);
                            const double maximum = sorted.constLast();
                            const bool active =
                                player->upscalingActive();
                            const bool pending =
                                player->upscalingConfigurationPending();
                            const int fileLoads = player->property(
                                "smokeFileLoads").toInt();
                            const bool passed = p95 <= 5.0
                                && maximum <= 20.0 && active && !pending
                                && fileLoads == 1;
                            printResult({
                                {QStringLiteral("result"), passed
                                    ? QStringLiteral("pass")
                                    : QStringLiteral("benchmark-budget-failed")},
                                {QStringLiteral("sampleCount"),
                                 sorted.size()},
                                {QStringLiteral("p95DispatchCpuMs"), p95},
                                {QStringLiteral("maxDispatchCpuMs"), maximum},
                                {QStringLiteral("active"), active},
                                {QStringLiteral("pending"), pending},
                                {QStringLiteral("fileLoads"), fileLoads},
                            });
                            application.exit(passed ? 0 : 17);
                            return;
                        }
                        const bool nextEnabled =
                            !player->upscalingActive();
                        QTimer::singleShot(
                            0,
                            &application,
                            [player, runtime, nextEnabled] {
                                player->configureUpscaling(nextEnabled
                                    ? runtime
                                    : QVariantMap {
                                        {QStringLiteral("schema"), 1},
                                        {QStringLiteral("enabled"), false},
                                    });
                            });
                    }
                    if (!enabled)
                        return;
                    player->setProperty(
                        "smokeEnableCompletions",
                        player->property("smokeEnableCompletions").toInt()
                            + 1);
                    player->setProperty(
                        "smokeEnableSucceeded", success);
                    player->setProperty(
                        "smokeEnableDispatchCpuMs", dispatchCpuMs);
                    player->setProperty(
                        "smokeEnableCompletionMs", completionMs);
                });
            QObject::connect(
                player,
                &MpvVideoItem::upscalingConfigurationPendingChanged,
                player,
                [player] {
                    player->setProperty(
                        "smokePendingTransitions",
                        player->property("smokePendingTransitions").toInt()
                            + 1);
                });
            QObject::connect(
                player,
                &MpvVideoItem::playbackError,
                &application,
                [&](const QString &message) {
                    printResult({
                        {QStringLiteral("result"),
                         QStringLiteral("playback-error")},
                        {QStringLiteral("message"), message},
                    });
                    application.exit(7);
                });
            QObject::connect(
                player,
                &MpvVideoItem::upscalingFallback,
                &application,
                [&, player, badShaderPath](
                    const QString &, const QString &errorCode,
                    const QString &) {
                    if (failureRollback) {
                        const bool inactive = !player->upscalingActive();
                        const bool pending =
                            player->upscalingConfigurationPending();
                        const int fileLoads = player->property(
                            "smokeFileLoads").toInt();
                        const bool expectedError =
                            errorCode == QStringLiteral("shader-compile")
                            || errorCode
                                == QStringLiteral("mpv-shader-list-rejected");
                        const bool passed = inactive && !pending
                            && expectedError && fileLoads == 1;
                        QFile::remove(badShaderPath);
                        printResult({
                            {QStringLiteral("result"), passed
                                ? QStringLiteral("pass")
                                : QStringLiteral("rollback-failed")},
                            {QStringLiteral("errorCode"), errorCode},
                            {QStringLiteral("active"), !inactive},
                            {QStringLiteral("pending"), pending},
                            {QStringLiteral("fileLoads"), fileLoads},
                        });
                        application.exit(passed ? 0 : 9);
                        return;
                    }
                    printResult({
                        {QStringLiteral("result"), QStringLiteral("fallback")},
                        {QStringLiteral("errorCode"), errorCode},
                        {QStringLiteral("active"), player->upscalingActive()},
                    });
                    application.exit(4);
                });
            QObject::connect(
                player,
                &MpvVideoItem::fileLoaded,
                &application,
                [&, player, runtime, mediaPath, badShaderPath,
                 benchmarkSamples] {
                    const int fileLoads =
                        player->property("smokeFileLoads").toInt() + 1;
                    player->setProperty("smokeFileLoads", fileLoads);
                    if (pendingOpen) {
                        QTimer::singleShot(
                            500,
                            &application,
                            [&, player] {
                                const bool active =
                                    player->upscalingActive();
                                const bool pending =
                                    player->upscalingConfigurationPending();
                                const bool pendingAtReopen =
                                    player->property(
                                        "smokePendingAtReopen").toBool();
                                const int finalFileLoads = player->property(
                                    "smokeFileLoads").toInt();
                                const double duration = player->duration();
                                const bool passed = active && !pending
                                    && pendingAtReopen && finalFileLoads == 1
                                    && duration > 0.8 && duration < 1.2;
                                printResult({
                                    {QStringLiteral("result"), passed
                                        ? QStringLiteral("pass")
                                        : QStringLiteral("pending-open-failed")},
                                    {QStringLiteral("active"), active},
                                    {QStringLiteral("pending"), pending},
                                    {QStringLiteral("pendingAtReopen"),
                                     pendingAtReopen},
                                    {QStringLiteral("fileLoads"),
                                     finalFileLoads},
                                    {QStringLiteral("duration"), duration},
                                    {QStringLiteral("profileId"),
                                     player->effectiveUpscalingProfile()},
                                });
                                application.exit(passed ? 0 : 11);
                            });
                        return;
                    }
                    if (player->property("smokeScenarioStarted").toBool())
                        return;
                    if (toggleBenchmark) {
                        player->setProperty("smokeScenarioStarted", true);
                        benchmarkSamples->clear();
                        player->setProperty("smokeBenchmarkRunning", true);
                        player->configureUpscaling({
                            {QStringLiteral("schema"), 1},
                            {QStringLiteral("enabled"), false},
                        });
                        return;
                    }
                    if (rapidDisable) {
                        player->setProperty("smokeScenarioStarted", true);
                        QTimer::singleShot(
                            250,
                            &application,
                            [&, player, runtime] {
                                QElapsedTimer enableTimer;
                                enableTimer.start();
                                const bool enableAccepted =
                                    player->configureUpscaling(runtime);
                                const double enableCallMs =
                                    enableTimer.nsecsElapsed() / 1'000'000.0;
                                const bool pendingAfterEnable =
                                    player->upscalingConfigurationPending();
                                QElapsedTimer disableTimer;
                                disableTimer.start();
                                const bool disableAccepted =
                                    player->configureUpscaling({
                                        {QStringLiteral("schema"), 1},
                                        {QStringLiteral("enabled"), false},
                                    });
                                const double disableCallMs =
                                    disableTimer.nsecsElapsed() / 1'000'000.0;
                                QTimer::singleShot(
                                    750,
                                    &application,
                                    [&, player, enableAccepted,
                                     enableCallMs, pendingAfterEnable,
                                     disableAccepted, disableCallMs] {
                                        const bool active =
                                            player->upscalingActive();
                                        const bool pending =
                                            player->upscalingConfigurationPending();
                                        const int finalFileLoads =
                                            player->property(
                                                "smokeFileLoads").toInt();
                                        const int enableCompletions =
                                            player->property(
                                                "smokeEnableCompletions").toInt();
                                        const bool passed = enableAccepted
                                            && disableAccepted
                                            && pendingAfterEnable
                                            && enableCallMs <= 20.0
                                            && disableCallMs <= 20.0
                                            && !active && !pending
                                            && finalFileLoads == 1
                                            && enableCompletions == 0;
                                        printResult({
                                            {QStringLiteral("result"), passed
                                                ? QStringLiteral("pass")
                                                : QStringLiteral("rapid-disable-failed")},
                                            {QStringLiteral("active"), active},
                                            {QStringLiteral("pending"), pending},
                                            {QStringLiteral("fileLoads"),
                                             finalFileLoads},
                                            {QStringLiteral("enableCompletions"),
                                             enableCompletions},
                                            {QStringLiteral("enableCallMs"),
                                             enableCallMs},
                                            {QStringLiteral("disableCallMs"),
                                             disableCallMs},
                                        });
                                        application.exit(passed ? 0 : 15);
                                    });
                            });
                        return;
                    }
                    if (rapidToggle) {
                        player->setProperty("smokeScenarioStarted", true);
                        QTimer::singleShot(
                            250,
                            &application,
                            [&, player, runtime] {
                                QElapsedTimer disableTimer;
                                disableTimer.start();
                                const bool disableAccepted =
                                    player->configureUpscaling({
                                        {QStringLiteral("schema"), 1},
                                        {QStringLiteral("enabled"), false},
                                    });
                                const double disableCallMs =
                                    disableTimer.nsecsElapsed() / 1'000'000.0;
                                const bool pendingAfterDisable =
                                    player->upscalingConfigurationPending();
                                QElapsedTimer enableTimer;
                                enableTimer.start();
                                const bool enableAccepted =
                                    player->configureUpscaling(runtime);
                                const double enableCallMs =
                                    enableTimer.nsecsElapsed() / 1'000'000.0;
                                QTimer::singleShot(
                                    750,
                                    &application,
                                    [&, player, disableAccepted,
                                     disableCallMs, pendingAfterDisable,
                                     enableAccepted, enableCallMs] {
                                        const bool active =
                                            player->upscalingActive();
                                        const bool pending =
                                            player->upscalingConfigurationPending();
                                        const int finalFileLoads =
                                            player->property(
                                                "smokeFileLoads").toInt();
                                        const int enableCompletions =
                                            player->property(
                                                "smokeEnableCompletions").toInt();
                                        const bool passed = disableAccepted
                                            && enableAccepted
                                            && pendingAfterDisable
                                            && disableCallMs <= 20.0
                                            && enableCallMs <= 20.0
                                            && active && !pending
                                            && finalFileLoads == 1
                                            && enableCompletions == 2;
                                        printResult({
                                            {QStringLiteral("result"), passed
                                                ? QStringLiteral("pass")
                                                : QStringLiteral("rapid-toggle-failed")},
                                            {QStringLiteral("active"), active},
                                            {QStringLiteral("pending"), pending},
                                            {QStringLiteral("fileLoads"),
                                             finalFileLoads},
                                            {QStringLiteral("enableCompletions"),
                                             enableCompletions},
                                            {QStringLiteral("disableCallMs"),
                                             disableCallMs},
                                            {QStringLiteral("enableCallMs"),
                                             enableCallMs},
                                        });
                                        application.exit(passed ? 0 : 10);
                                    });
                            });
                        return;
                    }
                    if (pendingStop) {
                        player->setProperty("smokeScenarioStarted", true);
                        QTimer::singleShot(
                            250,
                            &application,
                            [&, player, runtime] {
                                const bool accepted =
                                    player->configureUpscaling(runtime);
                                const bool pendingBeforeStop =
                                    player->upscalingConfigurationPending();
                                player->stop();
                                QTimer::singleShot(
                                    600,
                                    &application,
                                    [&, player, accepted,
                                     pendingBeforeStop] {
                                        const bool active =
                                            player->upscalingActive();
                                        const bool pending =
                                            player->upscalingConfigurationPending();
                                        const bool idle = player->playbackState()
                                            == MpvVideoItem::PlaybackState::Idle;
                                        const int finalFileLoads =
                                            player->property(
                                                "smokeFileLoads").toInt();
                                        const bool passed = accepted
                                            && pendingBeforeStop
                                            && !active && !pending && idle
                                            && finalFileLoads == 1;
                                        printResult({
                                            {QStringLiteral("result"), passed
                                                ? QStringLiteral("pass")
                                                : QStringLiteral("pending-stop-failed")},
                                            {QStringLiteral("active"), active},
                                            {QStringLiteral("pending"), pending},
                                            {QStringLiteral("idle"), idle},
                                            {QStringLiteral("fileLoads"),
                                             finalFileLoads},
                                        });
                                        application.exit(passed ? 0 : 12);
                                    });
                            });
                        return;
                    }
                    if (stopConfigure) {
                        player->setProperty("smokeScenarioStarted", true);
                        QTimer::singleShot(
                            250,
                            &application,
                            [&, player, runtime] {
                                player->configureUpscaling(runtime);
                                const bool pendingBeforeStop =
                                    player->upscalingConfigurationPending();
                                player->stop();
                                const bool acceptedAfterStop =
                                    player->configureUpscaling(runtime);
                                QTimer::singleShot(
                                    750,
                                    &application,
                                    [&, player, pendingBeforeStop,
                                     acceptedAfterStop] {
                                        const bool active =
                                            player->upscalingActive();
                                        const bool pending =
                                            player->upscalingConfigurationPending();
                                        const bool idle = player->playbackState()
                                            == MpvVideoItem::PlaybackState::Idle;
                                        const int enableCompletions =
                                            player->property(
                                                "smokeEnableCompletions").toInt();
                                        const bool passed = pendingBeforeStop
                                            && acceptedAfterStop && active
                                            && !pending && idle
                                            && enableCompletions == 2;
                                        printResult({
                                            {QStringLiteral("result"), passed
                                                ? QStringLiteral("pass")
                                                : QStringLiteral("stop-configure-failed")},
                                            {QStringLiteral("active"), active},
                                            {QStringLiteral("pending"), pending},
                                            {QStringLiteral("idle"), idle},
                                            {QStringLiteral("enableCompletions"),
                                             enableCompletions},
                                        });
                                        application.exit(passed ? 0 : 13);
                                    });
                            });
                        return;
                    }
                    if (failureRollback) {
                        player->setProperty("smokeScenarioStarted", true);
                        QTimer::singleShot(
                            250,
                            &application,
                            [&, player, runtime, badShaderPath] {
                                QFile badShader(badShaderPath);
                                if (!badShader.open(
                                        QIODevice::WriteOnly
                                        | QIODevice::Truncate)) {
                                    application.exit(3);
                                    return;
                                }
                                badShader.write(
                                    "//!HOOK MAIN\n//!BIND HOOKED\n"
                                    "vec4 hook() { this_is_not_glsl; }\n");
                                badShader.close();
                                QVariantMap invalidRuntime = runtime;
                                invalidRuntime.insert(
                                    QStringLiteral("orderedShaderPaths"),
                                    QStringList {badShaderPath});
                                player->configureUpscaling(invalidRuntime);
                            });
                        return;
                    }
                    if (originalPlayback) {
                        QTimer::singleShot(
                            500,
                            &application,
                            [&, player] {
                                const bool active =
                                    player->upscalingActive();
                                const bool pending =
                                    player->upscalingConfigurationPending();
                                const int pendingTransitions =
                                    player->property(
                                        "smokePendingTransitions").toInt();
                                const bool passed = !active && !pending
                                    && pendingTransitions == 0;
                                printResult({
                                    {QStringLiteral("result"), passed
                                        ? QStringLiteral("pass")
                                        : QStringLiteral("original-failed")},
                                    {QStringLiteral("active"), active},
                                    {QStringLiteral("pending"), pending},
                                    {QStringLiteral("pendingTransitions"),
                                     pendingTransitions},
                                });
                                application.exit(passed ? 0 : 14);
                            });
                        return;
                    }
                    if (runtimeToggle) {
                        QTimer::singleShot(
                            350,
                            &application,
                            [&, player, runtime] {
                                QElapsedTimer disableTimer;
                                disableTimer.start();
                                const bool disableApplied =
                                    player->configureUpscaling({
                                        {QStringLiteral("schema"), 1},
                                        {QStringLiteral("enabled"), false},
                                    });
                                const double disableApplyMs =
                                    disableTimer.nsecsElapsed() / 1'000'000.0;
                                const bool inactiveAfterDisable =
                                    !player->upscalingActive();
                                QTimer::singleShot(
                                    150,
                                    &application,
                                    [&, player, runtime, disableApplied,
                                     disableApplyMs, inactiveAfterDisable] {
                                        QElapsedTimer enableTimer;
                                        enableTimer.start();
                                        const bool enableApplied =
                                            player->configureUpscaling(runtime);
                                        const double enableApplyMs =
                                            enableTimer.nsecsElapsed()
                                            / 1'000'000.0;
                                        QTimer::singleShot(
                                            650,
                                            &application,
                                            [&, player, disableApplied,
                                             disableApplyMs,
                                             inactiveAfterDisable,
                                             enableApplied,
                                             enableApplyMs] {
                                                const bool activeAfterEnable =
                                                    player->upscalingActive();
                                                const int finalFileLoads =
                                                    player->property(
                                                        "smokeFileLoads")
                                                        .toInt();
                                                const bool completionSucceeded =
                                                    player->property(
                                                        "smokeEnableSucceeded")
                                                        .toBool();
                                                const double asyncDispatchCpuMs =
                                                    player->property(
                                                        "smokeEnableDispatchCpuMs")
                                                        .toDouble();
                                                const double enableCompletionMs =
                                                    player->property(
                                                        "smokeEnableCompletionMs")
                                                        .toDouble();
                                                const bool dispatchWithinBudget =
                                                    disableApplyMs <= 20.0
                                                    && enableApplyMs <= 20.0
                                                    && asyncDispatchCpuMs <= 20.0;
                                                const bool passed =
                                                    disableApplied
                                                    && inactiveAfterDisable
                                                    && enableApplied
                                                    && activeAfterEnable
                                                    && completionSucceeded
                                                    && dispatchWithinBudget
                                                    && finalFileLoads == 1;
                                                printResult({
                                                    {QStringLiteral("result"),
                                                     passed
                                                         ? QStringLiteral("pass")
                                                         : QStringLiteral("toggle-failed")},
                                                    {QStringLiteral("active"),
                                                     activeAfterEnable},
                                                    {QStringLiteral("fileLoads"),
                                                     finalFileLoads},
                                                    {QStringLiteral(
                                                         "disableApplyMs"),
                                                     disableApplyMs},
                                                    {QStringLiteral(
                                                         "enableApplyMs"),
                                                     enableApplyMs},
                                                    {QStringLiteral(
                                                         "asyncDispatchCpuMs"),
                                                     asyncDispatchCpuMs},
                                                    {QStringLiteral(
                                                         "enableCompletionMs"),
                                                     enableCompletionMs},
                                                    {QStringLiteral(
                                                         "dispatchWithinBudget"),
                                                     dispatchWithinBudget},
                                                    {QStringLiteral("profileId"),
                                                     player->effectiveUpscalingProfile()},
                                                });
                                                application.exit(passed ? 0 : 8);
                                            });
                                    });
                            });
                        return;
                    }
                    QTimer::singleShot(
                        qualification ? 12'000
                                      : (performanceProtection ? 3500 : 1500),
                        &application,
                        [&, player] {
                            const bool active = player->upscalingActive();
                            const int networkRequests = offlineNetwork
                                ? offlineNetwork->requestCount() : -1;
                            const bool offlineBoundaryHeld = !offline
                                || networkRequests == 0;
                            printResult({
                                {QStringLiteral("result"),
                                 active && offlineBoundaryHeld
                                     ? QStringLiteral("pass")
                                     : (active
                                           ? QStringLiteral("network-used")
                                           : QStringLiteral("inactive"))},
                                {QStringLiteral("active"), active},
                                {QStringLiteral("offline"), offline},
                                {QStringLiteral("networkRequests"),
                                 networkRequests},
                                {QStringLiteral("profileId"),
                                 player->effectiveUpscalingProfile()},
                            });
                            application.exit(
                                active && offlineBoundaryHeld ? 0 : 5);
                        });
                });

            window->show();
            fprintf(stderr, "player starting profile=%s shaders=%lld\n",
                profile.profileId.toUtf8().constData(),
                static_cast<long long>(shaderPaths.size()));
            if (originalPlayback || rapidDisable) {
                player->open(QUrl::fromLocalFile(mediaPath), {});
            } else if (pendingOpen) {
                player->openWithUpscaling(
                    QUrl::fromLocalFile(mediaPath), {}, runtime);
                player->setProperty(
                    "smokePendingAtReopen",
                    player->upscalingConfigurationPending());
                player->openWithUpscaling(
                    QUrl::fromLocalFile(secondMediaPath), {}, runtime);
            } else {
                player->openWithUpscaling(
                    QUrl::fromLocalFile(mediaPath), {}, runtime);
            }
        });

    QTimer::singleShot(offline ? 15'000 : 60'000, &application, [&] {
        printResult({
            {QStringLiteral("result"), QStringLiteral("timeout")},
        });
        application.exit(6);
    });
    assets->download(taskId, artifacts);
    const int result = application.exec();
    delete window;
    YanamiPerformance::PerformanceTrace::shutdown();
    return result;
}
