#include "UpscalingCatalog.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSet>
#include <QTest>
#include <QUrl>

#include <algorithm>
#include <iterator>

using namespace YanamiUpscaling;

namespace {

QStringList requiredIds(const ResolvedProfile &profile)
{
    QStringList result;
    result.reserve(profile.requiredArtifacts.size());
    for (const ShaderArtifact &artifact : profile.requiredArtifacts) {
        result.append(artifact.id);
    }
    return result;
}

QString sizeSuffix(const QString &size)
{
    return size.toLower();
}

void verifyCompleteProfile(const ResolvedProfile &profile)
{
    QCOMPARE(profile.providerId, QStringLiteral("anime4k"));
    QCOMPARE(profile.backendKind, BackendKind::GlslShaders);
    QVERIFY(!profile.profileId.isEmpty());
    QVERIFY(!profile.version.isEmpty());
    QVERIFY(!profile.assetSetId.isEmpty());
    QVERIFY(!profile.orderedShaderArtifactIds.isEmpty());

    QSet<QString> unique;
    for (const QString &id : profile.orderedShaderArtifactIds) {
        QVERIFY2(UpscalingCatalog::findArtifact(id) != nullptr, qPrintable(id));
        unique.insert(id);
    }
    QCOMPARE(profile.requiredArtifacts.size(), unique.size());
    for (const ShaderArtifact &artifact : profile.requiredArtifacts) {
        QVERIFY(unique.contains(artifact.id));
        QCOMPARE(artifact.providerId, profile.providerId);
        QCOMPARE(artifact.version, profile.version);
        QCOMPARE(artifact.assetSetId, profile.assetSetId);
    }
}

} // namespace

class UpscalingCatalogTests final : public QObject {
    Q_OBJECT

private slots:
    void stableIds();
    void backendIdsRoundTrip();
    void artifactMetadataIsPinnedAndSafe();
    void anime4kAssetsAreExact();
    void anime4kCommitAssetSetIsComplete();
    void standardProfileOrderAndResources();
    void customAnime4kCoversAllModesAndSizes();
    void customAnime4kSecondPassOrder();
    void invalidCustomFallsBackToDocumentedDefaults();
    void unknownProviderFailsClosed();
    void mpvOptionsAndFallbacksAreClosed();
    void deploymentArtifactsContainEveryRuntimeFallback();
    void resolveAverageStaysBelowBudget();
};

void UpscalingCatalogTests::stableIds()
{
    QCOMPARE(
        UpscalingCatalog::providerIds(),
        QStringList({QStringLiteral("anime4k")}));
    QCOMPARE(
        UpscalingCatalog::presetIds(),
        QStringList({
            QStringLiteral("performance"),
            QStringLiteral("balanced"),
            QStringLiteral("quality"),
            QStringLiteral("custom")}));
}

void UpscalingCatalogTests::backendIdsRoundTrip()
{
    QCOMPARE(
        UpscalingCatalog::backendKindId(BackendKind::None), QString());
    QCOMPARE(
        UpscalingCatalog::backendKindId(BackendKind::GlslShaders),
        QStringLiteral("glsl-shaders"));

    BackendKind parsed = BackendKind::None;
    QVERIFY(UpscalingCatalog::parseBackendKindId(
        QStringLiteral("glsl-shaders"), &parsed));
    QCOMPARE(parsed, BackendKind::GlslShaders);
    QVERIFY(!UpscalingCatalog::parseBackendKindId(
        QStringLiteral("gpu-next"), &parsed));
    QVERIFY(!UpscalingCatalog::parseBackendKindId(
        QStringLiteral("d3d11vpp"), &parsed));
    QVERIFY(!UpscalingCatalog::parseBackendKindId(
        QStringLiteral("glsl-shaders-extra"), &parsed));
    QVERIFY(!UpscalingCatalog::parseBackendKindId(
        QStringLiteral("glsl-shaders"), nullptr));
}

