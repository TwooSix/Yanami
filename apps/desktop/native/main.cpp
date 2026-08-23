#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

#include "MpvVideoItem.hpp"
#include "ApplicationViewModel.hpp"
#include "AsyncOperationState.hpp"
#include "AsyncResourceState.hpp"
#include "AsyncImageProvider.hpp"
#include "DesktopBackendServices.hpp"
#include "DevelopmentHooks.hpp"
#include "MediaStore.hpp"
#include "LocaleController.hpp"
#include "RuntimeLogger.hpp"
#include "WindowController.hpp"

namespace {
Q_LOGGING_CATEGORY(applicationLog, "yanami.application")
using DevelopmentHook = DevelopmentHooks::Variable;

class RuntimeLoggerGuard final
{
public:
    explicit RuntimeLoggerGuard(bool installed)
        : m_installed(installed)
    {
    }

    ~RuntimeLoggerGuard()
    {
        if (m_installed)
            RuntimeLogger::shutdown();
    }

    RuntimeLoggerGuard(const RuntimeLoggerGuard &) = delete;
    RuntimeLoggerGuard &operator=(const RuntimeLoggerGuard &) = delete;

private:
    bool m_installed = false;
};
}

int main(int argc, char *argv[])
{
    QGuiApplication::setApplicationName(QStringLiteral("Yanami"));
    QGuiApplication::setApplicationVersion(QStringLiteral(YANAMI_VERSION));
    QGuiApplication::setOrganizationName(QStringLiteral("Yanami"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("yanami.local"));

    // libmpv's render API is OpenGL. Keeping Qt on the same graphics API avoids
    // cross-API copies and lets controls and video share one scene graph.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickWindow::setDefaultAlphaBuffer(false);

    QGuiApplication app(argc, argv);
    const bool runtimeSmokeTest = app.arguments().contains(
        QStringLiteral("--runtime-smoke-test"));
    const bool runtimeLoggerInstalled = RuntimeLogger::install();
    RuntimeLoggerGuard runtimeLoggerGuard(runtimeLoggerInstalled);
    if (runtimeLoggerInstalled) {
        qCInfo(applicationLog).noquote()
            << "application_start"
            << "version=" << QCoreApplication::applicationVersion()
            << "qtVersion=" << qVersion()
            << "os=" << QSysInfo::prettyProductName()
            << "kernel=" << QSysInfo::kernelType()
            << "kernelVersion=" << QSysInfo::kernelVersion()
            << "cpuArchitecture=" << QSysInfo::currentCpuArchitecture()
            << "buildAbi=" << QSysInfo::buildAbi()
            << "logPath=" << RuntimeLogger::currentLogPath();
    } else {
        qWarning().noquote()
            << "application_logging_unavailable"
            << "reason=runtime_logger_install_failed";
    }
    app.setWindowIcon(QIcon(QStringLiteral(
        ":/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png")));
    qmlRegisterType<MpvVideoItem>("Yanami.Native", 1, 0, "MpvVideoItem");
    qmlRegisterType<MediaQueryProxyModel>(
        "Yanami.Native", 1, 0, "MediaQueryProxyModel");
    qmlRegisterType<AsyncResourceState>(
        "Yanami.Native", 1, 0, "AsyncResourceState");
    qmlRegisterUncreatableType<AsyncOperationState>(
        "Yanami.Native", 1, 0, "AsyncOperationState",
        QStringLiteral("AsyncOperationState instances are owned by feature view models."));
    DesktopBackendServices backendServices;
    ApplicationViewModel applicationViewModel(backendServices.portSet());
    LocaleController localeController(nullptr);
    WindowController windowController;
    QQmlApplicationEngine engine;
    engine.addImageProvider(
        QStringLiteral("yanami"),
        new AsyncImageProvider(QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("cache"))));
    localeController.setEngine(&engine);
#ifdef YANAMI_ENABLE_DEV_HOOKS
    const QString autoplayItemId = DevelopmentHooks::value(DevelopmentHook::AutoplayItemId);
    QVariantMap autoplayContext;
    const QByteArray autoplayContextJson =
        DevelopmentHooks::value(DevelopmentHook::AutoplayContextJson).toUtf8();
    if (!autoplayContextJson.isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            autoplayContextJson, &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject())
            autoplayContext = document.object().toVariantMap();
        else
            qWarning().noquote() << "development_autoplay_context_invalid";
    }
    const QString autoplayRecentTitle =
        DevelopmentHooks::value(DevelopmentHook::AutoplayRecentTitle);
    const bool autoplayResumeFirst =
        DevelopmentHooks::isSet(DevelopmentHook::AutoplayResumeFirst);
    if (!autoplayRecentTitle.isEmpty()) {
        const auto autoplayStarted = std::make_shared<bool>(false);
        const auto tryAutoplayRecent = [&applicationViewModel, autoplayRecentTitle, autoplayStarted] {
            if (*autoplayStarted)
                return;
            QVariantList items = applicationViewModel.home()->mediaStore()
                ->queryItems(QStringLiteral("recent"));
            items.append(applicationViewModel.home()->mediaStore()
                ->queryItems(QStringLiteral("resume")));
            const auto match = std::find_if(items.cbegin(), items.cend(), [&autoplayRecentTitle](const QVariant &value) {
                return value.toMap()
                    .value(QStringLiteral("title"))
                    .toString()
                    .contains(autoplayRecentTitle, Qt::CaseInsensitive);
            });
            if (match == items.cend())
                return;
            const QString itemId = match->toMap().value(QStringLiteral("id")).toString();
            if (itemId.isEmpty())
                return;
            *autoplayStarted = true;
            applicationViewModel.playback()->prepare(itemId);
        };
        QObject::connect(
            applicationViewModel.home()->mediaStore(),
            &MediaStore::queryChanged,
            &applicationViewModel,
            tryAutoplayRecent);
        QTimer::singleShot(0, &applicationViewModel, tryAutoplayRecent);
    } else if (autoplayResumeFirst) {
        const auto autoplayStarted = std::make_shared<bool>(false);
        const auto tryAutoplayResume = [&applicationViewModel, autoplayStarted] {
            if (*autoplayStarted)
                return;
            const QVariantList items =
                applicationViewModel.home()->mediaStore()
                    ->queryItems(QStringLiteral("resume"));
            if (items.isEmpty())
                return;
            const QString itemId = items.constFirst().toMap().value(QStringLiteral("id")).toString();
            if (itemId.isEmpty())
                return;
            *autoplayStarted = true;
            applicationViewModel.playback()->prepare(itemId);
        };
        QObject::connect(
            applicationViewModel.home()->mediaStore(),
            &MediaStore::queryChanged,
            &applicationViewModel,
            tryAutoplayResume);
        QTimer::singleShot(0, &applicationViewModel, tryAutoplayResume);
    } else if (!autoplayItemId.isEmpty()) {
        auto *autoplayTimer = new QTimer(&applicationViewModel);
        autoplayTimer->setInterval(250);
        QObject::connect(autoplayTimer, &QTimer::timeout,
                         &applicationViewModel,
                         [&applicationViewModel, autoplayItemId, autoplayContext, autoplayTimer] {
            if (!applicationViewModel.session()->connected()
                || applicationViewModel.session()->busy())
                return;
            autoplayTimer->stop();
            applicationViewModel.playback()->prepareInContext(
                autoplayItemId, autoplayContext);
        });
        autoplayTimer->start();
    } else if (DevelopmentHooks::isSet(DevelopmentHook::AutoplayFirst)) {
        const auto autoplayStarted = std::make_shared<bool>(false);
        QObject::connect(
            applicationViewModel.home()->mediaStore(),
            &MediaStore::queryChanged,
            &applicationViewModel,
            [&applicationViewModel, autoplayStarted](const QString &kind, const QString &) {
                if (*autoplayStarted || kind != QStringLiteral("library"))
                    return;
                const QVariantList items =
                    applicationViewModel.home()->mediaStore()
                        ->queryItems(QStringLiteral("library"));
                if (items.isEmpty())
                    return;
                const QString itemId = items.constFirst().toMap().value(QStringLiteral("id")).toString();
                if (!itemId.isEmpty()) {
                    *autoplayStarted = true;
                    applicationViewModel.playback()->prepare(itemId);
                }
            });
    }
