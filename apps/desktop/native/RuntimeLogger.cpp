#include "RuntimeLogger.hpp"

#include "DevelopmentHooks.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageLogContext>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QtLogging>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#endif

namespace RuntimeLogger {
namespace {

constexpr qint64 maximumLogBytes = 10LL * 1024LL * 1024LL;
constexpr qsizetype maximumEntryBytes = 1024 * 1024;
constexpr int archiveRetentionDays = 14;
constexpr qsizetype maximumArchiveCount = 30;
constexpr qint64 rotationRetryMilliseconds = 60'000;

struct LoggerState
{
    QMutex mutex;
    QFile file;
    QString activePath;
    QDate activeDate;
    QtMessageHandler previousHandler = nullptr;
    quint64 rotationSequence = 0;
    bool installed = false;
    qint64 rotationRetryAfter = 0;
};

LoggerState &loggerState()
{
    // The Qt message handler can be invoked during static destruction. Keeping
    // this state alive until process exit avoids a destruction-order race.
    static auto *state = new LoggerState;
    return *state;
}

void writeToStderr(const QByteArray &message)
{
    std::fwrite(message.constData(), 1, static_cast<size_t>(message.size()), stderr);
    std::fwrite("\n", 1, 1, stderr);
    std::fflush(stderr);
}

QString quoteField(QString value)
{
    value.replace(u'\\', QStringLiteral("\\\\"));
    value.replace(u'\"', QStringLiteral("\\\""));
    value.replace(u'\r', QStringLiteral("\\r"));
    value.replace(u'\n', QStringLiteral("\\n"));
    value.replace(u'\t', QStringLiteral("\\t"));
    return u'\"' + value + u'\"';
}

QString escapeFieldContent(QString value)
{
    value.replace(u'\\', QStringLiteral("\\\\"));
    value.replace(u'\"', QStringLiteral("\\\""));
    value.replace(u'\r', QStringLiteral("\\r"));
    value.replace(u'\n', QStringLiteral("\\n"));
    value.replace(u'\t', QStringLiteral("\\t"));
    return value;
}

QString redactSensitiveData(QString message)
{
    static const QRegularExpression urlPattern(
        QStringLiteral(R"(\bhttps?://[^\s\"'<>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = urlPattern.globalMatch(message);
    QList<QPair<qsizetype, QPair<qsizetype, QString>>> replacements;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QUrl url(match.captured());
        QString replacement;
        if (url.isValid() && !url.host().isEmpty()) {
            QString host = url.host(QUrl::FullyEncoded);
            if (host.contains(u':') && !host.startsWith(u'['))
                host = u'[' + host + u']';
            replacement = url.scheme().toLower() + QStringLiteral("://") + host;
            if (url.port() >= 0)
                replacement += u':' + QString::number(url.port());
            replacement += QStringLiteral("/<redacted>");
        } else {
            replacement = QStringLiteral("https://<redacted>");
        }
        replacements.append({match.capturedStart(), {match.capturedLength(), replacement}});
    }
    for (auto iterator = replacements.crbegin(); iterator != replacements.crend(); ++iterator)
        message.replace(iterator->first, iterator->second.first, iterator->second.second);

    static const QRegularExpression namedSecretPattern(
        QStringLiteral(
            R"((\b[\"']?(?:x[-_]?emby[-_]?token|api[-_]?key|access[-_]?token|token|authorization)[\"']?\s*(?::|=)\s*[\"']?)(?:bearer\s+)?([^\s&,;\"'}\]]+))"),
        QRegularExpression::CaseInsensitiveOption);
    message.replace(namedSecretPattern, QStringLiteral("\\1<redacted>"));

    static const QRegularExpression bearerPattern(
        QStringLiteral(R"(\bbearer\s+[A-Za-z0-9._~+/=-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    message.replace(bearerPattern, QStringLiteral("Bearer <redacted>"));
    return message;
}

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO";
    case QtWarningMsg:
        return "WARNING";
    case QtCriticalMsg:
        return "CRITICAL";
    case QtFatalMsg:
        return "FATAL";
    }
    return "UNKNOWN";
}

QString resolvedLogPath()
{
    const QString overridePath =
        DevelopmentHooks::value(DevelopmentHooks::Variable::LogPath);
    QString requestedPath;
    if (!overridePath.isEmpty()) {
        requestedPath = QFileInfo(overridePath).absoluteFilePath();
    } else {
        const QString dataLocation =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (dataLocation.isEmpty())
            return {};
        requestedPath = QDir(dataLocation).absoluteFilePath(QStringLiteral("logs/yanami.log"));
    }

    const QFileInfo requestedFile(requestedPath);
    const QString suffix = requestedFile.completeSuffix();
    const QString pidSuffix = QStringLiteral("-p%1").arg(QCoreApplication::applicationPid())
        + (suffix.isEmpty() ? QString() : u'.' + suffix);
    return QDir(requestedFile.absolutePath()).absoluteFilePath(
        requestedFile.completeBaseName() + pidSuffix);
}

QString logFamilyPrefix(const QFileInfo &activeFile)
{
    QString baseName = activeFile.completeBaseName();
    static const QRegularExpression pidSuffix(QStringLiteral(R"(-p\d+$)"));
    baseName.remove(pidSuffix);
    return baseName;
}

QString logFamilySuffix(const QFileInfo &activeFile)
{
    const QString suffix = activeFile.completeSuffix();
    return suffix.isEmpty() ? QString() : u'.' + suffix;
}

enum class ManagedLogKind
{
    None,
    Active,
    Archive,
};

struct ManagedLog
{
    QFileInfo file;
    ManagedLogKind kind = ManagedLogKind::None;
    qint64 pid = 0;
};

ManagedLog managedLog(const QFileInfo &candidate, const QFileInfo &activeFile)
{
    ManagedLog result{candidate};
    if (!candidate.isFile())
        return result;

    const QString name = candidate.fileName();
    const QString suffix = QRegularExpression::escape(logFamilySuffix(activeFile));
    const QString family = QRegularExpression::escape(logFamilyPrefix(activeFile));
    const QRegularExpression activePattern(
        QStringLiteral("^%1-p(\\d+)%2$").arg(family, suffix));
    const QRegularExpression archivePattern(
        QStringLiteral("^%1-p(\\d+)\\.archive-[-0-9A-Za-z]+%2$").arg(family, suffix));
    QRegularExpressionMatch match = activePattern.match(name);
    if (match.hasMatch()) {
        result.kind = ManagedLogKind::Active;
        result.pid = match.captured(1).toLongLong();
        return result;
    }
    match = archivePattern.match(name);
    if (match.hasMatch()) {
        result.kind = ManagedLogKind::Archive;
        result.pid = match.captured(1).toLongLong();
    }
    return result;
}

bool processIsRunning(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    if (pid > std::numeric_limits<DWORD>::max())
        return false;
    const HANDLE process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(pid));
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD exitCode = 0;
    const bool running = GetExitCodeProcess(process, &exitCode) != FALSE
        && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno == EPERM;
#endif
}

void cleanManagedLogs(const QString &activePath, const QDateTime &now)
{
    const QFileInfo activeFile(activePath);
    QDir directory(activeFile.absolutePath());
    QList<ManagedLog> removableLogs;
    const QFileInfoList files = directory.entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot,
        QDir::NoSort);
    for (const QFileInfo &candidate : files) {
        ManagedLog log = managedLog(candidate, activeFile);
        if (log.kind == ManagedLogKind::None
            || candidate.absoluteFilePath() == activeFile.absoluteFilePath())
            continue;
        if (log.kind == ManagedLogKind::Active && processIsRunning(log.pid))
            continue;
        removableLogs.append(std::move(log));
    }

