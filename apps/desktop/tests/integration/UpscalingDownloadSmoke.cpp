#include "UpscalingAssetManager.hpp"
#include "UpscalingCatalog.hpp"
#include "OfflineNetworkAccessManager.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

namespace {

YanamiUpscaling::ShaderArtifact offlineArtifact(
    const YanamiUpscaling::ResolvedProfile &profile,
    const QByteArray &payload)
{
    return {
        .id = QStringLiteral("anime4k-offline-smoke-shader"),
        .providerId = profile.providerId,
        .version = profile.version,
        .assetSetId = QStringLiteral("anime4k-offline-smoke"),
        .fileName = QStringLiteral("offline-smoke.glsl"),
        .installRelativePath = QStringLiteral(
            "anime4k/offline-smoke/offline-smoke.glsl"),
        .downloadUrl = QStringLiteral(
            "https://raw.githubusercontent.com/bloc97/Anime4K/offline-smoke.glsl"),
        .sizeBytes = payload.size(),
        .sha256 = QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()),
        .licenseSpdx = QStringLiteral("MIT"),
        .licenseUrl = QStringLiteral(
            "https://github.com/bloc97/Anime4K/blob/master/LICENSE"),
    };
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("yanami-upscaling-download-smoke"));
    const QString provider = argc > 1
        ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("anime4k");
    const QString preset = argc > 2
        ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("performance");
    const QString smokeMode = argc > 3
        ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("live");
    const bool offline = smokeMode == QStringLiteral("offline");
    if (!offline && smokeMode != QStringLiteral("live"))
        return 5;
    const auto profile = YanamiUpscaling::UpscalingCatalog::resolve(
        provider, preset);
    if (profile.providerId != provider || profile.requiredArtifacts.isEmpty())
        return 2;

    QTemporaryDir directory;
    if (!directory.isValid())
        return 3;

    const QByteArray offlinePayload = QByteArrayLiteral(
        "//!HOOK MAIN\n"
        "//!BIND HOOKED\n"
        "vec4 hook(){return HOOKED_tex(HOOKED_pos);}\n");
    const QVector<YanamiUpscaling::ShaderArtifact> artifacts = offline
        ? QVector<YanamiUpscaling::ShaderArtifact> {
              offlineArtifact(profile, offlinePayload)}
        : profile.requiredArtifacts;
    auto offlineNetwork = offline
        ? std::make_unique<YanamiTest::OfflineNetworkAccessManager>(
              offlinePayload, true, &application)
        : nullptr;
    auto manager = offline
        ? std::make_unique<UpscalingAssetManager>(
              offlineNetwork.get(), directory.path())
        : std::make_unique<UpscalingAssetManager>(directory.path());
    const QString taskId = QStringLiteral("integration-%1-%2")
        .arg(provider, preset);
    QObject::connect(
        manager.get(),
        &UpscalingAssetManager::stateChanged,
        &application,
        [&] (const QString &changedId) {
            if (changedId != taskId)
                return;
            const QVariantMap state = manager->stateFor(taskId);
            const QString phase = state.value(
                QStringLiteral("phase")).toString();
            if (phase != QStringLiteral("ready")
                && phase != QStringLiteral("failed")) {
                return;
            }
            bool filesValid = phase == QStringLiteral("ready");
            for (const auto &artifact : artifacts) {
                const QFileInfo info(manager->absolutePath(artifact));
                filesValid &= info.exists()
                    && info.isFile()
                    && info.size() == artifact.sizeBytes;
            }
            const int networkRequests = offlineNetwork
                ? offlineNetwork->requestCount() : -1;
            if (offline)
                filesValid &= networkRequests == artifacts.size();
            const QJsonObject result {
                {QStringLiteral("providerId"), provider},
                {QStringLiteral("presetId"), preset},
                {QStringLiteral("profileId"), profile.profileId},
                {QStringLiteral("phase"), phase},
                {QStringLiteral("fileCount"),
                 artifacts.size()},
                {QStringLiteral("bytesTotal"),
                 state.value(QStringLiteral("bytesTotal")).toLongLong()},
                {QStringLiteral("filesValid"), filesValid},
                {QStringLiteral("mode"), smokeMode},
                {QStringLiteral("networkRequests"), networkRequests},
                {QStringLiteral("errorCode"),
                 state.value(QStringLiteral("errorCode")).toString()},
            };
            fprintf(stdout, "%s\n",
                QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
            application.exit(filesValid ? 0 : 1);
        });
    QTimer::singleShot(offline ? 10'000 : 60'000, &application, [&] {
        fprintf(stderr, "upscaling download smoke timed out\n");
        application.exit(4);
    });
    manager->download(taskId, artifacts);
    return application.exec();
}
