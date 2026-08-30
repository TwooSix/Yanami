#include <QGuiApplication>
#ifdef Q_OS_WIN
#include <QCursor>
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QIcon>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSaveFile>
#ifdef Q_OS_WIN
#include <QScreen>
#endif
#include <QSGRendererInterface>
#include <QSysInfo>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

#include "ApplicationPaths.hpp"
#include "ApplicationViewModel.hpp"
#include "AsyncOperationState.hpp"
#include "AsyncResourceState.hpp"
#include "BootstrapReadySignal.hpp"
#include "AsyncImageProvider.hpp"
#include "DesktopBackendServices.hpp"
#ifdef Q_OS_WIN
#include "DesktopEntryGuard.hpp"
#endif
#include "DevelopmentHooks.hpp"
#include "InputModalityController.hpp"
#include "MediaStore.hpp"
#include "LocaleController.hpp"
#include "MpvApi.hpp"
#include "MpvVideoItem.hpp"
#include "PerformanceTrace.hpp"
#include "RuntimeLogger.hpp"
#include "WindowController.hpp"

namespace {
Q_LOGGING_CATEGORY(applicationLog, "yanami.application")
using DevelopmentHook = DevelopmentHooks::Variable;

constexpr auto BootstrapReadyOption = "--yanami-bootstrap-ready-file";
constexpr auto BootstrapReadyFileName = "desktop-ready.json";

struct BootstrapHandoffRequest {
    bool supplied = false;
    QString readyFilePath;
    QString rejectionReason;
    BootstrapReadySignal readySignal;

    bool usable() const
    {
        return supplied && rejectionReason.isEmpty()
            && !readyFilePath.isEmpty() && readySignal.valid;
    }
};

BootstrapHandoffRequest bootstrapHandoffFromArguments(
    const QStringList &arguments,
    const QStringList &handoffTemporaryRoots)
{
    BootstrapHandoffRequest request;
    request.readySignal = bootstrapReadySignalFromArguments(arguments);
    const QString option = QString::fromLatin1(BootstrapReadyOption);
    const QString prefix = option + QLatin1Char('=');
    QStringList values;
    for (const QString &argument : arguments) {
        if (argument == option) {
            request.supplied = true;
            values.push_back(QString());
        } else if (argument.startsWith(prefix)) {
            request.supplied = true;
            values.push_back(argument.mid(prefix.size()));
        }
    }

    if (request.readySignal.supplied) {
        request.supplied = true;
        if (!request.readySignal.valid) {
            request.rejectionReason = request.readySignal.rejectionReason;
            return request;
        }
    }

    if (!request.supplied)
        return request;
    if (values.size() != 1) {
        request.rejectionReason = QStringLiteral("option_count");
        return request;
    }
    const QString candidate = values.constFirst();
    if (candidate.isEmpty()) {
        request.rejectionReason = QStringLiteral("empty_path");
        return request;
    }
    if (!QDir::isAbsolutePath(candidate)) {
        request.rejectionReason = QStringLiteral("path_not_absolute");
        return request;
    }

    const QFileInfo readyFile(QDir::cleanPath(candidate));
    if (readyFile.fileName() != QString::fromLatin1(BootstrapReadyFileName)) {
        request.rejectionReason = QStringLiteral("unexpected_file_name");
        return request;
    }
    if (readyFile.exists()) {
        request.rejectionReason = QStringLiteral("target_already_exists");
        return request;
    }

    const QFileInfo parentDirectory(readyFile.absolutePath());
    if (!parentDirectory.exists() || !parentDirectory.isDir()
        || parentDirectory.isSymLink()) {
        request.rejectionReason = QStringLiteral("parent_not_private_directory");
        return request;
    }
    if (!parentDirectory.fileName().startsWith(
            QStringLiteral("YanamiBootstrap-"))) {
        request.rejectionReason = QStringLiteral("unexpected_parent_name");
        return request;
    }

    const BootstrapHandoffParentValidation parentValidation =
        validateBootstrapHandoffParent(
            parentDirectory.absoluteFilePath(), handoffTemporaryRoots);
    if (parentValidation
        == BootstrapHandoffParentValidation::CanonicalPathUnavailable) {
        request.rejectionReason = QStringLiteral("canonical_path_unavailable");
        return request;
    }
    if (parentValidation
        == BootstrapHandoffParentValidation::OutsideTemporaryRoots) {
        request.rejectionReason = QStringLiteral("parent_outside_temp");
        return request;
    }

    request.readyFilePath = readyFile.absoluteFilePath();
    return request;
}

bool publishBootstrapReadyFile(const QString &path, QString *errorCode)
{
    if (QFileInfo::exists(path)) {
        if (errorCode)
            *errorCode = QStringLiteral("target_created_before_publish");
        return false;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("schemaVersion"), QStringLiteral("1.0"));
    payload.insert(QStringLiteral("state"), QStringLiteral("desktop_ready"));
    payload.insert(
        QStringLiteral("processId"),
        static_cast<double>(QCoreApplication::applicationPid()));
    QByteArray serialized =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    serialized.append('\n');

    QSaveFile readyFile(path);
    readyFile.setDirectWriteFallback(false);
    if (!readyFile.open(QIODevice::WriteOnly)) {
        if (errorCode)
            *errorCode = QStringLiteral("open_failed");
        return false;
    }
    if (readyFile.write(serialized) != serialized.size()) {
        readyFile.cancelWriting();
        if (errorCode)
            *errorCode = QStringLiteral("write_failed");
        return false;
    }
    if (!readyFile.commit()) {
        if (errorCode)
            *errorCode = QStringLiteral("commit_failed");
        return false;
    }
    return true;
}

int runMpvRuntimeSmokeTest()
{
    if (!std::setlocale(LC_NUMERIC, "C")) {
        qCritical("mpv runtime smoke could not select the C numeric locale");
        return EXIT_FAILURE;
    }
    QString error;
    const MpvFunctions *functions = MpvApi::instance().load(&error);
    if (!functions) {
        qCritical().noquote() << "mpv runtime smoke failed:" << error;
        return EXIT_FAILURE;
    }
    mpv_handle *handle = functions->create();
    if (!handle) {
        qCritical("mpv runtime smoke could not create a client handle");
        return EXIT_FAILURE;
    }
    functions->terminateDestroy(handle);
    qInfo().noquote()
        << "mpv_runtime_smoke_passed path="
        << QDir::toNativeSeparators(MpvApi::instance().loadedFileName());
    return EXIT_SUCCESS;
}

double percentile(QVector<double> values, double quantile)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const qsizetype index = std::clamp<qsizetype>(
        static_cast<qsizetype>(std::ceil(quantile * values.size()) - 1),
        0,
        values.size() - 1);
    return values.at(index);
}