    const QDateTime expiration = now.addDays(-archiveRetentionDays);
    for (qsizetype index = removableLogs.size() - 1; index >= 0; --index) {
        if (removableLogs.at(index).file.lastModified().toLocalTime() >= expiration)
            continue;
        const QString path = removableLogs.at(index).file.absoluteFilePath();
        if (!QFile::remove(path)) {
            writeToStderr("RuntimeLogger: failed to remove an expired managed log");
            continue;
        }
        removableLogs.removeAt(index);
    }

    std::sort(removableLogs.begin(), removableLogs.end(), [](const ManagedLog &left, const ManagedLog &right) {
        if (left.file.lastModified() != right.file.lastModified())
            return left.file.lastModified() > right.file.lastModified();
        return left.file.fileName() > right.file.fileName();
    });
    qsizetype deletionIndex = removableLogs.size() - 1;
    while (removableLogs.size() > maximumArchiveCount && deletionIndex >= 0) {
        const QString path = removableLogs.at(deletionIndex).file.absoluteFilePath();
        if (!QFile::remove(path)) {
            writeToStderr("RuntimeLogger: failed to remove an excess managed log");
            --deletionIndex;
            continue;
        }
        removableLogs.removeAt(deletionIndex);
        --deletionIndex;
    }
}

QString nextArchivePath(LoggerState &state, const QDateTime &now)
{
    const QFileInfo activeFile(state.activePath);
    const QString prefix = activeFile.completeBaseName()
        + QStringLiteral(".archive-")
        + now.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
        + u'-';
    const QString suffix = logFamilySuffix(activeFile);
    const QDir directory(activeFile.absolutePath());

    for (int attempt = 0; attempt < 10'000; ++attempt) {
        const QString name = prefix
            + QString::number(state.rotationSequence++)
            + suffix;
        const QString path = directory.absoluteFilePath(name);
        if (!QFileInfo::exists(path))
            return path;
    }
    return {};
}

bool openActiveFile(LoggerState &state, const QDateTime &now)
{
    const QFileInfo existingFile(state.activePath);
    const QDate existingDate = existingFile.exists() && existingFile.size() > 0
        ? existingFile.lastModified().toLocalTime().date()
        : now.date();
    state.file.setFileName(state.activePath);
    if (!state.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        writeToStderr(
            "RuntimeLogger: failed to open the configured log file: "
            + state.file.errorString().toUtf8());
        return false;
    }
    if (!state.activeDate.isValid())
        state.activeDate = existingDate;
    return true;
}

bool rotateActiveFile(LoggerState &state, const QDateTime &now)
{
    if (state.file.isOpen()) {
        state.file.flush();
        state.file.close();
    }

    const QFileInfo activeFile(state.activePath);
    if (activeFile.exists() && activeFile.size() > 0) {
        const QString archivePath = nextArchivePath(state, now);
        if (archivePath.isEmpty() || !QFile::rename(state.activePath, archivePath)) {
            writeToStderr("RuntimeLogger: failed to rotate the configured log file");
            return false;
        }
    }

    cleanManagedLogs(state.activePath, now);
    state.rotationRetryAfter = 0;
    state.activeDate = now.date();
    return openActiveFile(state, now);
}

QByteArray boundedUtf8(const QString &value, qsizetype byteLimit)
{
    static const QByteArray truncationMarker =
        QByteArrayLiteral("...[message truncated by RuntimeLogger]");
    const QByteArray encoded = value.toUtf8();
    if (encoded.size() <= byteLimit)
        return encoded;
    if (byteLimit <= truncationMarker.size())
        return truncationMarker.left(std::max<qsizetype>(0, byteLimit));

    const qsizetype contentLimit = byteLimit - truncationMarker.size();
    qsizetype lower = 0;
    qsizetype upper = value.size();
    while (lower < upper) {
        const qsizetype middle = lower + (upper - lower + 1) / 2;
        qsizetype boundary = middle;
        if (boundary > 0 && boundary < value.size()
            && value.at(boundary - 1).isHighSurrogate()
            && value.at(boundary).isLowSurrogate()) {
            --boundary;
        }
        if (value.left(boundary).toUtf8().size() <= contentLimit)
            lower = middle;
        else
            upper = middle - 1;
    }
    if (lower > 0 && lower < value.size()
        && value.at(lower - 1).isHighSurrogate()
        && value.at(lower).isLowSurrogate()) {
        --lower;
    }
    QByteArray result = value.left(lower).toUtf8();
    while (result.size() > contentLimit && lower > 0) {
        --lower;
        if (lower > 0 && value.at(lower - 1).isHighSurrogate())
            --lower;
        result = value.left(lower).toUtf8();
    }
    result.append(truncationMarker);
    return result;
}

QByteArray formatEntry(
    QtMsgType type,
    const QMessageLogContext &context,
    const QString &message,
    const QDateTime &now)
{
    const auto threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    const QString category = context.category
        ? QString::fromUtf8(context.category)
        : QStringLiteral("default");
    const QString sourceFile = context.file
        ? QString::fromUtf8(context.file)
        : QStringLiteral("<unknown>");
    const QString function = context.function
        ? QString::fromUtf8(context.function)
        : QStringLiteral("<unknown>");
    const QString source = QStringLiteral("%1:%2").arg(sourceFile).arg(context.line);

    QString prefix;
    prefix.reserve(category.size() + 128);
    // QDateTime's LocalTime ISO rendering omits the UTC offset on some Qt
    // builds. Convert to a fixed offset first so logs can be correlated with
    // proxy/server timestamps without guessing the machine time zone.
    prefix += now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODateWithMs);
    prefix += QStringLiteral(" pid=");
    prefix += QString::number(QCoreApplication::applicationPid());
    prefix += QStringLiteral(" thread=0x");
    prefix += QString::number(threadId, 16);
    prefix += QStringLiteral(" level=");
    prefix += QString::fromLatin1(levelName(type));
    prefix += QStringLiteral(" category=");
    prefix += quoteField(category);
    prefix += QStringLiteral(" message=\"");