#endif

    engine.rootContext()->setContextProperty(
        QStringLiteral("app"), &applicationViewModel);
    engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &localeController);
    engine.rootContext()->setContextProperty(QStringLiteral("windowShell"), &windowController);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);
    engine.loadFromModule("Yanami", "Main");

#ifdef YANAMI_ENABLE_DEV_HOOKS
    const QString developmentLanguage =
        DevelopmentHooks::value(DevelopmentHook::SwitchLanguage);
    if (!developmentLanguage.isEmpty()) {
        QTimer::singleShot(500, &localeController, [&localeController, developmentLanguage] {
            localeController.setLanguage(developmentLanguage);
        });
    }
#endif

    if (!engine.rootObjects().isEmpty()) {
        auto *root = engine.rootObjects().constFirst();
        if (auto *window = qobject_cast<QWindow *>(root))
            windowController.configureWindow(window);
#ifdef YANAMI_ENABLE_DEV_HOOKS
        if (DevelopmentHooks::isSet(DevelopmentHook::RenderDiagnostics)) {
            root->setProperty("developmentRenderDiagnostics", true);
            if (auto *quickWindow = qobject_cast<QQuickWindow *>(root)) {
                struct FrameTimingState {
                    QElapsedTimer clock;
                    qint64 lastFrameNs = 0;
                    qint64 reportStartNs = 0;
                    quint64 frames = 0;
                    quint64 over20Ms = 0;
                    quint64 over33Ms = 0;
                    double totalIntervalMs = 0;
                    double maximumIntervalMs = 0;
                };
                const auto timing = std::make_shared<FrameTimingState>();
                timing->clock.start();
                QObject::connect(
                    quickWindow,
                    &QQuickWindow::frameSwapped,
                    quickWindow,
                    [timing] {
                        const qint64 now = timing->clock.nsecsElapsed();
                        if (timing->lastFrameNs != 0) {
                            const double intervalMs = (now - timing->lastFrameNs) / 1'000'000.0;
                            ++timing->frames;
                            timing->totalIntervalMs += intervalMs;
                            timing->maximumIntervalMs = std::max(timing->maximumIntervalMs, intervalMs);
                            if (intervalMs > 20.0)
                                ++timing->over20Ms;
                            if (intervalMs > 33.34)
                                ++timing->over33Ms;
                        }
                        timing->lastFrameNs = now;
                        if (timing->reportStartNs == 0)
                            timing->reportStartNs = now;
                        if (now - timing->reportStartNs < 1'000'000'000)
                            return;
                        const double elapsedSeconds = (now - timing->reportStartNs) / 1'000'000'000.0;
                        qInfo() << "frame-timing fps=" << std::round(timing->frames / elapsedSeconds * 10.0) / 10.0
                                << "averageMs=" << (timing->frames > 0
                                           ? timing->totalIntervalMs / timing->frames
                                           : 0.0)
                                << "maxMs=" << timing->maximumIntervalMs
                                << "over20Ms=" << timing->over20Ms
                                << "over33Ms=" << timing->over33Ms;
                        timing->reportStartNs = now;
                        timing->frames = 0;
                        timing->over20Ms = 0;
                        timing->over33Ms = 0;
                        timing->totalIntervalMs = 0;
                        timing->maximumIntervalMs = 0;
                    },
                    Qt::DirectConnection);
            }
        }
        const QString developmentSearchQuery =
            DevelopmentHooks::value(DevelopmentHook::SearchQuery);
        if (!developmentSearchQuery.isEmpty())
            root->setProperty("developmentSearchQuery", developmentSearchQuery);
        const QString developmentMediaMenuPreview =
            DevelopmentHooks::value(DevelopmentHook::MediaMenuPreview);
        if (!developmentMediaMenuPreview.isEmpty())
            root->setProperty("developmentMediaMenuPreview", developmentMediaMenuPreview);
        const QString developmentCollectionSequence =
            DevelopmentHooks::value(DevelopmentHook::CollectionSequence);
        if (!developmentCollectionSequence.isEmpty())
            root->setProperty("developmentCollectionSequence", developmentCollectionSequence);
        if (DevelopmentHooks::isSet(DevelopmentHook::ScrollRegression))
            root->setProperty("developmentScrollRegression", true);
        bool scanProgressIsValid = false;
        const double developmentLibraryScanProgress =
            DevelopmentHooks::value(DevelopmentHook::LibraryScanProgress)
                .toDouble(&scanProgressIsValid);
        if (scanProgressIsValid) {
            root->setProperty(
                "developmentLibraryScanProgress",
                std::clamp(developmentLibraryScanProgress, 0.0, 100.0));
        }
        bool seekIsValid = false;
        const int developmentSeekSeconds = DevelopmentHooks::intValue(
            DevelopmentHook::PlaybackSeekSeconds, &seekIsValid);
        if (seekIsValid && developmentSeekSeconds > 0)
            root->setProperty("developmentSeekSeconds", developmentSeekSeconds);
        bool autoStopIsValid = false;
        const int developmentAutoStopMs = DevelopmentHooks::intValue(
            DevelopmentHook::PlaybackAutostopMs, &autoStopIsValid);
        if (autoStopIsValid && developmentAutoStopMs > 0)
            root->setProperty("developmentAutoStopMs", developmentAutoStopMs);
        if (DevelopmentHooks::isSet(DevelopmentHook::AutoSkipIntro))
            root->setProperty("developmentAutoSkipIntro", true);
        if (DevelopmentHooks::isSet(DevelopmentHook::ShowLoading))
            root->setProperty("developmentLoadingPreview", true);
        if (DevelopmentHooks::isSet(DevelopmentHook::ShowDanmakuMenu))
            root->setProperty("developmentDanmakuPreview", true);
        if (DevelopmentHooks::isSet(DevelopmentHook::ShowPlaybackQueue))
            root->setProperty("developmentPlaybackQueuePreview", true);
        const QString developmentDanmakuSearchQuery =
            DevelopmentHooks::value(DevelopmentHook::DanmakuSearchQuery);
        if (!developmentDanmakuSearchQuery.isEmpty())
            root->setProperty("developmentDanmakuSearchQuery", developmentDanmakuSearchQuery);
        bool danmakuPreviewSizeIsValid = false;
        const double developmentDanmakuPreviewFontSize =
            DevelopmentHooks::value(DevelopmentHook::DanmakuPreviewFontSize)
                .toDouble(&danmakuPreviewSizeIsValid);
        if (danmakuPreviewSizeIsValid && developmentDanmakuPreviewFontSize > 0)
            root->setProperty(
                "developmentDanmakuPreviewFontSize",
                developmentDanmakuPreviewFontSize);
        if (DevelopmentHooks::isSet(DevelopmentHook::DisableDanmakuAfterLoad))
            root->setProperty("developmentDisableDanmakuAfterLoad", true);
        if (DevelopmentHooks::isSet(DevelopmentHook::ReenableDanmakuAfterDisable))
            root->setProperty("developmentReenableDanmakuAfterDisable", true);
        const QString developmentLocalMedia =
            DevelopmentHooks::value(DevelopmentHook::LocalMedia);
        if (!developmentLocalMedia.isEmpty()) {
            root->setProperty("developmentLocalMediaUrl",
                              QUrl::fromLocalFile(developmentLocalMedia));
        }
        bool danmakuStressCountIsValid = false;
        const int danmakuStressCount = DevelopmentHooks::intValue(
            DevelopmentHook::DanmakuStyleStressCount,
            &danmakuStressCountIsValid);
        if (danmakuStressCountIsValid && danmakuStressCount > 0)
            root->setProperty("developmentDanmakuStyleStressCount",
                              danmakuStressCount);
        bool danmakuToggleStressIsValid = false;
        const int danmakuToggleStressCount = DevelopmentHooks::intValue(
            DevelopmentHook::DanmakuToggleStressCount,
            &danmakuToggleStressIsValid);
        if (danmakuToggleStressIsValid && danmakuToggleStressCount > 0)
            root->setProperty("developmentDanmakuToggleStressCount",
                              danmakuToggleStressCount);
#endif
    }

    // Release automation exercises the installed application through this
    // production-safe path. Reaching the queued exit proves that the platform
    // plugin, native bridge, and root QML object all loaded successfully.
    if (runtimeSmokeTest) {
        if (engine.rootObjects().isEmpty()) {
            qCCritical(applicationLog) << "runtime_smoke_test_root_missing";
            return EXIT_FAILURE;
        }
        QTimer::singleShot(0, &app, [&app] {
            qCInfo(applicationLog) << "runtime_smoke_test_ready";
            app.exit(EXIT_SUCCESS);
        });
    }

#ifdef YANAMI_ENABLE_DEV_HOOKS
    const QString screenshotPath =
        DevelopmentHooks::value(DevelopmentHook::ScreenshotPath);
    if (!screenshotPath.isEmpty() && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (window) {
            bool widthIsValid = false;
            bool heightIsValid = false;
            const int requestedWidth = DevelopmentHooks::intValue(
                DevelopmentHook::WindowWidth, &widthIsValid);
            const int requestedHeight = DevelopmentHooks::intValue(
                DevelopmentHook::WindowHeight, &heightIsValid);
            if (widthIsValid)
                window->setWidth(std::max(window->minimumWidth(), requestedWidth));
            if (heightIsValid)
                window->setHeight(std::max(window->minimumHeight(), requestedHeight));
            bool pageIsValid = false;
            const int page = DevelopmentHooks::intValue(
                DevelopmentHook::ScreenshotPage, &pageIsValid);
            if (pageIsValid)
                window->setProperty("currentPage", page);
            bool delayIsValid = false;
            const int requestedDelay = DevelopmentHooks::intValue(
                DevelopmentHook::ScreenshotDelayMs, &delayIsValid);
            const int screenshotDelay = delayIsValid ? qBound(250, requestedDelay, 60000) : 1200;
            QTimer::singleShot(screenshotDelay, window, [window, screenshotPath, &app] {
                const bool saved = window->grabWindow().save(screenshotPath);
                app.exit(saved ? EXIT_SUCCESS : EXIT_FAILURE);
            });
        }
    }
#endif
    const int exitCode = app.exec();
    qCInfo(applicationLog).noquote()
        << "application_event_loop_finished"
        << "exitCode=" << exitCode;
    return exitCode;
}
