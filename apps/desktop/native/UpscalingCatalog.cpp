#include "UpscalingCatalog.hpp"

#include <QHash>
#include <QMetaType>
#include <QSet>

namespace YanamiUpscaling {
namespace {

constexpr auto kAnime4kVersion = "v4.0.1";
constexpr auto kAnime4kCommit = "4029bf701ecaa15f163cdc49cffe5501c1acf410";
constexpr auto kAnime4kAssetSet =
    "anime4k-v4.0.1-4029bf701ecaa15f163cdc49cffe5501c1acf410-glsl";

QString providerAnime4k()
{
    return QString::fromLatin1(Id::ProviderAnime4k);
}

ShaderArtifact anime4kArtifact(
    const QString &id,
    const QString &sourcePath,
    qint64 sizeBytes,
    const QString &sha256)
{
    const QString version = QString::fromLatin1(kAnime4kVersion);
    const QString fileName = sourcePath.section(QLatin1Char('/'), -1);
    return {
        id,
        providerAnime4k(),
        version,
        QString::fromLatin1(kAnime4kAssetSet),
        fileName,
        QStringLiteral("anime4k/%1/%2").arg(version, fileName),
        QStringLiteral("https://raw.githubusercontent.com/bloc97/Anime4K/%1/%2")
            .arg(QString::fromLatin1(kAnime4kCommit), sourcePath),
        sizeBytes,
        sha256,
        QStringLiteral("MIT"),
        QStringLiteral("https://github.com/bloc97/Anime4K/blob/%1/LICENSE")
            .arg(QString::fromLatin1(kAnime4kCommit))};
}

QVector<ShaderArtifact> makeArtifacts()
{
    QVector<ShaderArtifact> result;
    result.reserve(23);

    result.append(anime4kArtifact(
        QStringLiteral("anime4k.clamp"),
        QStringLiteral("glsl/Restore/Anime4K_Clamp_Highlights.glsl"),
        2795,
        QStringLiteral("a2a9bf7fbc1d75d09660ca2e701e4d7fb0cf5457b94da47e1825032fa2b3671a")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.autodown.x2"),
        QStringLiteral("glsl/Upscale/Anime4K_AutoDownscalePre_x2.glsl"),
        1560,
        QStringLiteral("8c58291740146bd766a4d73f132775a797fe80f7d07919b5d767e27a5dc85656")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.autodown.x4"),
        QStringLiteral("glsl/Upscale/Anime4K_AutoDownscalePre_x4.glsl"),
        1568,
        QStringLiteral("5af62d8cd844916dc1126613e13bad3beab195787f93a71200b47c6ec78f2e41")));

    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.a.s"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_S.glsl"),
        17136,
        QStringLiteral("97c24dc370ab300c108bfaa09db7f175aeff343674842c299cf3940a3d330427")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.a.m"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_M.glsl"),
        35916,
        QStringLiteral("67ea3ed26539e8de3b7d307688535d2ff17e8d147e11dda0247da7770dbecf41")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.a.l"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_L.glsl"),
        69921,
        QStringLiteral("d6efe215e6ee8af1ec560478a91afc1df83fac4ba43b2c806ee61ca2267ed674")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.a.vl"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_VL.glsl"),
        144075,
        QStringLiteral("35036722733305cd4d4e57660b883bbe2569ba2914033c254327107d7b77e35e")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.a.ul"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_UL.glsl"),
        308660,
        QStringLiteral("81ec48ff700108c8e1571ed82d4527006ee200c05e94f8b7f836c02179169b40")));

    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.b.s"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_Soft_S.glsl"),
        17198,
        QStringLiteral("9f6867f2ef42786729522d86fe24147cb4ea145418e3974df70496acf52dc392")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.b.m"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_Soft_M.glsl"),
        36016,
        QStringLiteral("a78a2c76898e08e09e442a9628c64208c26e8e15789649b8755223f009794c02")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.b.l"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_Soft_L.glsl"),
        70020,
        QStringLiteral("8f2a5c73b526c6e4c67bce0366f7dcd7410bfa4a31e718240d86f53660788e63")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.b.vl"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_Soft_VL.glsl"),
        144204,
        QStringLiteral("094334b0e20c1a201fe4941c7c68de72451e5aee9efb5524d7fb82b12dca64b9")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.restore.b.ul"),
        QStringLiteral("glsl/Restore/Anime4K_Restore_CNN_Soft_UL.glsl"),
        308873,
        QStringLiteral("41d71cad7b2f3af9086852fca70046f763272d2cb49dd83de9647d942866cac8")));

    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale.s"),
        QStringLiteral("glsl/Upscale/Anime4K_Upscale_CNN_x2_S.glsl"),
        18638,
        QStringLiteral("4c53ec2e287908f7ee7bcb266b0170421626d663576468b7d7dafc62962649a4")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale.m"),
        QStringLiteral("glsl/Upscale/Anime4K_Upscale_CNN_x2_M.glsl"),
        37685,
        QStringLiteral("716e02098a68f0d648761f2b96b4dd139e1cb09b174bb369fca3aa34328fff7e")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale.l"),
        QStringLiteral("glsl/Upscale/Anime4K_Upscale_CNN_x2_L.glsl"),
        73443,
        QStringLiteral("db1fedf7be82f6fd9034e6bf39b64daf2b7576988bb584ec38f24f5236b1cd97")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale.vl"),
        QStringLiteral("glsl/Upscale/Anime4K_Upscale_CNN_x2_VL.glsl"),
        146743,
        QStringLiteral("5638fe31c37c151a3443fea3451a3ef91af073f4dbb9615f6c0d1e29db11493d")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale.ul"),
        QStringLiteral("glsl/Upscale/Anime4K_Upscale_CNN_x2_UL.glsl"),
        290257,
        QStringLiteral("fa7cf0ecc1cca84d8291bbff5a42b60f5816d57b4e97d42bc377235ac8db02e8")));

    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale-denoise.s"),
        QStringLiteral("glsl/Upscale+Denoise/Anime4K_Upscale_Denoise_CNN_x2_S.glsl"),
        18667,
        QStringLiteral("1a45ad3aa20d8368399f2fd46791deed957c2fd0a4afd131cc92516827abab93")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale-denoise.m"),
        QStringLiteral("glsl/Upscale+Denoise/Anime4K_Upscale_Denoise_CNN_x2_M.glsl"),
        37714,
        QStringLiteral("8c72b042e2301fe66a45c3089720459148e2504cd72af16f9c0d5017ff14181e")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale-denoise.l"),
        QStringLiteral("glsl/Upscale+Denoise/Anime4K_Upscale_Denoise_CNN_x2_L.glsl"),
        73584,
        QStringLiteral("6cc4604c9544fd4fd9e3a75fd797511bf5fe1e626e9e1dbcee03302823a63207")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale-denoise.vl"),
        QStringLiteral("glsl/Upscale+Denoise/Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"),
        146811,
        QStringLiteral("359c48fe5a317fbc6b706ce368401eef496e84ed98abac7a43efebca2b65d79b")));
    result.append(anime4kArtifact(
        QStringLiteral("anime4k.upscale-denoise.ul"),
        QStringLiteral("glsl/Upscale+Denoise/Anime4K_Upscale_Denoise_CNN_x2_UL.glsl"),
        290040,
        QStringLiteral("38bd11e7e92ff1a615a274be912d6f2cfe67f5b43771f0383a91c6ab3d0f8cb6")));

    return result;
}