    QString suffix = QStringLiteral("\" source=");
    suffix += quoteField(source);
    suffix += QStringLiteral(" function=");
    suffix += quoteField(function);
    suffix += u'\n';

    QByteArray encodedPrefix = prefix.toUtf8();
    const QByteArray encodedSuffix = suffix.toUtf8();
    const qsizetype messageBudget = std::max<qsizetype>(
        0,
        maximumEntryBytes - encodedPrefix.size() - encodedSuffix.size());
    encodedPrefix += boundedUtf8(
        escapeFieldContent(redactSensitiveData(message)),
        messageBudget);
    encodedPrefix += encodedSuffix;
    return encodedPrefix;
}

bool prepareForWrite(LoggerState &state, const QDateTime &now, qsizetype bytes)
{
    if (!state.file.isOpen() && !openActiveFile(state, now))
        return false;

    const bool dateChanged = state.activeDate != now.date();
    const bool sizeExceeded = state.file.size() > 0
        && state.file.size() + bytes > maximumLogBytes;
    const qint64 nowMilliseconds = now.toMSecsSinceEpoch();
    if ((dateChanged || sizeExceeded)
        && nowMilliseconds >= state.rotationRetryAfter) {
        if (!rotateActiveFile(state, now)) {
            // Preserve logging after a transient sharing/permission failure and
            // avoid retrying the same failing rename for every message.
            state.rotationRetryAfter = nowMilliseconds + rotationRetryMilliseconds;
            if (!state.file.isOpen())
                openActiveFile(state, now);
        }
    }
    return state.file.isOpen();
}