void UpscalingCatalogTests::artifactMetadataIsPinnedAndSafe()
{
    const auto &artifacts = UpscalingCatalog::artifacts();
    QCOMPARE(artifacts.size(), 23);

    const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}$"));
    const QSet<QString> allowedHosts = {
        QStringLiteral("raw.githubusercontent.com")};
    QSet<QString> ids;
    QSet<QString> installPaths;

    for (const ShaderArtifact &artifact : artifacts) {
        QVERIFY2(!ids.contains(artifact.id), qPrintable(artifact.id));
        ids.insert(artifact.id);
        QVERIFY(!artifact.id.isEmpty());
        QCOMPARE(artifact.providerId, QStringLiteral("anime4k"));
        QVERIFY(artifact.sizeBytes > 0);
        QVERIFY2(shaPattern.match(artifact.sha256).hasMatch(), qPrintable(artifact.sha256));
        QCOMPARE(artifact.licenseSpdx, QStringLiteral("MIT"));

        const QUrl url(artifact.downloadUrl);
        QVERIFY2(url.isValid(), qPrintable(artifact.downloadUrl));
        QCOMPARE(url.scheme(), QStringLiteral("https"));
        QVERIFY2(allowedHosts.contains(url.host()), qPrintable(url.host()));
        QVERIFY(url.userInfo().isEmpty());

        const QUrl licenseUrl(artifact.licenseUrl);
        QVERIFY(licenseUrl.isValid());
        QCOMPARE(licenseUrl.scheme(), QStringLiteral("https"));
        QCOMPARE(licenseUrl.host(), QStringLiteral("github.com"));

        QVERIFY(!QDir::isAbsolutePath(artifact.installRelativePath));
        QVERIFY(!artifact.installRelativePath.contains(QLatin1Char('\\')));
        const QString cleanPath = QDir::cleanPath(artifact.installRelativePath);
        QCOMPARE(cleanPath, artifact.installRelativePath);
        QVERIFY(cleanPath != QStringLiteral(".."));
        QVERIFY(!cleanPath.startsWith(QStringLiteral("../")));
        QVERIFY(!cleanPath.contains(QStringLiteral("/../")));
        QVERIFY2(!installPaths.contains(cleanPath), qPrintable(cleanPath));
        installPaths.insert(cleanPath);
        QCOMPARE(cleanPath.section(QLatin1Char('/'), -1), artifact.fileName);

        QCOMPARE(UpscalingCatalog::findArtifact(artifact.id), &artifact);
    }
    QCOMPARE(UpscalingCatalog::findArtifact(QStringLiteral("missing")), nullptr);
}