const QVector<ShaderArtifact> &allArtifacts()
{
    static const QVector<ShaderArtifact> value = makeArtifacts();
    return value;
}

const QHash<QString, qsizetype> &artifactIndex()
{
    static const QHash<QString, qsizetype> value = [] {
        QHash<QString, qsizetype> index;
        index.reserve(allArtifacts().size());
        for (qsizetype i = 0; i < allArtifacts().size(); ++i) {
            index.insert(allArtifacts().at(i).id, i);
        }
        return index;
    }();
    return value;
}

QStringList fallbackProfiles(const QString &providerId, const QString &presetId)
{
    const QString performance = providerId + QStringLiteral("/performance");
    const QString balanced = providerId + QStringLiteral("/balanced");
    if (presetId == QLatin1String(Id::PresetPerformance)) {
        return {QStringLiteral("original")};
    }
    if (presetId == QLatin1String(Id::PresetBalanced)) {
        return {performance, QStringLiteral("original")};
    }
    // A custom chain can be cheaper than the standard Balanced profile (for
    // example Anime4K S with a single pass). Its exact GPU cost is not known
    // until it is benchmarked on the active renderer, so never "downgrade"
    // custom to Balanced. Performance is the only bounded standard fallback.
    if (presetId == QLatin1String(Id::PresetCustom)) {
        return {performance, QStringLiteral("original")};
    }
    return {balanced, performance, QStringLiteral("original")};
}

