#include "ApplicationPaths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

#ifdef Q_OS_WIN
#include <cstdlib>
#include <string>
#include <qt_windows.h>
#endif

namespace ApplicationPaths {
namespace {

struct State
{
    bool configured = false;
    bool isolated = false;
    QString profileRoot;
    ConfigurationResult result;
};

State &state()
{
    static State value;
    return value;
}

bool pathsReferToSameLocation(const QString &left, const QString &right)
{
    const QString cleanLeft = QDir::cleanPath(left);
    const QString cleanRight = QDir::cleanPath(right);
#ifdef Q_OS_WIN
    return cleanLeft.compare(cleanRight, Qt::CaseInsensitive) == 0;
#else
    return cleanLeft == cleanRight;
#endif
}

QString childPath(const QString &root, const QString &relative)
{
    return QDir(root).absoluteFilePath(relative);
}

bool setEnvironmentPath(const char *name, const QString &path, QString *error)
{
#ifdef Q_OS_WIN
    const std::wstring variableName =
        QString::fromLatin1(name).toStdWString();
    const std::wstring nativePath = path.toStdWString();
    const BOOL operatingSystemUpdated = ::SetEnvironmentVariableW(
        variableName.c_str(), nativePath.c_str());
    const DWORD operatingSystemError = operatingSystemUpdated
        ? ERROR_SUCCESS : ::GetLastError();
    const int runtimeError =
        ::_wputenv_s(variableName.c_str(), nativePath.c_str());
    if (operatingSystemUpdated && runtimeError == 0) {
        return true;
    }
#else
    if (qputenv(name, QFile::encodeName(path)))
        return true;
#endif
    if (error) {
#ifdef Q_OS_WIN
        *error = QStringLiteral(
                     "Unable to set isolated environment variable %1 "
                     "(Windows error %2, runtime error %3).")
                     .arg(QString::fromLatin1(name),
                          QString::number(operatingSystemError),
                          QString::number(runtimeError));
#else
        *error = QStringLiteral("Unable to set isolated environment variable %1.")
                     .arg(QString::fromLatin1(name));
#endif
    }
    return false;
}

} // namespace

bool validateIsolatedProfileRoot(
    const QString &requestedRoot,
    QString *resolvedRoot,
    QString *error)
{
    if (resolvedRoot)
        resolvedRoot->clear();
    if (error)
        error->clear();

    const QString trimmed = requestedRoot.trimmed();
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("The isolated profile root is empty.");
        return false;
    }

    const QFileInfo requestedInfo(trimmed);
    if (!requestedInfo.isAbsolute()) {
        if (error) {
            *error = QStringLiteral(
                "The isolated profile root must be an absolute path.");
        }
        return false;
    }

    const QString absoluteRoot = QDir::cleanPath(
        requestedInfo.absoluteFilePath());
    const QString filesystemRoot = QFileInfo(absoluteRoot).dir().rootPath();
    if (pathsReferToSameLocation(absoluteRoot, filesystemRoot)) {
        if (error) {
            *error = QStringLiteral(
                "The isolated profile root cannot be a filesystem root.");
        }
        return false;
    }

    if (resolvedRoot)
        *resolvedRoot = absoluteRoot;
    return true;
}

ConfigurationResult configureFromEnvironment()
{
    State &current = state();
    if (current.configured)
        return current.result;

    current.configured = true;
    const QString requestedRoot = qEnvironmentVariable(
        isolatedProfileEnvironment);
    if (requestedRoot.isEmpty()) {
        current.result = {};
        return current.result;
    }

    QString root;
    QString error;
    if (!validateIsolatedProfileRoot(requestedRoot, &root, &error)) {
        current.result = {
            .succeeded = false,
            .isolated = true,
            .profileRoot = {},
            .error = error,
        };
        return current.result;
    }

    const QStringList requiredDirectories {
        root,
        childPath(root, QStringLiteral("data/cache")),
        childPath(root, QStringLiteral("data/models/upscaling")),
        childPath(root, QStringLiteral("logs")),
        childPath(root, QStringLiteral("settings/user")),
        childPath(root, QStringLiteral("settings/system")),
        childPath(root, QStringLiteral("qt/qml-cache")),
        childPath(root, QStringLiteral("temp")),
#ifdef Q_OS_WIN
        childPath(root, QStringLiteral("roaming")),
        childPath(root, QStringLiteral("local")),
#else
        childPath(root, QStringLiteral("xdg/data")),
        childPath(root, QStringLiteral("xdg/cache")),
        childPath(root, QStringLiteral("xdg/config")),
#endif
    };
    for (const QString &directory : requiredDirectories) {
        if (!QDir().mkpath(directory)) {
            current.result = {
                .succeeded = false,
                .isolated = true,
                .profileRoot = root,
                .error = QStringLiteral(
                    "Unable to create isolated profile directory: %1")
                             .arg(directory),
            };
            return current.result;
        }
    }

    const QString qmlCache = childPath(root, QStringLiteral("qt/qml-cache"));
    if (!setEnvironmentPath("QML_DISK_CACHE_PATH", qmlCache, &error)
        || !setEnvironmentPath(
            "TMPDIR", childPath(root, QStringLiteral("temp")), &error)) {
        current.result = {false, true, root, error};
        return current.result;
    }
#ifdef Q_OS_WIN
    if (!setEnvironmentPath(
            "APPDATA", childPath(root, QStringLiteral("roaming")), &error)
        || !setEnvironmentPath(
            "LOCALAPPDATA", childPath(root, QStringLiteral("local")), &error)
        || !setEnvironmentPath(
            "TEMP", childPath(root, QStringLiteral("temp")), &error)
        || !setEnvironmentPath(
            "TMP", childPath(root, QStringLiteral("temp")), &error)) {
        current.result = {false, true, root, error};
        return current.result;
    }
#else
    if (!setEnvironmentPath(
            "XDG_DATA_HOME", childPath(root, QStringLiteral("xdg/data")), &error)
        || !setEnvironmentPath(
            "XDG_CACHE_HOME", childPath(root, QStringLiteral("xdg/cache")), &error)
        || !setEnvironmentPath(
            "XDG_CONFIG_HOME", childPath(root, QStringLiteral("xdg/config")), &error)) {
        current.result = {false, true, root, error};
        return current.result;
    }
#endif

    // Default-constructed QSettings uses the platform registry on Windows.
    // Force both lookup scopes into this run so even fallback reads cannot
    // observe a real Yanami setting.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        childPath(root, QStringLiteral("settings/user")));
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::SystemScope,
        childPath(root, QStringLiteral("settings/system")));

    current.isolated = true;
    current.profileRoot = root;
    current.result = {true, true, root, {}};
    return current.result;
}

bool isolated()
{
    return state().isolated;
}

QString profileRoot()
{
    return state().profileRoot;
}

QString dataRoot()
{
    if (isolated())
        return childPath(profileRoot(), QStringLiteral("data"));
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString cacheRoot()
{
    return QDir(dataRoot()).filePath(QStringLiteral("cache"));
}

QString upscalingAssetRoot()
{
    return QDir(dataRoot()).filePath(QStringLiteral("models/upscaling"));
}

QString logFilePath()
{
    if (isolated()) {
        return childPath(
            profileRoot(), QStringLiteral("logs/yanami.log"));
    }
    const QString dataLocation = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    return dataLocation.isEmpty()
        ? QString {}
        : QDir(dataLocation).absoluteFilePath(
              QStringLiteral("logs/yanami.log"));
}

} // namespace ApplicationPaths