void UpscalingCatalogTests::anime4kAssetsAreExact()
{
    struct Expected {
        const char *id;
        const char *fileName;
        qint64 sizeBytes;
        const char *sha256;
    };
    const Expected expected[] = {
        {"anime4k.clamp", "Anime4K_Clamp_Highlights.glsl", 2795,
         "a2a9bf7fbc1d75d09660ca2e701e4d7fb0cf5457b94da47e1825032fa2b3671a"},
        {"anime4k.autodown.x2", "Anime4K_AutoDownscalePre_x2.glsl", 1560,
         "8c58291740146bd766a4d73f132775a797fe80f7d07919b5d767e27a5dc85656"},
        {"anime4k.autodown.x4", "Anime4K_AutoDownscalePre_x4.glsl", 1568,
         "5af62d8cd844916dc1126613e13bad3beab195787f93a71200b47c6ec78f2e41"},
        {"anime4k.restore.a.s", "Anime4K_Restore_CNN_S.glsl", 17136,
         "97c24dc370ab300c108bfaa09db7f175aeff343674842c299cf3940a3d330427"},
        {"anime4k.restore.a.m", "Anime4K_Restore_CNN_M.glsl", 35916,
         "67ea3ed26539e8de3b7d307688535d2ff17e8d147e11dda0247da7770dbecf41"},
        {"anime4k.restore.a.l", "Anime4K_Restore_CNN_L.glsl", 69921,
         "d6efe215e6ee8af1ec560478a91afc1df83fac4ba43b2c806ee61ca2267ed674"},
        {"anime4k.restore.a.vl", "Anime4K_Restore_CNN_VL.glsl", 144075,
         "35036722733305cd4d4e57660b883bbe2569ba2914033c254327107d7b77e35e"},
        {"anime4k.restore.a.ul", "Anime4K_Restore_CNN_UL.glsl", 308660,
         "81ec48ff700108c8e1571ed82d4527006ee200c05e94f8b7f836c02179169b40"},
        {"anime4k.restore.b.s", "Anime4K_Restore_CNN_Soft_S.glsl", 17198,
         "9f6867f2ef42786729522d86fe24147cb4ea145418e3974df70496acf52dc392"},
        {"anime4k.restore.b.m", "Anime4K_Restore_CNN_Soft_M.glsl", 36016,
         "a78a2c76898e08e09e442a9628c64208c26e8e15789649b8755223f009794c02"},
        {"anime4k.restore.b.l", "Anime4K_Restore_CNN_Soft_L.glsl", 70020,
         "8f2a5c73b526c6e4c67bce0366f7dcd7410bfa4a31e718240d86f53660788e63"},
        {"anime4k.restore.b.vl", "Anime4K_Restore_CNN_Soft_VL.glsl", 144204,
         "094334b0e20c1a201fe4941c7c68de72451e5aee9efb5524d7fb82b12dca64b9"},
        {"anime4k.restore.b.ul", "Anime4K_Restore_CNN_Soft_UL.glsl", 308873,
         "41d71cad7b2f3af9086852fca70046f763272d2cb49dd83de9647d942866cac8"},
        {"anime4k.upscale.s", "Anime4K_Upscale_CNN_x2_S.glsl", 18638,
         "4c53ec2e287908f7ee7bcb266b0170421626d663576468b7d7dafc62962649a4"},
        {"anime4k.upscale.m", "Anime4K_Upscale_CNN_x2_M.glsl", 37685,
         "716e02098a68f0d648761f2b96b4dd139e1cb09b174bb369fca3aa34328fff7e"},
        {"anime4k.upscale.l", "Anime4K_Upscale_CNN_x2_L.glsl", 73443,
         "db1fedf7be82f6fd9034e6bf39b64daf2b7576988bb584ec38f24f5236b1cd97"},
        {"anime4k.upscale.vl", "Anime4K_Upscale_CNN_x2_VL.glsl", 146743,
         "5638fe31c37c151a3443fea3451a3ef91af073f4dbb9615f6c0d1e29db11493d"},
        {"anime4k.upscale.ul", "Anime4K_Upscale_CNN_x2_UL.glsl", 290257,
         "fa7cf0ecc1cca84d8291bbff5a42b60f5816d57b4e97d42bc377235ac8db02e8"},
        {"anime4k.upscale-denoise.s", "Anime4K_Upscale_Denoise_CNN_x2_S.glsl", 18667,
         "1a45ad3aa20d8368399f2fd46791deed957c2fd0a4afd131cc92516827abab93"},
        {"anime4k.upscale-denoise.m", "Anime4K_Upscale_Denoise_CNN_x2_M.glsl", 37714,
         "8c72b042e2301fe66a45c3089720459148e2504cd72af16f9c0d5017ff14181e"},
        {"anime4k.upscale-denoise.l", "Anime4K_Upscale_Denoise_CNN_x2_L.glsl", 73584,
         "6cc4604c9544fd4fd9e3a75fd797511bf5fe1e626e9e1dbcee03302823a63207"},
        {"anime4k.upscale-denoise.vl", "Anime4K_Upscale_Denoise_CNN_x2_VL.glsl", 146811,
         "359c48fe5a317fbc6b706ce368401eef496e84ed98abac7a43efebca2b65d79b"},
        {"anime4k.upscale-denoise.ul", "Anime4K_Upscale_Denoise_CNN_x2_UL.glsl", 290040,
         "38bd11e7e92ff1a615a274be912d6f2cfe67f5b43771f0383a91c6ab3d0f8cb6"}};

    QCOMPARE(std::size(expected), std::size_t(23));

    for (const Expected &item : expected) {
        const ShaderArtifact *artifact =
            UpscalingCatalog::findArtifact(QString::fromLatin1(item.id));
        QVERIFY(artifact != nullptr);
        QCOMPARE(artifact->providerId, QStringLiteral("anime4k"));
        QCOMPARE(artifact->version, QStringLiteral("v4.0.1"));
        QCOMPARE(
            artifact->assetSetId,
            QStringLiteral("anime4k-v4.0.1-4029bf701ecaa15f163cdc49cffe5501c1acf410-glsl"));
        QCOMPARE(artifact->fileName, QString::fromLatin1(item.fileName));
        QCOMPARE(artifact->sizeBytes, item.sizeBytes);
        QCOMPARE(artifact->sha256, QString::fromLatin1(item.sha256));
        QCOMPARE(
            artifact->installRelativePath,
            QStringLiteral("anime4k/v4.0.1/")
                + QString::fromLatin1(item.fileName));
        QVERIFY(artifact->downloadUrl.endsWith(
            QLatin1Char('/') + QString::fromLatin1(item.fileName)));
    }
}