QVector<ShaderArtifact> requiredArtifacts(const QStringList &orderedIds)
{
    QVector<ShaderArtifact> result;
    QSet<QString> seen;
    result.reserve(orderedIds.size());
    for (const QString &id : orderedIds) {
        if (seen.contains(id)) {
            continue;
        }
        const auto it = artifactIndex().constFind(id);
        if (it != artifactIndex().cend()) {
            result.append(allArtifacts().at(it.value()));
            seen.insert(id);
        }
    }
    return result;
}

ResolvedProfile baseProfile(
    const QString &providerId,
    const QString &presetId,
    const QString &version,
    const QString &assetSetId)
{
    ResolvedProfile result;
    result.providerId = providerId;
    result.presetId = presetId;
    result.profileId = providerId + QLatin1Char('/') + presetId;
    result.version = version;
    result.assetSetId = assetSetId;
    result.backendKind = BackendKind::GlslShaders;
    result.fallbackProfileIds = fallbackProfiles(providerId, presetId);
    return result;
}

bool hasOnlyFields(const QVariantMap &value, const QSet<QString> &allowed)
{
    for (auto it = value.cbegin(); it != value.cend(); ++it) {
        if (!allowed.contains(it.key())) {
            return false;
        }
    }
    return true;
}

bool readStrictString(
    const QVariantMap &value,
    const QString &key,
    const QSet<QString> &allowed,
    QString *target)
{
    const auto it = value.constFind(key);
    if (it == value.cend()) {
        return true;
    }
    if (it->metaType().id() != QMetaType::QString) {
        return false;
    }
    const QString candidate = it->toString();
    if (!allowed.contains(candidate)) {
        return false;
    }
    *target = candidate;
    return true;
}

bool readStrictBool(
    const QVariantMap &value,
    const QString &key,
    bool *target)
{
    const auto it = value.constFind(key);
    if (it == value.cend())
        return true;
    if (it->metaType().id() != QMetaType::Bool)
        return false;
    *target = it->toBool();
    return true;
}

QString animeSizeSuffix(const QString &size)
{
    return size.toLower();
}

QString animeRestoreId(const QString &mode, const QString &size)
{
    const QString family = mode == QStringLiteral("B") ? QStringLiteral("b") : QStringLiteral("a");
    return QStringLiteral("anime4k.restore.%1.%2").arg(family, animeSizeSuffix(size));
}

QString animeUpscaleId(const QString &size)
{
    return QStringLiteral("anime4k.upscale.%1").arg(animeSizeSuffix(size));
}

QString animeDenoiseId(const QString &size)
{
    return QStringLiteral("anime4k.upscale-denoise.%1").arg(animeSizeSuffix(size));
}

void appendAnimePrimary(QStringList *chain, const QString &mode, const QString &size)
{
    if (mode == QStringLiteral("C")) {
        chain->append(animeDenoiseId(size));
        return;
    }
    chain->append(animeRestoreId(mode, size));
    chain->append(animeUpscaleId(size));
}

bool readStrictInt(
    const QVariantMap &value,
    const QString &key,
    int minimum,
    int maximum,
    int *target)
{
    const auto it = value.constFind(key);
    if (it == value.cend()) {
        return true;
    }
    if (it->metaType().id() != QMetaType::Int) {
        return false;
    }
    const int candidate = it->toInt();
    if (candidate < minimum || candidate > maximum) {
        return false;
    }
    *target = candidate;
    return true;
}