bool hasCachedHomeContent(const ApplicationViewModel &viewModel)
{
    const HomeViewModel *home = viewModel.home();
    const MediaStore *store = home ? home->mediaStore() : nullptr;
    return store
        && (store->libraryModel()->rowCount() > 0
            || store->resumeModel()->rowCount() > 0
            || !store->queryItems(QStringLiteral("latestSections")).isEmpty());
}

#ifdef Q_OS_WIN
void centerBootstrapWindowOnPointerScreen(QWindow *window)
{
    if (!window)
        return;
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = window->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect available = screen->availableGeometry();
    const QSize size = window->size();
    window->setPosition({
        available.x() + (available.width() - size.width()) / 2,
        available.y() + (available.height() - size.height()) / 2,
    });
}
#endif

QEvent::Type syntheticInteractionProbeEventType()
{
    static const auto type = static_cast<QEvent::Type>(
        QEvent::registerEventType());
    return type;
}

class PerformanceGuiApplication final : public QGuiApplication
{
public:
    struct DispatchStats {
        quint64 dispatchCount = 0;
        quint64 longTaskOver50Count = 0;
        double maxDispatchMs = 0.0;
    };

    PerformanceGuiApplication(int &argc, char **argv)
        : QGuiApplication(argc, argv)
    {
    }

    void beginInteractionProbeWindow()
    {
        m_dispatchStats = {};
        m_interactionProbeActive = true;
    }

    DispatchStats endInteractionProbeWindow()
    {
        m_interactionProbeActive = false;
        return m_dispatchStats;
    }

