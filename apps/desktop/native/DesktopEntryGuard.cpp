#include "DesktopEntryGuard.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace {
constexpr auto ReadyFileOption = "--yanami-bootstrap-ready-file";
constexpr auto ReadyHandleOption = "--yanami-bootstrap-ready-handle";

bool isBootstrapHandoffArgument(const QString &argument)
{
    const auto matches = [&argument](const char *option) {
        const QString normalized = QString::fromLatin1(option);
        return argument == normalized
            || argument.startsWith(normalized + QLatin1Char('='));
    };
    return matches(ReadyFileOption) || matches(ReadyHandleOption);
}
}

namespace DesktopEntryGuard {

QStringList forwardedArguments(const QStringList &arguments)
{
    QStringList forwarded;
    // QCoreApplication::arguments() includes argv[0], while QProcess expects
    // only the child arguments.
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (!isBootstrapHandoffArgument(arguments.at(index)))
            forwarded.push_back(arguments.at(index));
    }
    return forwarded;
}

RedirectResult redirectToSiblingBootstrap(const QStringList &arguments)
{
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const QString launcherPath = applicationDirectory.filePath(
        QStringLiteral("Yanami.exe"));
    const QFileInfo launcher(launcherPath);
    if (!launcher.exists() || !launcher.isFile()) {
        return {
            RedirectStatus::LauncherMissing,
            0,
            QStringLiteral("sibling_launcher_missing"),
        };
    }

    qint64 processId = 0;
    if (!QProcess::startDetached(
            launcher.absoluteFilePath(),
            forwardedArguments(arguments),
            applicationDirectory.absolutePath(),
            &processId)) {
        return {
            RedirectStatus::StartFailed,
            0,
            QStringLiteral("sibling_launcher_start_failed"),
        };
    }
    return {RedirectStatus::Started, processId, {}};
}

} // namespace DesktopEntryGuard