ResolvedProfile resolveAnime4k(const QString &presetId, const QVariantMap &custom)
{
    ResolvedProfile result = baseProfile(
        providerAnime4k(),
        presetId,
        QString::fromLatin1(kAnime4kVersion),
        QString::fromLatin1(kAnime4kAssetSet));

    if (presetId == QLatin1String(Id::PresetPerformance)) {
        result.orderedShaderArtifactIds = {
            QStringLiteral("anime4k.clamp"),
            QStringLiteral("anime4k.restore.a.m"),
            QStringLiteral("anime4k.upscale.m"),
            QStringLiteral("anime4k.autodown.x2"),
            QStringLiteral("anime4k.autodown.x4"),
            QStringLiteral("anime4k.upscale.s")};
    } else if (presetId == QLatin1String(Id::PresetBalanced)) {
        result.orderedShaderArtifactIds = {
            QStringLiteral("anime4k.clamp"),
            QStringLiteral("anime4k.restore.a.vl"),
            QStringLiteral("anime4k.upscale.vl"),
            QStringLiteral("anime4k.autodown.x2"),
            QStringLiteral("anime4k.autodown.x4"),
            QStringLiteral("anime4k.upscale.m")};
    } else if (presetId == QLatin1String(Id::PresetQuality)) {
        result.orderedShaderArtifactIds = {
            QStringLiteral("anime4k.clamp"),
            QStringLiteral("anime4k.restore.a.vl"),
            QStringLiteral("anime4k.upscale.vl"),
            QStringLiteral("anime4k.autodown.x2"),
            QStringLiteral("anime4k.autodown.x4"),
            QStringLiteral("anime4k.restore.a.m"),
            QStringLiteral("anime4k.upscale.m")};
    } else {
        QString mode = QStringLiteral("A");
        QString primarySize = QStringLiteral("M");
        QString secondarySize = QStringLiteral("S");
        int passes = 1;
        bool autoDownscale = true;
        const QSet<QString> sizes = {
            QStringLiteral("S"),
            QStringLiteral("M"),
            QStringLiteral("L"),
            QStringLiteral("VL"),
            QStringLiteral("UL")};
        bool valid = hasOnlyFields(
            custom,
            {QStringLiteral("mode"),
             QStringLiteral("primarySize"),
             QStringLiteral("secondarySize"),
             QStringLiteral("passes"),
             QStringLiteral("autoDownscale")});
        valid = valid
            && readStrictString(
                custom,
                QStringLiteral("mode"),
                {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")},
                &mode)
            && readStrictString(custom, QStringLiteral("primarySize"), sizes, &primarySize)
            && readStrictString(custom, QStringLiteral("secondarySize"), sizes, &secondarySize)
            && readStrictInt(custom, QStringLiteral("passes"), 1, 2, &passes)
            && readStrictBool(
                custom, QStringLiteral("autoDownscale"), &autoDownscale);
        // mpv cannot apply the exact same shader file twice in one chain.
        if (valid && mode != QStringLiteral("C")
            && primarySize == secondarySize) {
            valid = false;
        }
        if (!valid) {
            mode = QStringLiteral("A");
            primarySize = QStringLiteral("M");
            secondarySize = QStringLiteral("S");
            passes = 1;
            autoDownscale = true;
            result.usedDefaultCustom = true;
        }
        result.normalizedCustom = {
            {QStringLiteral("mode"), mode},
            {QStringLiteral("primarySize"), primarySize},
            {QStringLiteral("secondarySize"), secondarySize},
            {QStringLiteral("passes"), passes},
            {QStringLiteral("autoDownscale"), autoDownscale}};

        result.orderedShaderArtifactIds.append(QStringLiteral("anime4k.clamp"));
        appendAnimePrimary(&result.orderedShaderArtifactIds, mode, primarySize);
        if (autoDownscale) {
            result.orderedShaderArtifactIds.append(
                QStringLiteral("anime4k.autodown.x2"));
            result.orderedShaderArtifactIds.append(
                QStringLiteral("anime4k.autodown.x4"));
        }
        if (passes == 2) {
            // Official mode C uses denoise+upscale first, then a normal A pass.
            const QString secondaryMode = mode == QStringLiteral("C")
                ? QStringLiteral("A")
                : mode;
            appendAnimePrimary(&result.orderedShaderArtifactIds, secondaryMode, secondarySize);
        } else {
            // Anime4K A/B/C are 4x pipelines: the primary stage performs the
            // first 2x step, and a final normal upscale performs the second.
            // A one-restore-pass custom profile still needs this final shader.
            result.orderedShaderArtifactIds.append(animeUpscaleId(secondarySize));
        }
    }

    result.requiredArtifacts = requiredArtifacts(result.orderedShaderArtifactIds);
    return result;
}

} // namespace

