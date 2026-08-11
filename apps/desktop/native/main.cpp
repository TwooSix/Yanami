#include <QGuiApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QIcon>
#include <QMessageLogContext>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

#include "MpvVideoItem.hpp"
#include "BackendController.hpp"
#include "LocaleController.hpp"
#include "WindowController.hpp"

namespace {
QString developmentLogPath;

void developmentMessageHandler(
    QtMsgType type,
    const QMessageLogContext &context,
    const QString &message)
{
    QFile logFile(developmentLogPath);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    const char *level = "INFO";
    if (type == QtDebugMsg)
        level = "DEBUG";
    else if (type == QtWarningMsg)
        level = "WARNING";
    else if (type == QtCriticalMsg)
        level = "CRITICAL";
    else if (type == QtFatalMsg)
        level = "FATAL";

    QTextStream stream(&logFile);
    stream << level << ": " << message;
    if (context.file)
        stream << " (" << context.file << ':' << context.line << ')';
    stream << '\n';
}
}

int main(int argc, char *argv[])
{
    QGuiApplication::setApplicationName(QStringLiteral("Yanami"));
    QGuiApplication::setOrganizationName(QStringLiteral("Yanami"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("yanami.local"));

    developmentLogPath = qEnvironmentVariable("YANAMI_DEV_LOG_PATH");
    if (!developmentLogPath.isEmpty())
        qInstallMessageHandler(developmentMessageHandler);

    // libmpv's render API is OpenGL. Keeping Qt on the same graphics API avoids
    // cross-API copies and lets controls and video share one scene graph.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickWindow::setDefaultAlphaBuffer(false);

    QGuiApplication app(argc, argv);
    qmlRegisterType<MpvVideoItem>("Yanami.Native", 1, 0, "MpvVideoItem");

    BackendController backend;
    LocaleController localeController(nullptr);
    WindowController windowController;
    QQmlApplicationEngine engine;
    localeController.setEngine(&engine);
    const QString autoplayItemId = qEnvironmentVariable("YANAMI_DEV_AUTOPLAY_ITEM_ID");
    const QString autoplayRecentTitle = qEnvironmentVariable("YANAMI_DEV_AUTOPLAY_RECENT_TITLE");
    const bool autoplayResumeFirst = qEnvironmentVariableIsSet("YANAMI_DEV_AUTOPLAY_RESUME_FIRST");
    if (!autoplayRecentTitle.isEmpty()) {
        const auto autoplayStarted = std::make_shared<bool>(false);
        const auto tryAutoplayRecent = [&backend, autoplayRecentTitle, autoplayStarted] {
            if (*autoplayStarted)
                return;
            QVariantList items = backend.recentItems();
            items.append(backend.resumeItems());
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
            backend.preparePlayback(itemId);
        };
        QObject::connect(
            &backend,
            &BackendController::recentItemsChanged,
            &backend,
            tryAutoplayRecent);
        QObject::connect(
            &backend,
            &BackendController::resumeItemsChanged,
            &backend,
            tryAutoplayRecent);
        QTimer::singleShot(0, &backend, tryAutoplayRecent);
    } else if (autoplayResumeFirst) {
        const auto autoplayStarted = std::make_shared<bool>(false);
        const auto tryAutoplayResume = [&backend, autoplayStarted] {
            if (*autoplayStarted)
                return;
            const QVariantList items = backend.resumeItems();
            if (items.isEmpty())
                return;
            const QString itemId = items.constFirst().toMap().value(QStringLiteral("id")).toString();
            if (itemId.isEmpty())
                return;
            *autoplayStarted = true;
            backend.preparePlayback(itemId);
        };
        QObject::connect(
            &backend,
            &BackendController::resumeItemsChanged,
            &backend,
            tryAutoplayResume);
        QTimer::singleShot(0, &backend, tryAutoplayResume);
    } else if (!autoplayItemId.isEmpty()) {
        QTimer::singleShot(0, &backend, [&backend, autoplayItemId] {
            backend.preparePlayback(autoplayItemId);
        });
    } else if (qEnvironmentVariableIsSet("YANAMI_DEV_AUTOPLAY_FIRST")) {
        QObject::connect(
            &backend,
            &BackendController::mediaItemsChanged,
            &backend,
            [&backend] {
                const QVariantList items = backend.mediaItems();
                if (items.isEmpty())
                    return;
                const QString itemId = items.constFirst().toMap().value(QStringLiteral("id")).toString();
                if (!itemId.isEmpty())
                    backend.preparePlayback(itemId);
            },
            Qt::SingleShotConnection);
    }

    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &localeController);
    engine.rootContext()->setContextProperty(QStringLiteral("windowShell"), &windowController);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);
    engine.loadFromModule("Yanami", "Main");

    const QString developmentLanguage = qEnvironmentVariable("YANAMI_DEV_SWITCH_LANGUAGE");
    if (!developmentLanguage.isEmpty()) {
        QTimer::singleShot(500, &localeController, [&localeController, developmentLanguage] {
            localeController.setLanguage(developmentLanguage);
        });
    }

    if (!engine.rootObjects().isEmpty()) {
        auto *root = engine.rootObjects().constFirst();
        if (auto *window = qobject_cast<QWindow *>(root))
            windowController.configureWindow(window);
        if (qEnvironmentVariableIsSet("YANAMI_DEV_RENDER_DIAGNOSTICS")) {
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
        bool seekIsValid = false;
        const int developmentSeekSeconds = qEnvironmentVariableIntValue(
            "YANAMI_DEV_PLAYBACK_SEEK_SECONDS", &seekIsValid);
        if (seekIsValid && developmentSeekSeconds > 0)
            root->setProperty("developmentSeekSeconds", developmentSeekSeconds);
        bool autoStopIsValid = false;
        const int developmentAutoStopMs = qEnvironmentVariableIntValue(
            "YANAMI_DEV_PLAYBACK_AUTOSTOP_MS", &autoStopIsValid);
        if (autoStopIsValid && developmentAutoStopMs > 0)
            root->setProperty("developmentAutoStopMs", developmentAutoStopMs);
        if (qEnvironmentVariableIsSet("YANAMI_DEV_AUTO_SKIP_INTRO"))
            root->setProperty("developmentAutoSkipIntro", true);
        if (qEnvironmentVariableIsSet("YANAMI_DEV_SHOW_LOADING"))
            root->setProperty("developmentLoadingPreview", true);
    }

    const QString screenshotPath = qEnvironmentVariable("YANAMI_DEV_SCREENSHOT_PATH");
    if (!screenshotPath.isEmpty() && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (window) {
            bool pageIsValid = false;
            const int page = qEnvironmentVariableIntValue("YANAMI_DEV_SCREENSHOT_PAGE", &pageIsValid);
            if (pageIsValid)
                window->setProperty("currentPage", page);
            bool delayIsValid = false;
            const int requestedDelay = qEnvironmentVariableIntValue(
                "YANAMI_DEV_SCREENSHOT_DELAY_MS", &delayIsValid);
            const int screenshotDelay = delayIsValid ? qBound(250, requestedDelay, 60000) : 1200;
            QTimer::singleShot(screenshotDelay, window, [window, screenshotPath, &app] {
                const bool saved = window->grabWindow().save(screenshotPath);
                app.exit(saved ? EXIT_SUCCESS : EXIT_FAILURE);
            });
        }
    }
    return app.exec();
}