void writeEntryLocked(
    LoggerState &state,
    QtMsgType type,
    const QMessageLogContext &context,
    const QString &message,
    const QDateTime &now)
{
    const QByteArray entry = formatEntry(type, context, message, now);
    if (!prepareForWrite(state, now, entry.size())) {
        writeToStderr(entry.trimmed());
        return;
    }

    if (state.file.write(entry) != entry.size()) {
        writeToStderr(
            "RuntimeLogger: failed to write log file: "
            + state.file.errorString().toUtf8());
        writeToStderr(entry.trimmed());
        return;
    }
    state.file.flush();
}

void messageHandler(
    QtMsgType type,
    const QMessageLogContext &context,
    const QString &message)
{
    thread_local bool handlingMessage = false;
    if (handlingMessage) {
        writeToStderr("RuntimeLogger: recursive Qt log message suppressed");
        return;
    }
    struct RecursionGuard
    {
        explicit RecursionGuard(bool &active)
            : flag(active)
        {
            flag = true;
        }
        ~RecursionGuard()
        {
            flag = false;
        }
        bool &flag;
    } recursionGuard(handlingMessage);
    LoggerState &state = loggerState();
    const QMutexLocker locker(&state.mutex);
    if (!state.installed) {
        writeToStderr(formatEntry(type, context, message, QDateTime::currentDateTime()).trimmed());
        return;
    }
    writeEntryLocked(state, type, context, message, QDateTime::currentDateTime());
}