    bool notify(QObject *receiver, QEvent *event) override
    {
        if (!m_interactionProbeActive)
            return QGuiApplication::notify(receiver, event);
        const bool measure = m_notifyDepth == 0;
        const qint64 startedNs = measure
            ? YanamiPerformance::PerformanceTrace::monotonicNanoseconds()
            : 0;
        ++m_notifyDepth;
        const bool delivered = QGuiApplication::notify(receiver, event);
        --m_notifyDepth;
        if (measure) {
            const qint64 elapsedNs =
                YanamiPerformance::PerformanceTrace::monotonicNanoseconds()
                - startedNs;
            ++m_dispatchStats.dispatchCount;
            if (elapsedNs > 50'000'000)
                ++m_dispatchStats.longTaskOver50Count;
            m_dispatchStats.maxDispatchMs = std::max(
                m_dispatchStats.maxDispatchMs,
                elapsedNs / 1'000'000.0);
        }
        return delivered;
    }

private:
    DispatchStats m_dispatchStats;
    int m_notifyDepth = 0;
    bool m_interactionProbeActive = false;
};

class InputToFrameProbe final : public QObject
{
public:
    InputToFrameProbe(
        PerformanceGuiApplication &application,
        QQuickWindow &window,
        bool exitApplicationWhenFinished)
        : QObject(&window)
        , m_application(application)
        , m_window(window)
        , m_syntheticTarget(this)
        , m_exitApplicationWhenFinished(exitApplicationWhenFinished)
    {
        m_application.installEventFilter(this);
        QObject::connect(
            &m_window,
            &QQuickWindow::frameSwapped,
            this,
            [this] { publishPendingInputs(); });
    }

    ~InputToFrameProbe() override
    {
        m_application.removeEventFilter(this);
    }

