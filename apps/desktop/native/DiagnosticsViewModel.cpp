#include "DiagnosticsViewModel.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrentRun>

namespace {

Q_LOGGING_CATEGORY(diagnosticsLog, "yanami.diagnostics")

} // namespace

DiagnosticsViewModel::DiagnosticsViewModel(QObject *parent)
    : QObject(parent)
{
    connect(
        &m_exportWatcher,
        &QFutureWatcher<RuntimeLogger::LogExportResult>::finished,
        this,
        &DiagnosticsViewModel::finishExport);
}

DiagnosticsViewModel::~DiagnosticsViewModel()
{
    if (m_exportWatcher.isRunning())
        m_exportWatcher.waitForFinished();
}

void DiagnosticsViewModel::exportLogs()
{
    if (m_exporting)
        return;

    const QString destinationPath = nextExportPath();
    if (destinationPath.isEmpty()) {
        setFailure(tr("No writable folder is available for diagnostics."));
        return;
    }

    m_exporting = true;
    m_lastExportPath.clear();
    m_errorMessage.clear();
    emit stateChanged();
    qCInfo(diagnosticsLog).noquote()
        << "diagnostics_export_started"
        << "fileName=" << QFileInfo(destinationPath).fileName();

    m_exportWatcher.setFuture(QtConcurrent::run(
        [destinationPath] {
            return RuntimeLogger::exportRecentLogs(destinationPath);
        }));
}

void DiagnosticsViewModel::openExportFolder()
{
    if (m_lastExportPath.isEmpty()
        || !QFileInfo::exists(m_lastExportPath)) {
        setFailure(tr("The exported diagnostics file is no longer available."));
        return;
    }

    const QString directory = QFileInfo(m_lastExportPath).absolutePath();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
        setFailure(tr("Could not open the diagnostics folder."));
        return;
    }
    if (!m_errorMessage.isEmpty()) {
        m_errorMessage.clear();
        emit stateChanged();
    }
}

QString DiagnosticsViewModel::nextExportPath() const
{
    const QString logDirectory = QFileInfo(
        RuntimeLogger::currentLogPath()).absolutePath();
    const QString canonicalLogDirectory =
        QDir(logDirectory).canonicalPath();
    const QList<QStandardPaths::StandardLocation> locations = {
        QStandardPaths::DownloadLocation,
        QStandardPaths::DocumentsLocation,
        QStandardPaths::TempLocation,
    };
    QString destinationDirectory;
    for (const auto location : locations) {
        const QString candidate =
            QStandardPaths::writableLocation(location).trimmed();
        if (candidate.isEmpty())
            continue;
        QDir directory(candidate);
        if (!canonicalLogDirectory.isEmpty()
            && directory.canonicalPath() == canonicalLogDirectory) {
            continue;
        }
        if (directory.exists() || directory.mkpath(QStringLiteral("."))) {
            destinationDirectory = directory.absolutePath();
            break;
        }
    }
    if (destinationDirectory.isEmpty())
        return {};

    const QString stem = QStringLiteral("Yanami-diagnostics-")
        + QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd-HHmmss-zzz"))
        + QStringLiteral("-p")
        + QString::number(QCoreApplication::applicationPid());
    QDir directory(destinationDirectory);
    QString path = directory.absoluteFilePath(stem + QStringLiteral(".log"));
    for (int suffix = 2; QFileInfo::exists(path) && suffix < 10'000; ++suffix) {
        path = directory.absoluteFilePath(
            stem + u'-' + QString::number(suffix) + QStringLiteral(".log"));
    }
    return QFileInfo::exists(path) ? QString{} : path;
}

QString DiagnosticsViewModel::exportErrorMessage(
    RuntimeLogger::LogExportError error) const
{
    using RuntimeLogger::LogExportError;
    switch (error) {
    case LogExportError::LoggerUnavailable:
        return tr("Runtime logging is unavailable.");
    case LogExportError::InvalidDestination:
        return tr("The diagnostics destination is not valid.");
    case LogExportError::NoLogsAvailable:
        return tr("No runtime logs are available to export.");
    case LogExportError::DestinationUnavailable:
        return tr("Could not create the diagnostics file.");
    case LogExportError::ReadFailed:
        return tr("Could not read the runtime logs.");
    case LogExportError::WriteFailed:
        return tr("Could not write the diagnostics file.");
    case LogExportError::None:
        break;
    }
    return tr("Could not export diagnostics.");
}

void DiagnosticsViewModel::setFailure(const QString &message)
{
    m_exporting = false;
    m_errorMessage = message;
    emit stateChanged();
    emit exportFailed(message);
}

void DiagnosticsViewModel::finishExport()
{
    const RuntimeLogger::LogExportResult result = m_exportWatcher.result();
    m_exporting = false;
    if (!result.succeeded()) {
        qCWarning(diagnosticsLog).noquote()
            << "diagnostics_export_failed"
            << "error=" << static_cast<int>(result.error)
            << "detail=" << result.detail;
        setFailure(exportErrorMessage(result.error));
        return;
    }

    m_lastExportPath = result.destinationPath;
    m_errorMessage.clear();
    emit stateChanged();
    qCInfo(diagnosticsLog).noquote()
        << "diagnostics_export_succeeded"
        << "fileName=" << QFileInfo(result.destinationPath).fileName()
        << "sourceCount=" << result.exportedFileCount;
    emit exportSucceeded(result.destinationPath);
}