void UpscalingCatalogTests::anime4kCommitAssetSetIsComplete()
{
    constexpr auto commit = "4029bf701ecaa15f163cdc49cffe5501c1acf410";
    const QString rawPrefix = QStringLiteral("https://raw.githubusercontent.com/bloc97/Anime4K/")
        + QString::fromLatin1(commit) + QLatin1Char('/');
    int count = 0;
    for (const ShaderArtifact &artifact : UpscalingCatalog::artifacts()) {
        if (artifact.providerId != QStringLiteral("anime4k")) {
            continue;
        }
        ++count;
        QCOMPARE(artifact.version, QStringLiteral("v4.0.1"));
        QCOMPARE(
            artifact.assetSetId,
            QStringLiteral("anime4k-v4.0.1-4029bf701ecaa15f163cdc49cffe5501c1acf410-glsl"));
        QVERIFY2(artifact.downloadUrl.startsWith(rawPrefix), qPrintable(artifact.downloadUrl));
    }
    QCOMPARE(count, 23);

    for (const QString &size : {
             QStringLiteral("s"),
             QStringLiteral("m"),
             QStringLiteral("l"),
             QStringLiteral("vl"),
             QStringLiteral("ul")}) {
        QVERIFY(UpscalingCatalog::findArtifact(QStringLiteral("anime4k.restore.a.") + size));
        QVERIFY(UpscalingCatalog::findArtifact(QStringLiteral("anime4k.restore.b.") + size));
        QVERIFY(UpscalingCatalog::findArtifact(QStringLiteral("anime4k.upscale.") + size));
        QVERIFY(UpscalingCatalog::findArtifact(QStringLiteral("anime4k.upscale-denoise.") + size));
    }
    QVERIFY(UpscalingCatalog::findArtifact(QStringLiteral("anime4k.clamp")));
    QVERIFY(UpscalingCatalog::findArtifact(QStringLiteral("anime4k.autodown.x2")));
    QVERIFY(UpscalingCatalog::findArtifact(QStringLiteral("anime4k.autodown.x4")));
}

void UpscalingCatalogTests::standardProfileOrderAndResources()
{
    struct Case {
        QString preset;
        QStringList chain;
    };
    const QVector<Case> cases = {
        {QStringLiteral("performance"),
         {QStringLiteral("anime4k.clamp"),
          QStringLiteral("anime4k.restore.a.m"),
          QStringLiteral("anime4k.upscale.m"),
          QStringLiteral("anime4k.autodown.x2"),
          QStringLiteral("anime4k.autodown.x4"),
          QStringLiteral("anime4k.upscale.s")}},
        {QStringLiteral("balanced"),
         {QStringLiteral("anime4k.clamp"),
          QStringLiteral("anime4k.restore.a.vl"),
          QStringLiteral("anime4k.upscale.vl"),
          QStringLiteral("anime4k.autodown.x2"),
          QStringLiteral("anime4k.autodown.x4"),
          QStringLiteral("anime4k.upscale.m")}},
        {QStringLiteral("quality"),
         {QStringLiteral("anime4k.clamp"),
          QStringLiteral("anime4k.restore.a.vl"),
          QStringLiteral("anime4k.upscale.vl"),
          QStringLiteral("anime4k.autodown.x2"),
          QStringLiteral("anime4k.autodown.x4"),
          QStringLiteral("anime4k.restore.a.m"),
          QStringLiteral("anime4k.upscale.m")}}};

    for (const Case &item : cases) {
        const ResolvedProfile profile = UpscalingCatalog::resolve(
            QStringLiteral("anime4k"), item.preset);
        QCOMPARE(profile.providerId, QStringLiteral("anime4k"));
        QCOMPARE(profile.presetId, item.preset);
        QCOMPARE(
            profile.profileId,
            QStringLiteral("anime4k/") + item.preset);
        QCOMPARE(profile.orderedShaderArtifactIds, item.chain);
        QCOMPARE(requiredIds(profile), item.chain);
        verifyCompleteProfile(profile);
    }
}