    void startSyntheticProbe()
    {
        if (m_syntheticProbeActive)
            return;
        m_syntheticProbeActive = true;
        m_syntheticSubmitted = 0;
        m_syntheticPresented = 0;
        m_syntheticDropped = 0;
        m_syntheticCompletionScheduled = false;
        InputModalityService &inputModality =
            InputModalityService::instance();
        m_inputModalityBefore = static_cast<int>(inputModality.modality());
        m_inputModalityChanges = 0;
        m_inputModalityConnection = QObject::connect(
            &inputModality,
            &InputModalityService::modalityChanged,
            this,
            [this] { ++m_inputModalityChanges; });
        m_application.beginInteractionProbeWindow();
        QCoreApplication::postEvent(
            &m_syntheticTarget,
            new QEvent(syntheticInteractionProbeEventType()));
        m_window.update();
        QTimer::singleShot(2'000, this, [this] { finishSyntheticProbe(); });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        QString kind;
        const bool synthetic = watched == &m_syntheticTarget
            && event->type() == syntheticInteractionProbeEventType();
        if (synthetic) {
            kind = QStringLiteral("synthetic_hook");
        } else {
            switch (event->type()) {
            case QEvent::KeyPress:
                kind = QStringLiteral("key");
                break;
            case QEvent::MouseButtonPress:
                kind = QStringLiteral("pointer");
                break;
            case QEvent::Wheel:
                kind = QStringLiteral("wheel");
                break;
            case QEvent::TouchBegin:
                kind = QStringLiteral("touch");
                break;
            case QEvent::InputMethod: {
                const auto *inputMethod = static_cast<QInputMethodEvent *>(event);
                if (inputMethod->commitString().isEmpty())
                    break;
                kind = QStringLiteral("ime_commit");
                break;
            }
            default:
                break;
            }
        }
        if (!kind.isEmpty()) {
            if (m_pending.size() == 64) {
                if (m_pending.constFirst().synthetic)
                    ++m_syntheticDropped;
                m_pending.removeFirst();
            }
            const quint64 generation = ++m_generation;
            m_pending.push_back({
                generation,
                YanamiPerformance::PerformanceTrace::monotonicNanoseconds(),
                kind,
                synthetic,
            });
            if (synthetic) {
                ++m_syntheticSubmitted;
                YanamiPerformance::PerformanceTrace::mark(
                    QStringLiteral("interaction_input_received"),
                    {
                        {QStringLiteral("generation"), generation},
                        {QStringLiteral("inputKind"), kind},
                        {QStringLiteral("synthetic"), true},
                    });
                // This registered user event is visible only to this opt-in
                // trace filter. Product input filters and QML never classify
                // it as pointer, keyboard, touch, wheel, or IME input.
                m_window.update();
            }
        }
        return false;
    }

private:
    struct PendingInput {
        quint64 generation = 0;
        qint64 startedNs = 0;
        QString kind;
        bool synthetic = false;
    };

    void publishPendingInputs()
    {
        if (m_pending.isEmpty())
            return;
        const qint64 presentedNs =
            YanamiPerformance::PerformanceTrace::monotonicNanoseconds();
        const QVector<PendingInput> pending = std::exchange(m_pending, {});
        bool syntheticPresented = false;
        for (const PendingInput &input : pending) {
            YanamiPerformance::PerformanceTrace::mark(
                QStringLiteral("interaction_next_frame"),
                {
                    {QStringLiteral("generation"), input.generation},
                    {QStringLiteral("inputKind"), input.kind},
                    {QStringLiteral("latencyMs"),
                     (presentedNs - input.startedNs) / 1'000'000.0},
                    {QStringLiteral("synthetic"), input.synthetic},
                });
            if (input.synthetic) {
                ++m_syntheticPresented;
                syntheticPresented = true;
            }
        }
        if (syntheticPresented && !m_syntheticCompletionScheduled) {
            m_syntheticCompletionScheduled = true;
            QTimer::singleShot(0, this, [this] { finishSyntheticProbe(); });
        }
    }

    void finishSyntheticProbe()
    {
        if (!m_syntheticProbeActive)
            return;
        m_syntheticProbeActive = false;
        const PerformanceGuiApplication::DispatchStats dispatch =
            m_application.endInteractionProbeWindow();
        InputModalityService &inputModality =
            InputModalityService::instance();
        const int inputModalityAfter =
            static_cast<int>(inputModality.modality());
        QObject::disconnect(m_inputModalityConnection);
        const qsizetype syntheticPending = std::count_if(
            m_pending.cbegin(),
            m_pending.cend(),
            [](const PendingInput &input) { return input.synthetic; });
        YanamiPerformance::PerformanceTrace::mark(
            QStringLiteral("interaction_probe_complete"),
            {
                {QStringLiteral("syntheticSubmitted"), m_syntheticSubmitted},
                {QStringLiteral("syntheticPresented"), m_syntheticPresented},
                {QStringLiteral("syntheticDropped"), m_syntheticDropped},
                {QStringLiteral("syntheticPending"), syntheticPending},
                {QStringLiteral("dispatchCount"), dispatch.dispatchCount},
                {QStringLiteral("longTaskOver50Count"),
                 dispatch.longTaskOver50Count},
                {QStringLiteral("maxDispatchMs"), dispatch.maxDispatchMs},
                {QStringLiteral("qpaPlatform"),
                 QGuiApplication::platformName()},
                {QStringLiteral("quickBackendEnvironment"),
                 QString::fromLocal8Bit(qgetenv("QT_QUICK_BACKEND"))},
                {QStringLiteral("rendererApi"),
                 static_cast<int>(
                     m_window.rendererInterface()->graphicsApi())},
                {QStringLiteral("windowExposed"), m_window.isExposed()},
                {QStringLiteral("inputModalityBefore"),
                 m_inputModalityBefore},
                {QStringLiteral("inputModalityAfter"),
                 inputModalityAfter},
                {QStringLiteral("inputModalityChanges"),
                 m_inputModalityChanges},
            });
        YanamiPerformance::PerformanceTrace::flush();
        if (m_exitApplicationWhenFinished)
            QCoreApplication::exit(EXIT_SUCCESS);
    }

    PerformanceGuiApplication &m_application;
    QQuickWindow &m_window;
    QObject m_syntheticTarget;
    QVector<PendingInput> m_pending;
    quint64 m_generation = 0;
    quint64 m_syntheticSubmitted = 0;
    quint64 m_syntheticPresented = 0;
    quint64 m_syntheticDropped = 0;
    bool m_syntheticProbeActive = false;
    bool m_syntheticCompletionScheduled = false;
    bool m_exitApplicationWhenFinished = false;
    int m_inputModalityBefore = -1;
    int m_inputModalityChanges = 0;
    QMetaObject::Connection m_inputModalityConnection;
};

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

class PerformanceTraceGuard final
{
public:
    PerformanceTraceGuard() = default;

    ~PerformanceTraceGuard()
    {
        YanamiPerformance::PerformanceTrace::shutdown();
    }

    PerformanceTraceGuard(const PerformanceTraceGuard &) = delete;
    PerformanceTraceGuard &operator=(const PerformanceTraceGuard &) = delete;
};
}

