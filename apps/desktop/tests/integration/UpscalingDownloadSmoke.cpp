#include "UpscalingAssetManager.hpp"
#include "UpscalingCatalog.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("yanami-upscaling-download-smoke"));
    const QString provider = argc > 1
        ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("anime4k");
    const QString preset = argc > 2
        ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("performance");
    const auto profile = YanamiUpscaling::UpscalingCatalog::resolve(
        provider, preset);
    if (profile.providerId != provider || profile.requiredArtifacts.isEmpty())
        return 2;

    QTemporaryDir directory;
    if (!directory.isValid())
        return 3;
    UpscalingAssetManager manager(directory.path());
    const QString taskId = QStringLiteral("integration-%1-%2")
        .arg(provider, preset);
    QObject::connect(
        &manager,
        &UpscalingAssetManager::stateChanged,
        &application,
        [&] (const QString &changedId) {
            if (changedId != taskId)
                return;
            const QVariantMap state = manager.stateFor(taskId);
            const QString phase = state.value(
                QStringLiteral("phase")).toString();
            if (phase != QStringLiteral("ready")
                && phase != QStringLiteral("failed")) {
                return;
            }
            bool filesValid = phase == QStringLiteral("ready");
            for (const auto &artifact : profile.requiredArtifacts) {
                const QFileInfo info(manager.absolutePath(artifact));
                filesValid &= info.exists()
                    && info.isFile()
                    && info.size() == artifact.sizeBytes;
            }
            const QJsonObject result {
                {QStringLiteral("providerId"), provider},
                {QStringLiteral("presetId"), preset},
                {QStringLiteral("profileId"), profile.profileId},
                {QStringLiteral("phase"), phase},
                {QStringLiteral("fileCount"),
                 profile.requiredArtifacts.size()},
                {QStringLiteral("bytesTotal"),
                 state.value(QStringLiteral("bytesTotal")).toLongLong()},
                {QStringLiteral("filesValid"), filesValid},
                {QStringLiteral("errorCode"),
                 state.value(QStringLiteral("errorCode")).toString()},
            };
            fprintf(stdout, "%s\n",
                QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
            application.exit(filesValid ? 0 : 1);
        });
    QTimer::singleShot(60'000, &application, [&] {
        fprintf(stderr, "upscaling download smoke timed out\n");
        application.exit(4);
    });
    manager.download(taskId, profile.requiredArtifacts);
    return application.exec();
}
