#include "BackendInfrastructure.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>

RuntimeHost::RuntimeHost(QObject *parent)
    : QObject(parent)
    , m_runtime(std::make_unique<RustBridgeRuntime>())
{
}

RuntimeHost::~RuntimeHost()
{
    shutdown();
}

RuntimeInitializationResult RuntimeHost::initialize(
    const QString &applicationDirectory,
    const QString &dataDirectory)
{
    if (ready()) {
        return {
            true,
            {},
            {},
            m_dataDirectory,
            m_cacheDirectory,
        };
    }

    const QString resolvedApplicationDirectory = applicationDirectory.isEmpty()
        ? QCoreApplication::applicationDirPath()
        : QDir::cleanPath(applicationDirectory);
    const QString bridgePath = QDir(resolvedApplicationDirectory).filePath(
        platformBridgeFileName());
    QString loadError;
    if (!m_runtime->load(bridgePath, &loadError)) {
        return {
            false,
            QStringLiteral("bridge_load"),
            loadError,
            {},
            {},
        };
    }

    const QString resolvedDataDirectory = dataDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QDir::cleanPath(dataDirectory);
    if (resolvedDataDirectory.isEmpty()
        || !QDir().mkpath(resolvedDataDirectory)) {
        m_runtime->close();
        return {
            false,
            QStringLiteral("storage"),
            QStringLiteral("Unable to create the application data folder."),
            {},
            {},
        };
    }
    const QString resolvedCacheDirectory = QDir(resolvedDataDirectory)
        .filePath(QStringLiteral("cache"));
    if (!QDir().mkpath(resolvedCacheDirectory)) {
        m_runtime->close();
        return {
            false,
            QStringLiteral("storage"),
            QStringLiteral("Unable to create the application cache folder."),
            {},
            {},
        };
    }

    const YanamiStatusResult openResult =
        m_runtime->open(resolvedDataDirectory);
    if (openResult.value < 0) {
        m_runtime->close();
        return {
            false,
            openResult.errorCode,
            openResult.error,
            {},
            {},
        };
    }

    m_dataDirectory = resolvedDataDirectory;
    m_cacheDirectory = resolvedCacheDirectory;
    emit readyChanged();
    return {
        true,
        {},
        {},
        m_dataDirectory,
        m_cacheDirectory,
    };
}

void RuntimeHost::shutdown()
{
    if (!m_runtime)
        return;
    const bool wasReady = ready();
    m_runtime->cancelAll();
    m_runtime->close();
    m_dataDirectory.clear();
    m_cacheDirectory.clear();
    if (wasReady)
        emit readyChanged();
}

bool RuntimeHost::ready() const
{
    return m_runtime && m_runtime->ready();
}

QString RuntimeHost::platformBridgeFileName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("yanami_desktop_bridge.dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("libyanami_desktop_bridge.dylib");
#else
    return QStringLiteral("libyanami_desktop_bridge.so");
#endif
}

WorkerPools::WorkerPools()
{
    configure(m_sessionControl, 1);
    configure(m_danmakuControl, 1);
    configure(m_catalog, 2);
    configure(m_mediaRead, 4);
    configure(m_mediaMutation, 2);
    configure(m_playbackPrepare, 1);
    configure(m_playbackReport, 1);
}

WorkerPools::~WorkerPools()
{
    drain();
}

void WorkerPools::configure(QThreadPool &pool, int maximumThreads)
{
    pool.setMaxThreadCount(maximumThreads);
    pool.setExpiryTimeout(30000);
}

void WorkerPools::drain()
{
    m_sessionControl.waitForDone();
    m_danmakuControl.waitForDone();
    m_catalog.waitForDone();
    m_mediaRead.waitForDone();
    m_mediaMutation.waitForDone();
    m_playbackPrepare.waitForDone();
    m_playbackReport.waitForDone();
}