QStringList UpscalingCatalog::providerIds()
{
    return {providerAnime4k()};
}

QStringList UpscalingCatalog::presetIds()
{
    return {
        QString::fromLatin1(Id::PresetPerformance),
        QString::fromLatin1(Id::PresetBalanced),
        QString::fromLatin1(Id::PresetQuality),
        QString::fromLatin1(Id::PresetCustom)};
}

QString UpscalingCatalog::backendKindId(BackendKind backendKind)
{
    switch (backendKind) {
    case BackendKind::GlslShaders:
        return QString::fromLatin1(Id::BackendGlslShaders);
    case BackendKind::None:
        return {};
    }
    return {};
}

bool UpscalingCatalog::parseBackendKindId(
    const QString &serialized,
    BackendKind *backendKind)
{
    if (!backendKind)
        return false;
    if (serialized == QLatin1String(Id::BackendGlslShaders)) {
        *backendKind = BackendKind::GlslShaders;
        return true;
    }
    return false;
}

const QVector<ShaderArtifact> &UpscalingCatalog::artifacts()
{
    return allArtifacts();
}

const ShaderArtifact *UpscalingCatalog::findArtifact(const QString &artifactId)
{
    const auto it = artifactIndex().constFind(artifactId);
    return it == artifactIndex().cend() ? nullptr : &allArtifacts().at(it.value());
}

ResolvedProfile UpscalingCatalog::resolve(
    const QString &providerId,
    const QString &presetId,
    const QVariantMap &custom)
{
    if (providerId != providerAnime4k()) {
        ResolvedProfile rejected;
        rejected.requestedProviderId = providerId;
        rejected.requestedPresetId = presetId;
        rejected.usedDefaultProvider = true;
        return rejected;
    }

    QString resolvedPreset = presetId;
    bool usedDefaultPreset = false;
    if (!presetIds().contains(resolvedPreset)) {
        resolvedPreset = QString::fromLatin1(Id::PresetBalanced);
        usedDefaultPreset = true;
    }

    ResolvedProfile result = resolveAnime4k(resolvedPreset, custom);
    result.requestedProviderId = providerId;
    result.requestedPresetId = presetId;
    result.usedDefaultPreset = usedDefaultPreset;
    return result;
}

QVector<ShaderArtifact> UpscalingCatalog::deploymentArtifacts(
    const ResolvedProfile &profile)
{
    QVector<ShaderArtifact> result;
    QSet<QString> seen;
    const auto append = [&result, &seen](const auto &artifacts) {
        for (const ShaderArtifact &artifact : artifacts) {
            if (artifact.id.isEmpty() || seen.contains(artifact.id))
                continue;
            seen.insert(artifact.id);
            result.append(artifact);
        }
    };

    append(profile.requiredArtifacts);
    for (const QString &fallbackId : profile.fallbackProfileIds) {
        if (fallbackId == QLatin1String("original"))
            continue;
        const QStringList parts = fallbackId.split(QLatin1Char('/'));
        if (parts.size() != 2)
            continue;
        append(resolve(parts.at(0), parts.at(1)).requiredArtifacts);
    }
    return result;
}

bool UpscalingCatalog::isWhitelistedMpvOption(const QString &name)
{
    return name == QStringLiteral("scale")
        || name == QStringLiteral("cscale")
        || name == QStringLiteral("dscale")
        || name == QStringLiteral("sigmoid-upscaling")
        || name == QStringLiteral("correct-downscaling")
        || name == QStringLiteral("linear-downscaling")
        || name == QStringLiteral("scale-antiring")
        || name == QStringLiteral("cscale-antiring");
}

} // namespace YanamiUpscaling