int main(int argc, char *argv[])
{
    YanamiPerformance::PerformanceTrace::initialize(argc, argv);
    PerformanceTraceGuard performanceTraceGuard;
    YanamiPerformance::PerformanceTrace::mark(QStringLiteral("main_entered"));
    QGuiApplication::setApplicationName(QStringLiteral("Yanami"));
    QGuiApplication::setApplicationVersion(QStringLiteral(YANAMI_VERSION));
    QGuiApplication::setOrganizationName(QStringLiteral("Yanami"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("yanami.local"));

    // The launcher creates its private handoff directory before the desktop
    // starts. Freeze the trusted OS temp roots before isolated-profile setup
    // intentionally redirects TEMP/TMPDIR for the rest of this process.
    const QStringList bootstrapHandoffTrustedTemporaryRoots =
        bootstrapHandoffTemporaryRoots();

    const ApplicationPaths::ConfigurationResult profileConfiguration =
        ApplicationPaths::configureFromEnvironment();
    if (!profileConfiguration.succeeded) {
        fprintf(
            stderr,
            "Yanami isolated profile configuration failed: %s\n",
            profileConfiguration.error.toUtf8().constData());
        return EXIT_FAILURE;
    }

    // libmpv's render API is OpenGL. Keeping Qt on the same graphics API avoids
    // cross-API copies and lets controls and video share one scene graph.
    // Yanami imports the Basic controls explicitly; pin the global style too
    // so QtQuick.Dialogs uses the matching Basic fallback instead of loading a
    // platform-dependent style module that the production package omits.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickWindow::setDefaultAlphaBuffer(false);

    PerformanceGuiApplication app(argc, argv);
    YanamiPerformance::PerformanceTrace::mark(QStringLiteral("qt_app_ready"));
    const bool runtimeSmokeTest = app.arguments().contains(
        QStringLiteral("--runtime-smoke-test"));
    const bool mpvRuntimeSmokeTest = app.arguments().contains(
        QStringLiteral("--mpv-runtime-smoke-test"));
    const bool performanceRuntimeProbe = app.arguments().contains(
        QStringLiteral("--performance-runtime-probe"));
    const bool performanceRuntimeAutoExit = app.arguments().contains(
        QStringLiteral("--performance-runtime-auto-exit"));
    if (mpvRuntimeSmokeTest)
        return runMpvRuntimeSmokeTest();
    const BootstrapHandoffRequest bootstrapHandoff =
        bootstrapHandoffFromArguments(
            app.arguments(), bootstrapHandoffTrustedTemporaryRoots);
#ifdef Q_OS_WIN
    const bool explicitDirectDesktop = app.arguments().contains(
        QString::fromLatin1(DesktopEntryGuard::DirectDesktopOption))
        || qEnvironmentVariableIntValue(
            DesktopEntryGuard::DirectDesktopEnvironment) == 1;
    const bool diagnosticOrPerformanceMode = runtimeSmokeTest
        || mpvRuntimeSmokeTest
        || performanceRuntimeProbe
        || performanceRuntimeAutoExit
        || YanamiPerformance::PerformanceTrace::enabled();
    if (DesktopEntryGuard::routeForLaunch({
            bootstrapHandoff.usable(),
            explicitDirectDesktop,
            diagnosticOrPerformanceMode,
        }) == DesktopEntryGuard::Route::RedirectToBootstrap) {
        const DesktopEntryGuard::RedirectResult redirect =
            DesktopEntryGuard::redirectToSiblingBootstrap(app.arguments());
        if (redirect.status == DesktopEntryGuard::RedirectStatus::Started)
            return EXIT_SUCCESS;

        // A direct build tree or partially copied developer artifact may not
        // contain the user-facing launcher. Keep the internal executable
        // usable as a recovery path instead of turning packaging damage into
        // a hard startup failure.
        fprintf(
            stderr,
            "Yanami launcher redirect unavailable (%s); continuing the "
            "internal desktop entry.\n",
            redirect.detail.toUtf8().constData());
    }
#endif
    // Install the saved translator before any backend or view-model constructor
    // can materialize a user-visible status string.
    LocaleController localeController(nullptr);
    const bool runtimeLoggerInstalled = RuntimeLogger::install();
    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("logger_ready"),
        {{QStringLiteral("installed"), runtimeLoggerInstalled}});
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
    if (bootstrapHandoff.supplied && !bootstrapHandoff.usable()) {
        qCWarning(applicationLog).noquote()
            << "bootstrap_handoff_rejected"
            << "reason=" << bootstrapHandoff.rejectionReason;
        YanamiPerformance::PerformanceTrace::mark(
            QStringLiteral("bootstrap_handoff_rejected"),
            {{QStringLiteral("reason"),
              bootstrapHandoff.rejectionReason}});
    }
    app.setWindowIcon(QIcon(QStringLiteral(
        ":/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png")));
    qmlRegisterType<MpvVideoItem>("Yanami.Native", 1, 0, "MpvVideoItem");
    qmlRegisterType<MediaQueryProxyModel>(
        "Yanami.Native", 1, 0, "MediaQueryProxyModel");
    qmlRegisterType<MediaSearchModel>(
        "Yanami.Native", 1, 0, "MediaSearchModel");
    qmlRegisterType<AsyncResourceState>(
        "Yanami.Native", 1, 0, "AsyncResourceState");
    qmlRegisterUncreatableType<AsyncOperationState>(
        "Yanami.Native", 1, 0, "AsyncOperationState",
        QStringLiteral("AsyncOperationState instances are owned by feature view models."));
    DesktopBackendServices backendServices(
        ApplicationPaths::dataRoot(),
        ApplicationPaths::isolated());
    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("backend_services_ready"));
    ApplicationViewModel applicationViewModel(backendServices.portSet());
    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("view_models_ready"));
    WindowController windowController;
    QQmlApplicationEngine engine;
    // windeployqt stages Qt's runtime QML modules beside the executable.
    // Add that location explicitly so a build-tree qt.conf regenerated by
    // Qt CMake targets cannot hide the deployed modules from a direct launch.
    const QString applicationQmlPath =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("qml"));
    if (QDir(applicationQmlPath).exists())
        engine.addImportPath(applicationQmlPath);
    engine.addImageProvider(
        QStringLiteral("yanami"),
        new AsyncImageProvider(ApplicationPaths::cacheRoot()));
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
            MediaStore *store = applicationViewModel.home()->mediaStore();
            QVariantList items;
            for (const QVariant &sectionValue :
                 store->queryItems(QStringLiteral("latestSections"))) {
                const QString scopeId = sectionValue.toMap()
                    .value(QStringLiteral("id")).toString();
                items.append(store->queryItems(
                    QStringLiteral("latest"), scopeId));
            }
            items.append(store->queryItems(QStringLiteral("resume")));
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
    engine.rootContext()->setContextProperty(
        QStringLiteral("bootstrapHandoffRequested"),
        bootstrapHandoff.usable());
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);
    engine.loadFromModule("Yanami", "Main");
    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("qml_root_ready"),
        {{QStringLiteral("rootObjectCount"), engine.rootObjects().size()}});

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
        if (auto *window = qobject_cast<QWindow *>(root)) {
#ifdef Q_OS_WIN
            if (bootstrapHandoff.usable())
                centerBootstrapWindowOnPointerScreen(window);
#endif
            windowController.configureWindow(window);
        }
        if (auto *quickWindow = qobject_cast<QQuickWindow *>(root))
            applicationViewModel.upscaling()->observeWindow(quickWindow);
        if (YanamiPerformance::PerformanceTrace::enabled()) {
            if (auto *quickWindow = qobject_cast<QQuickWindow *>(root)) {
                auto *inputProbe = new InputToFrameProbe(
                    app,
                    *quickWindow,
                    performanceRuntimeAutoExit);
                struct StartupFrameState {
                    qint64 firstFrameNs = 0;
                    qint64 lastFrameNs = 0;
                    QVector<double> frameIntervalsMs;
                    int swappedFrameCount = 0;
                    int firstShellFrame = 1;
                    bool cachedContentAvailable = false;
                    bool cachedContentMarked = false;
                    bool exposureMarked = false;
                    bool settledMarked = false;
                };
                const auto startupFrames = std::make_shared<StartupFrameState>();
                startupFrames->firstShellFrame =
                    bootstrapHandoff.usable() ? 2 : 1;
                startupFrames->cachedContentAvailable =
                    hasCachedHomeContent(applicationViewModel);
                const auto publishStartupSettled =
                    [startupFrames,
                     inputProbe,
                     performanceRuntimeProbe,
                     performanceRuntimeAutoExit] {
                    if (startupFrames->settledMarked)
                        return;
                    startupFrames->settledMarked = true;
                    const QVector<double> &intervals =
                        startupFrames->frameIntervalsMs;
                    const auto overThreshold = [&intervals](double threshold) {
                        return std::count_if(
                            intervals.cbegin(),
                            intervals.cend(),
                            [threshold](double value) { return value > threshold; });
                    };
                    YanamiPerformance::PerformanceTrace::mark(
                        QStringLiteral("startup_settled"),
                        {
                            {QStringLiteral("frameCount"), intervals.size()},
                            {QStringLiteral("frameP95Ms"), percentile(intervals, 0.95)},
                            {QStringLiteral("frameP99Ms"), percentile(intervals, 0.99)},
                            {QStringLiteral("frameMaxMs"), percentile(intervals, 1.0)},
                            {QStringLiteral("framesOver33Ms"), overThreshold(33.34)},
                            {QStringLiteral("framesOver50Ms"), overThreshold(50.0)},
                        });
                    startupFrames->frameIntervalsMs.clear();
                    startupFrames->frameIntervalsMs.squeeze();
                    // The performance runner polls this milestone while the
                    // process remains alive. This is a trace persistence
                    // boundary, not evidence of an external Present event.
                    YanamiPerformance::PerformanceTrace::flush();
                    if (performanceRuntimeProbe) {
                        QTimer::singleShot(
                            0,
                            inputProbe,
                            [inputProbe] { inputProbe->startSyntheticProbe(); });
                    } else if (performanceRuntimeAutoExit) {
                        QCoreApplication::exit(EXIT_SUCCESS);
                    }
                };

                auto *exposureTimer = new QTimer(quickWindow);
                exposureTimer->setInterval(5);
                exposureTimer->setTimerType(Qt::PreciseTimer);
                QObject::connect(
                    exposureTimer,
                    &QTimer::timeout,
                    quickWindow,
                    [quickWindow, exposureTimer, startupFrames] {
                        if (!quickWindow->isExposed())
                            return;
                        if (!startupFrames->exposureMarked) {
                            startupFrames->exposureMarked = true;
                            YanamiPerformance::PerformanceTrace::mark(
                                QStringLiteral("window_exposed"));
                        }
                        exposureTimer->stop();
                        exposureTimer->deleteLater();
                    });
                exposureTimer->start();

                QObject::connect(
                    applicationViewModel.home()->mediaStore(),
                    &MediaStore::queryChanged,
                    quickWindow,
                    [startupFrames, &applicationViewModel](const QString &, const QString &) {
                        startupFrames->cachedContentAvailable =
                            hasCachedHomeContent(applicationViewModel);
                    });
                QObject::connect(
                    quickWindow,
                    &QQuickWindow::frameSwapped,
                    quickWindow,
                    [startupFrames, quickWindow, publishStartupSettled] {
                        if (startupFrames->settledMarked)
                            return;
                        const qint64 now =
                            YanamiPerformance::PerformanceTrace::monotonicNanoseconds();
                        ++startupFrames->swappedFrameCount;
                        // A first swap can race the 5 ms exposure poll. Keep the
                        // trace causally ordered without treating this internal
                        // Qt signal as external Present evidence.
                        if (!startupFrames->exposureMarked
                            && quickWindow->isExposed()) {
                            startupFrames->exposureMarked = true;
                            YanamiPerformance::PerformanceTrace::mark(
                                QStringLiteral("window_exposed"));
                        }
                        if (startupFrames->firstFrameNs == 0
                            && startupFrames->swappedFrameCount
                                >= startupFrames->firstShellFrame) {
                            startupFrames->firstFrameNs = now;
                            YanamiPerformance::PerformanceTrace::mark(
                                QStringLiteral("first_shell_present"));
                            QTimer::singleShot(
                                1'000, quickWindow, publishStartupSettled);
                        }
                        if (!startupFrames->cachedContentMarked
                            && startupFrames->firstFrameNs != 0
                            && startupFrames->cachedContentAvailable) {
                            startupFrames->cachedContentMarked = true;
                            YanamiPerformance::PerformanceTrace::mark(
                                QStringLiteral("first_cached_content_present"));
                        }
                        if (startupFrames->firstFrameNs != 0) {
                            if (startupFrames->lastFrameNs != 0) {
                                startupFrames->frameIntervalsMs.push_back(
                                    (now - startupFrames->lastFrameNs)
                                    / 1'000'000.0);
                            }
                            startupFrames->lastFrameNs = now;
                        }
                    });
                QTimer::singleShot(0, quickWindow, [] {
                    YanamiPerformance::PerformanceTrace::mark(
                        QStringLiteral("event_loop_ready"));
                });
            }
        }
        if (bootstrapHandoff.usable()) {
            if (auto *quickWindow = qobject_cast<QQuickWindow *>(root)) {
                struct BootstrapHandoffFrameState {
                    int swappedFrames = 0;
                    bool publicationAttempted = false;
                    QMetaObject::Connection connection;
                };
                const auto handoffFrames =
                    std::make_shared<BootstrapHandoffFrameState>();
                const QString readyFilePath = bootstrapHandoff.readyFilePath;
                const BootstrapReadySignal readySignal =
                    bootstrapHandoff.readySignal;
                handoffFrames->connection = QObject::connect(
                    quickWindow,
                    &QQuickWindow::frameSwapped,
                    quickWindow,
                    [handoffFrames, quickWindow, root, readyFilePath,
                     readySignal] {
                        ++handoffFrames->swappedFrames;
                        if (handoffFrames->swappedFrames == 1) {
                            // The first swap only proves that the transition
                            // scene exists. Reveal that scene and request one
                            // more frame before notifying the launcher.
                            if (!root->setProperty(
                                    "bootstrapHandoffPending", false)) {
                                quickWindow->setOpacity(1.0);
                            }
                            quickWindow->requestUpdate();
                            return;
                        }
                        if (handoffFrames->swappedFrames < 2
                            || handoffFrames->publicationAttempted) {
                            return;
                        }
                        handoffFrames->publicationAttempted = true;
                        QObject::disconnect(handoffFrames->connection);

                        // This second swap contains the visible QML brand
                        // transition. The launcher may retire its own splash
                        // only after the atomic ready file appears.
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("desktop_ready"),
                            {{QStringLiteral("visibleTransitionFrame"), true},
                             {QStringLiteral("swappedFrameCount"),
                              handoffFrames->swappedFrames}});
                        QString errorCode;
                        const bool readyFileCommitted =
                            publishBootstrapReadyFile(
                                readyFilePath, &errorCode);
                        QString readySignalError;
                        const bool nativeReadySignaled = readyFileCommitted
                            && signalBootstrapReady(
                                readySignal, &readySignalError);
                        if (!readyFileCommitted) {
                            readySignalError = QStringLiteral(
                                "ready_file_not_committed");
                        }
                        root->setProperty("bootstrapHandoffReady", true);
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("desktop_ready_file_committed"),
                            {{QStringLiteral("readyFileCommitted"),
                              readyFileCommitted},
                             {QStringLiteral("errorCode"), errorCode},
                             {QStringLiteral("nativeReadySignaled"),
                              nativeReadySignaled},
                             {QStringLiteral("readySignalError"),
                              readySignalError}});
                        YanamiPerformance::PerformanceTrace::flush();
                        if (readyFileCommitted) {
                            qCInfo(applicationLog)
                                << "bootstrap_desktop_ready_published";
                        } else {
                            qCWarning(applicationLog).noquote()
                                << "bootstrap_desktop_ready_publish_failed"
                                << "reason=" << errorCode;
                        }
                    });
            } else {
                // A malformed root must never leave the desktop transparent,
                // even though the launcher will independently time out or
                // observe process exit when no ready file is published.
                root->setProperty("bootstrapHandoffPending", false);
                root->setProperty("bootstrapHandoffReady", true);
                qCWarning(applicationLog)
                    << "bootstrap_handoff_root_is_not_quick_window";
            }
        }
        if (auto *quickWindow = qobject_cast<QQuickWindow *>(root)) {
            // SDL/XInput discovery is intentionally outside the construction
            // path of the QML singleton. Wait for the first completed shell
            // frame, then queue initialization so no controller work runs in
            // the frameSwapped delivery itself.
            QObject::connect(
                quickWindow,
                &QQuickWindow::frameSwapped,
                quickWindow,
                [quickWindow] {
                    QTimer::singleShot(0, quickWindow, [] {
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("controller_navigation_init_begin"));
                        InputModalityService &inputModality =
                            InputModalityService::instance();
                        inputModality.initializeControllerNavigation();
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("controller_navigation_init_end"),
                            {
                                {QStringLiteral("backend"),
                                 inputModality.controllerBackend()},
                                {QStringLiteral("connectedDeviceCount"),
                                 inputModality.connectedDevices().size()},
                            });
                    });
                },
                Qt::SingleShotConnection);
        }
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
    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("event_loop_finished"),
        {{QStringLiteral("exitCode"), exitCode}});
    YanamiPerformance::PerformanceTrace::shutdown();
    qCInfo(applicationLog).noquote()
        << "application_event_loop_finished"
        << "exitCode=" << exitCode;
    return exitCode;
}