ApplicationStatusService::ApplicationStatusService(
    RuntimeHost &runtimeHost,
    QObject *parent)
    : ApplicationStatusPort(parent)
    , m_runtimeHost(runtimeHost)
{
    connect(&runtimeHost, &RuntimeHost::readyChanged,
        this, &ApplicationStatusPort::stateChanged);
}

bool ApplicationStatusService::ready() const
{
    return m_runtimeHost.ready();
}

void ApplicationStatusService::clear()
{
    publishStatus({}, false);
}

void ApplicationStatusService::publishStatus(
    const QString &message,
    bool error)
{
    if (m_message == message && m_error == error)
        return;
    m_message = message;
    m_error = error;
    emit stateChanged();
}

QString ApplicationStatusService::userFacingBackendError(
    const QString &code,
    const QString &diagnosticMessage) const
{
    const QString normalizedCode = code.trimmed().toLower();
    if (normalizedCode == QStringLiteral("cancelled"))
        return tr("The operation was canceled.");
    if (normalizedCode == QStringLiteral("invalid_input")) {
        return tr("The request was invalid. Check the entered values and try again.");
    }
    if (normalizedCode == QStringLiteral("not_connected"))
        return tr("This action requires an active Emby connection.");
    if (normalizedCode == QStringLiteral("not_found"))
        return tr("The requested media item could not be found.");
    if (normalizedCode == QStringLiteral("permission_denied")) {
        return tr("This account does not have permission to perform that action.");
    }
    if (normalizedCode == QStringLiteral("unsupported")) {
        return tr("This server operation or media format is not supported yet.");
    }
    if (normalizedCode == QStringLiteral("storage"))
        return tr("Yanami could not read or save its local data.");
    if (normalizedCode == QStringLiteral("credentials"))
        return tr("The saved credentials were rejected. Sign in again.");
    if (normalizedCode == QStringLiteral("network")) {
        return tr("Yanami could not reach the Emby server. Check the server "
                  "address and network connection.");
    }
    if (normalizedCode == QStringLiteral("bridge_load")
        || normalizedCode == QStringLiteral("bridge_unavailable")
        || normalizedCode == QStringLiteral("bridge_protocol")) {
        return tr("Yanami could not start its application backend.");
    }
    if (!diagnosticMessage.isEmpty()) {
        qWarning().noquote()
            << "backend_error"
            << "code=" << normalizedCode
            << "message=" << diagnosticMessage;
    }
    return tr("The operation could not be completed. Please try again.");
}

QString ApplicationStatusService::userFacingDanmakuError(
    const QString &code,
    const QString &diagnosticMessage) const
{
    const QString normalizedCode = code.trimmed().toLower();
    if (normalizedCode == QStringLiteral("cancelled"))
        return tr("The danmaku operation was canceled.");
    if (normalizedCode == QStringLiteral("invalid_input")) {
        return tr("The danmaku request was invalid. Check the entered values and try again.");
    }
    if (normalizedCode == QStringLiteral("not_connected")) {
        return tr("Danmaku cannot be loaded because there is no active playback session.");
    }
    if (normalizedCode == QStringLiteral("not_found"))
        return tr("The requested danmaku could not be found.");
    if (normalizedCode == QStringLiteral("permission_denied"))
        return tr("The danmaku service rejected this request.");
    if (normalizedCode == QStringLiteral("unsupported"))
        return tr("The current danmaku request is not supported.");
    if (normalizedCode == QStringLiteral("storage"))
        return tr("Yanami could not read or save its local danmaku data.");
    if (normalizedCode == QStringLiteral("credentials")) {
        return tr("The danmaku service credentials were rejected. Check the danmaku settings.");
    }
    if (normalizedCode == QStringLiteral("network")) {
        return tr("Could not connect to the danmaku service. Check the network connection.");
    }
    if (!diagnosticMessage.isEmpty()) {
        qWarning().noquote()
            << "backend_error"
            << "domain=danmaku"
            << "code=" << normalizedCode
            << "message=" << diagnosticMessage;
    }
    return tr("The danmaku operation could not be completed. Please try again.");
}