void writeLifecycleEntry(LoggerState &state, const QString &message)
{
    const QMessageLogContext context(
        __FILE__,
        __LINE__,
        Q_FUNC_INFO,
        "yanami.runtime.logger");
    writeEntryLocked(state, QtInfoMsg, context, message, QDateTime::currentDateTime());
}

} // namespace

bool install()
{
    LoggerState &state = loggerState();
    const QMutexLocker locker(&state.mutex);
    if (state.installed)
        return true;

    state.activePath = resolvedLogPath();
    state.activeDate = {};
    state.rotationRetryAfter = 0;
    if (state.activePath.isEmpty()) {
        writeToStderr("RuntimeLogger: AppLocalDataLocation is unavailable");
        return false;
    }

    const QFileInfo activeFile(state.activePath);
    QDir directory(activeFile.absolutePath());
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        writeToStderr(
            "RuntimeLogger: failed to create the configured log directory");
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    cleanManagedLogs(state.activePath, now);
    if (activeFile.exists() && activeFile.size() > 0
        && (activeFile.lastModified().toLocalTime().date() != now.date()
            || activeFile.size() >= maximumLogBytes)) {
        if (!rotateActiveFile(state, now)) {
            state.rotationRetryAfter = now.toMSecsSinceEpoch() + rotationRetryMilliseconds;
            if (!state.file.isOpen() && !openActiveFile(state, now))
                return false;
        }
    } else if (!openActiveFile(state, now)) {
        return false;
    }

    state.installed = true;
    state.previousHandler = qInstallMessageHandler(messageHandler);
    writeLifecycleEntry(
        state,
        QStringLiteral(
            "runtime logger installed destination=%1 maxBytes=%2 retentionDays=%3 maxArchives=%4")
            .arg(DevelopmentHooks::value(DevelopmentHooks::Variable::LogPath).isEmpty()
                     ? QStringLiteral("app-local-data")
                     : QStringLiteral("environment-override"))
            .arg(maximumLogBytes)
            .arg(archiveRetentionDays)
            .arg(maximumArchiveCount));
    return true;
}

void shutdown()
{
    LoggerState &state = loggerState();
    const QMutexLocker locker(&state.mutex);
    if (!state.installed)
        return;

    writeLifecycleEntry(state, QStringLiteral("runtime logger shutting down"));
    qInstallMessageHandler(state.previousHandler);
    state.previousHandler = nullptr;
    state.installed = false;
    if (state.file.isOpen()) {
        state.file.flush();
        state.file.close();
    }
}

QString currentLogPath()
{
    LoggerState &state = loggerState();
    const QMutexLocker locker(&state.mutex);
    return state.activePath;
}

} // namespace RuntimeLogger