void UpscalingCatalogTests::customAnime4kCoversAllModesAndSizes()
{
    const QStringList modes = {
        QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")};
    const QStringList sizes = {
        QStringLiteral("S"),
        QStringLiteral("M"),
        QStringLiteral("L"),
        QStringLiteral("VL"),
        QStringLiteral("UL")};

    for (const QString &mode : modes) {
        for (const QString &size : sizes) {
            const QString secondarySize = size == QStringLiteral("S")
                ? QStringLiteral("M") : QStringLiteral("S");
            const QVariantMap custom = {
                {QStringLiteral("mode"), mode},
                {QStringLiteral("primarySize"), size},
                {QStringLiteral("secondarySize"), secondarySize},
                {QStringLiteral("passes"), 1}};
            const ResolvedProfile profile = UpscalingCatalog::resolve(
                QStringLiteral("anime4k"), QStringLiteral("custom"), custom);
            QVERIFY(!profile.usedDefaultCustom);
            QVariantMap normalized = custom;
            normalized.insert(QStringLiteral("autoDownscale"), true);
            QCOMPARE(profile.normalizedCustom, normalized);
            QCOMPARE(profile.orderedShaderArtifactIds.first(), QStringLiteral("anime4k.clamp"));
            const QString suffix = sizeSuffix(size);
            if (mode == QStringLiteral("C")) {
                QVERIFY(profile.orderedShaderArtifactIds.contains(
                    QStringLiteral("anime4k.upscale-denoise.") + suffix));
            } else {
                const QString restoreFamily = mode == QStringLiteral("A")
                    ? QStringLiteral("a")
                    : QStringLiteral("b");
                QVERIFY(profile.orderedShaderArtifactIds.contains(
                    QStringLiteral("anime4k.restore.") + restoreFamily + QLatin1Char('.') + suffix));
                QVERIFY(profile.orderedShaderArtifactIds.contains(
                    QStringLiteral("anime4k.upscale.") + suffix));
            }
            QCOMPARE(
                profile.orderedShaderArtifactIds.last(),
                QStringLiteral("anime4k.upscale.") + sizeSuffix(secondarySize));
            verifyCompleteProfile(profile);
        }
    }
}

void UpscalingCatalogTests::customAnime4kSecondPassOrder()
{
    const ResolvedProfile modeA = UpscalingCatalog::resolve(
        QStringLiteral("anime4k"),
        QStringLiteral("custom"),
        {{QStringLiteral("mode"), QStringLiteral("A")},
         {QStringLiteral("primarySize"), QStringLiteral("M")},
         {QStringLiteral("secondarySize"), QStringLiteral("S")},
         {QStringLiteral("passes"), 2}});
    QCOMPARE(
        modeA.orderedShaderArtifactIds,
        QStringList({
            QStringLiteral("anime4k.clamp"),
            QStringLiteral("anime4k.restore.a.m"),
            QStringLiteral("anime4k.upscale.m"),
            QStringLiteral("anime4k.autodown.x2"),
            QStringLiteral("anime4k.autodown.x4"),
            QStringLiteral("anime4k.restore.a.s"),
            QStringLiteral("anime4k.upscale.s")}));

    const ResolvedProfile modeB = UpscalingCatalog::resolve(
        QStringLiteral("anime4k"),
        QStringLiteral("custom"),
        {{QStringLiteral("mode"), QStringLiteral("B")},
         {QStringLiteral("primarySize"), QStringLiteral("L")},
         {QStringLiteral("secondarySize"), QStringLiteral("S")},
         {QStringLiteral("passes"), 2}});
    QCOMPARE(modeB.orderedShaderArtifactIds.at(1), QStringLiteral("anime4k.restore.b.l"));
    QCOMPARE(modeB.orderedShaderArtifactIds.at(5), QStringLiteral("anime4k.restore.b.s"));

    const ResolvedProfile modeC = UpscalingCatalog::resolve(
        QStringLiteral("anime4k"),
        QStringLiteral("custom"),
        {{QStringLiteral("mode"), QStringLiteral("C")},
         {QStringLiteral("primarySize"), QStringLiteral("VL")},
         {QStringLiteral("secondarySize"), QStringLiteral("M")},
         {QStringLiteral("passes"), 2}});
    QCOMPARE(modeC.orderedShaderArtifactIds.at(1), QStringLiteral("anime4k.upscale-denoise.vl"));
    QCOMPARE(modeC.orderedShaderArtifactIds.at(4), QStringLiteral("anime4k.restore.a.m"));
    QCOMPARE(modeC.orderedShaderArtifactIds.at(5), QStringLiteral("anime4k.upscale.m"));
    verifyCompleteProfile(modeA);
    verifyCompleteProfile(modeB);
    verifyCompleteProfile(modeC);
}

void UpscalingCatalogTests::invalidCustomFallsBackToDocumentedDefaults()
{
    const ResolvedProfile defaultAnime = UpscalingCatalog::resolve(
        QStringLiteral("anime4k"), QStringLiteral("custom"));
    const QVector<QVariantMap> invalidAnime = {
        {{QStringLiteral("mode"), QStringLiteral("D")}},
        {{QStringLiteral("primarySize"), QStringLiteral("XL")}},
        {{QStringLiteral("passes"), 0}},
        {{QStringLiteral("passes"), 3}},
        {{QStringLiteral("passes"), QStringLiteral("2")}},
        {{QStringLiteral("passes"), QVariant::fromValue<qint64>(2)}},
        {{QStringLiteral("mode"), QStringLiteral("A")},
         {QStringLiteral("primarySize"), QStringLiteral("M")},
         {QStringLiteral("secondarySize"), QStringLiteral("M")},
         {QStringLiteral("passes"), 2}},
        {{QStringLiteral("extra"), true}}};
    for (const QVariantMap &custom : invalidAnime) {
        const ResolvedProfile profile = UpscalingCatalog::resolve(
            QStringLiteral("anime4k"), QStringLiteral("custom"), custom);
        QVERIFY(profile.usedDefaultCustom);
        QCOMPARE(profile.normalizedCustom, defaultAnime.normalizedCustom);
        QCOMPARE(profile.orderedShaderArtifactIds, defaultAnime.orderedShaderArtifactIds);
    }
}

void UpscalingCatalogTests::unknownProviderFailsClosed()
{
    for (const QString &providerId : {
             QStringLiteral("auto"),
             QStringLiteral("artcnn"),
             QStringLiteral("rtx"),
             QStringLiteral("unknown"),
             QStringLiteral("Anime4K")}) {
        const ResolvedProfile profile = UpscalingCatalog::resolve(
            providerId, QStringLiteral("quality"));
        QCOMPARE(profile.requestedProviderId, providerId);
        QCOMPARE(profile.requestedPresetId, QStringLiteral("quality"));
        QVERIFY(profile.usedDefaultProvider);
        QVERIFY(profile.providerId.isEmpty());
        QVERIFY(profile.presetId.isEmpty());
        QVERIFY(profile.profileId.isEmpty());
        QVERIFY(profile.version.isEmpty());
        QVERIFY(profile.assetSetId.isEmpty());
        QCOMPARE(profile.backendKind, BackendKind::None);
        QVERIFY(profile.orderedShaderArtifactIds.isEmpty());
        QVERIFY(profile.requiredArtifacts.isEmpty());
        QVERIFY(profile.mpvOptions.isEmpty());
        QVERIFY(profile.fallbackProfileIds.isEmpty());
        QVERIFY(profile.normalizedCustom.isEmpty());
    }
}

void UpscalingCatalogTests::mpvOptionsAndFallbacksAreClosed()
{
    const QString provider = QStringLiteral("anime4k");
    const ResolvedProfile performance =
        UpscalingCatalog::resolve(provider, QStringLiteral("performance"));
    const ResolvedProfile balanced =
        UpscalingCatalog::resolve(provider, QStringLiteral("balanced"));
    const ResolvedProfile quality =
        UpscalingCatalog::resolve(provider, QStringLiteral("quality"));
    const ResolvedProfile custom =
        UpscalingCatalog::resolve(provider, QStringLiteral("custom"));

    for (const ResolvedProfile *profile : {
             &performance, &balanced, &quality, &custom}) {
        for (auto it = profile->mpvOptions.cbegin();
             it != profile->mpvOptions.cend(); ++it) {
            QVERIFY(UpscalingCatalog::isWhitelistedMpvOption(it.key()));
        }
        QVERIFY(!profile->mpvOptions.contains(QStringLiteral("vo")));
    }
    QVERIFY(performance.mpvOptions.isEmpty());
    QVERIFY(balanced.mpvOptions.isEmpty());
    QVERIFY(quality.mpvOptions.isEmpty());
    QCOMPARE(
        performance.fallbackProfileIds,
        QStringList({QStringLiteral("original")}));
    QCOMPARE(
        balanced.fallbackProfileIds,
        QStringList({
            QStringLiteral("anime4k/performance"),
            QStringLiteral("original")}));
    QCOMPARE(
        quality.fallbackProfileIds,
        QStringList({
            QStringLiteral("anime4k/balanced"),
            QStringLiteral("anime4k/performance"),
            QStringLiteral("original")}));
    QCOMPARE(
        custom.fallbackProfileIds,
        QStringList({
            QStringLiteral("anime4k/performance"),
            QStringLiteral("original")}));
    QVERIFY(!UpscalingCatalog::isWhitelistedMpvOption(QStringLiteral("script-opts")));
    QVERIFY(!UpscalingCatalog::isWhitelistedMpvOption(QStringLiteral("input-conf")));
    QVERIFY(!UpscalingCatalog::isWhitelistedMpvOption(QStringLiteral("glsl-shaders")));
    QVERIFY(!UpscalingCatalog::isWhitelistedMpvOption(QStringLiteral("vo")));
}

void UpscalingCatalogTests::deploymentArtifactsContainEveryRuntimeFallback()
{
    const QString provider = QStringLiteral("anime4k");
    for (const QString &preset : {
             QStringLiteral("performance"),
             QStringLiteral("balanced"),
             QStringLiteral("quality"),
             QStringLiteral("custom")}) {
        const ResolvedProfile profile = UpscalingCatalog::resolve(
            provider, preset);
        const QVector<ShaderArtifact> deployment =
            UpscalingCatalog::deploymentArtifacts(profile);
        QSet<QString> deployedIds;
        for (const ShaderArtifact &artifact : deployment) {
            QVERIFY(!deployedIds.contains(artifact.id));
            deployedIds.insert(artifact.id);
        }
        for (const ShaderArtifact &artifact : profile.requiredArtifacts)
            QVERIFY(deployedIds.contains(artifact.id));
        for (const QString &fallbackId : profile.fallbackProfileIds) {
            if (fallbackId == QLatin1String("original"))
                continue;
            const QStringList parts = fallbackId.split(QLatin1Char('/'));
            QCOMPARE(parts.size(), 2);
            const ResolvedProfile fallback = UpscalingCatalog::resolve(
                parts.at(0), parts.at(1));
            for (const ShaderArtifact &artifact : fallback.requiredArtifacts)
                QVERIFY(deployedIds.contains(artifact.id));
        }
    }

    const ResolvedProfile quality = UpscalingCatalog::resolve(
        provider, QStringLiteral("quality"));
    const QVector<ShaderArtifact> qualityDeployment =
        UpscalingCatalog::deploymentArtifacts(quality);
    QVERIFY(std::any_of(
        qualityDeployment.cbegin(),
        qualityDeployment.cend(),
        [](const ShaderArtifact &artifact) {
            return artifact.id == QLatin1String("anime4k.upscale.s");
        }));
}

void UpscalingCatalogTests::resolveAverageStaysBelowBudget()
{
    const QVariantMap custom = {
        {QStringLiteral("mode"), QStringLiteral("C")},
        {QStringLiteral("primarySize"), QStringLiteral("VL")},
        {QStringLiteral("secondarySize"), QStringLiteral("M")},
        {QStringLiteral("passes"), 2}};
    constexpr int iterations = 10000;
    qint64 checksum = 0;

    // Warm static catalog/index initialization outside the measured region.
    (void) UpscalingCatalog::resolve(
        QStringLiteral("anime4k"), QStringLiteral("custom"), custom);
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < iterations; ++i) {
        const ResolvedProfile profile = (i & 1)
            ? UpscalingCatalog::resolve(
                QStringLiteral("anime4k"), QStringLiteral("custom"), custom)
            : UpscalingCatalog::resolve(
                QStringLiteral("anime4k"), QStringLiteral("quality"));
        checksum += profile.requiredArtifacts.size();
    }
    const double averageMilliseconds =
        (static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0) / iterations;
    QVERIFY(checksum > 0);
    QVERIFY2(
        averageMilliseconds < 0.25,
        qPrintable(QStringLiteral("average resolve time was %1 ms").arg(averageMilliseconds, 0, 'f', 6)));
}

QTEST_APPLESS_MAIN(UpscalingCatalogTests)

#include "UpscalingCatalogTests.moc"
