#include "UpscalingRuntimeConfig.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaType>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantList>

#include <filesystem>
#include <limits>

namespace {

QString filePath(const QString &root, const QString &name)
{
    return QFileInfo(QDir(root).filePath(name)).absoluteFilePath();
}

QString providerFilePath(
    const QString &root,
    const QString &providerId,
    const QString &name)
{
    return filePath(
        root, providerId + QLatin1Char('/') + name);
}

QString animeFilePath(const QString &root, const QString &name)
{
    return providerFilePath(root, QStringLiteral("anime4k"), name);
}

bool createFile(
    const QString &path,
    const QByteArray &contents = QByteArrayLiteral("//!HOOK MAIN\n"))
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

QVariantMap validInput(const QStringList &shaderPaths)
{
    return {
        {QStringLiteral("schema"), 1},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("providerId"), QStringLiteral("anime4k")},
        {QStringLiteral("profileId"), QStringLiteral("anime4k/balanced")},
        {QStringLiteral("modelVersion"), QStringLiteral("4.0.1")},
        {QStringLiteral("orderedShaderPaths"), shaderPaths},
        {QStringLiteral("options"), QVariantMap {
             {QStringLiteral("scale"), QStringLiteral("ewa_lanczossharp")},
             {QStringLiteral("cscale"), QStringLiteral("spline36")},
             {QStringLiteral("sigmoid-upscaling"), true},
             {QStringLiteral("scale-antiring"), 0.35},
         }},
        {QStringLiteral("performanceProtection"), true},
        {QStringLiteral("reservedHeadroomPercent"), 20},
    };
}

std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

} // namespace

class UpscalingRuntimeConfigTests final : public QObject
{
    Q_OBJECT

private slots:
    void emptyAndDisabledInputsAreValid()
    {
        const auto empty = YanamiUpscaling::validateRuntimeConfig({}, {});
        QVERIFY(empty.isValid());
        QVERIFY(!empty.config.enabled);
        QVERIFY(empty.config.providerId.isEmpty());
        QCOMPARE(empty.config.reservedHeadroomPercent,
            YanamiUpscaling::kDefaultReservedHeadroomPercent);

        const QVariantMap disabled {
            {QStringLiteral("schema"), 1},
            {QStringLiteral("enabled"), false},
            {QStringLiteral("providerId"), QStringLiteral("not-trusted")},
        };
        const auto result = YanamiUpscaling::validateRuntimeConfig(disabled, {});
        QVERIFY(result.isValid());
        QVERIFY(!result.config.enabled);
        QCOMPARE(result.config.backendKind,
            YanamiUpscaling::BackendKind::None);
        QVERIFY(result.config.orderedShaderPaths.isEmpty());
    }

    void validInputIsCanonicalizedAndPreservesShaderOrder()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString first = animeFilePath(
            assets.path(), QStringLiteral("a/first.glsl"));
        const QString second = animeFilePath(
            assets.path(), QStringLiteral("b/second.glsl"));
        QVERIFY(createFile(first));
        QVERIFY(createFile(second));

        QVariantMap input = validInput({first, second});
        input.insert(QStringLiteral("orderedShaderPaths"),
            QVariantList {first, second});
        input.insert(QStringLiteral("profileId"), QStringLiteral("anime4k/custom"));
        input.insert(QStringLiteral("modelVersion"), QStringLiteral("4.0.1-20260314"));
        input.insert(QStringLiteral("performanceProtection"), false);
        input.insert(QStringLiteral("reservedHeadroomPercent"), 30);

        const auto result = YanamiUpscaling::validateRuntimeConfig(
            input, assets.path());
        QVERIFY2(result.isValid(), qPrintable(result.errorCode));
        QVERIFY(result.config.enabled);
        QCOMPARE(result.config.providerId, QStringLiteral("anime4k"));
        QCOMPARE(result.config.profileId, QStringLiteral("anime4k/custom"));
        QCOMPARE(result.config.modelVersion,
            QStringLiteral("4.0.1-20260314"));
        QCOMPARE(result.config.backendKind,
            YanamiUpscaling::BackendKind::GlslShaders);
        QCOMPARE(result.config.orderedShaderPaths,
            QStringList({QFileInfo(first).canonicalFilePath(),
                         QFileInfo(second).canonicalFilePath()}));
        QCOMPARE(result.config.whitelistedOptions.size(), 4);
        QCOMPARE(result.config.whitelistedOptions
            .value(QStringLiteral("scale")).toString(),
            QStringLiteral("ewa_lanczossharp"));
        QCOMPARE(result.config.whitelistedOptions
            .value(QStringLiteral("sigmoid-upscaling")).metaType().id(),
            QMetaType::Bool);
        QCOMPARE(result.config.whitelistedOptions
            .value(QStringLiteral("scale-antiring")).toDouble(), 0.35);
        QVERIFY(!result.config.performanceProtection);
        QCOMPARE(result.config.reservedHeadroomPercent, 30);
    }

    void schemaAndEnabledTypesAreStrict()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        QVERIFY(createFile(shader));

        QVariantMap input = validInput({shader});
        input.insert(QStringLiteral("schema"), 2);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("schema_invalid"));

        input.insert(QStringLiteral("schema"), QStringLiteral("1"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("schema_invalid"));

        input = validInput({shader});
        input.remove(QStringLiteral("enabled"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("enabled_invalid"));

        input.insert(QStringLiteral("enabled"), 1);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("enabled_invalid"));

        input = validInput({shader});
        input.insert(QStringLiteral("performanceProtection"), 1);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("performance_protection_invalid"));

        const QVariantMap disabledWithWrongSchema {
            {QStringLiteral("schema"), 7},
            {QStringLiteral("enabled"), false},
        };
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(
                     disabledWithWrongSchema, {}).errorCode,
            QStringLiteral("schema_invalid"));
    }

    void providerAndIdentityFormatsAreRestricted()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        QVERIFY(createFile(shader));

        QVariantMap input = validInput({shader});
        input.insert(QStringLiteral("providerId"), QStringLiteral("unknown"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("provider_not_allowed"));

        input = validInput({shader});
        input.insert(QStringLiteral("providerId"), QStringLiteral("Anime4K"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("provider_not_allowed"));

        input = validInput({shader});
        input.insert(QStringLiteral("profileId"), QStringLiteral("../quality"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("profile_id_invalid"));

        input = validInput({shader});
        input.insert(QStringLiteral("profileId"), QStringLiteral("Quality"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("profile_id_invalid"));

        input = validInput({shader});
        input.insert(QStringLiteral("modelVersion"), QStringLiteral("4..0"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("model_version_invalid"));

        input = validInput({shader});
        input.insert(QStringLiteral("modelVersion"), QStringLiteral("../../4.0.1"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("model_version_invalid"));
    }

    void anime4kShaderBackendIsLegacyCompatibleAndStrict()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        QVERIFY(createFile(shader));

        const auto legacy = YanamiUpscaling::validateRuntimeConfig(
            validInput({shader}), assets.path());
        QVERIFY2(legacy.isValid(), qPrintable(legacy.errorCode));
        QCOMPARE(legacy.config.backendKind,
            YanamiUpscaling::BackendKind::GlslShaders);

        QVariantMap explicitBackend = validInput({shader});
        explicitBackend.insert(QStringLiteral("backend"), QVariantMap {
            {QStringLiteral("kind"), QStringLiteral("glsl-shaders")},
        });
        const auto explicitResult = YanamiUpscaling::validateRuntimeConfig(
            explicitBackend, assets.path());
        QVERIFY2(explicitResult.isValid(),
            qPrintable(explicitResult.errorCode));
        QCOMPARE(explicitResult.config.backendKind,
            YanamiUpscaling::BackendKind::GlslShaders);

        explicitBackend.insert(QStringLiteral("backend"), QVariantMap {
            {QStringLiteral("kind"), QStringLiteral("glsl-shaders")},
            {QStringLiteral("scaleFactor"), 2.0},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(
                     explicitBackend, assets.path()).errorCode,
            QStringLiteral("backend_options_invalid"));
    }

    void nonAnime4kProvidersAreRejectedBeforeBackendOrAssets()
    {
        const QStringList rejectedProviders {
            QStringLiteral("auto"),
            QStringLiteral("artcnn"),
            QStringLiteral("rtx"),
            QStringLiteral("unknown"),
        };
        for (const QString &providerId : rejectedProviders) {
            QVariantMap input = validInput({QStringLiteral("not-used.glsl")});
            input.insert(QStringLiteral("providerId"), providerId);
            QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, {}).errorCode,
                QStringLiteral("provider_not_allowed"));
        }
    }

    void profileBackendAndAssetIdentityMustMatchAnime4k()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        const QString otherProviderShader = providerFilePath(
            assets.path(), QStringLiteral("other"),
            QStringLiteral("compute.glsl"));
        QVERIFY(createFile(shader));
        QVERIFY(createFile(otherProviderShader));

        QVariantMap input = validInput({shader});
        input.insert(QStringLiteral("backend"), QVariantMap {
            {QStringLiteral("kind"), QStringLiteral("d3d11vpp")},
            {QStringLiteral("scaleFactor"), 2.0},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(
                     input, assets.path()).errorCode,
            QStringLiteral("backend_kind_invalid"));

        input = validInput({otherProviderShader});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(
                     input, assets.path()).errorCode,
            QStringLiteral("shader_provider_mismatch"));

        input = validInput({shader});
        input.insert(QStringLiteral("profileId"),
            QStringLiteral("other/balanced"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(
                     input, assets.path()).errorCode,
            QStringLiteral("profile_provider_mismatch"));
    }

    void shaderPathAliasesAreAcceptedButCannotBeAmbiguous()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        QVERIFY(createFile(shader));

        QVariantMap input = validInput({shader});
        input.insert(QStringLiteral("shaderPaths"),
            input.take(QStringLiteral("orderedShaderPaths")));
        auto result = YanamiUpscaling::validateRuntimeConfig(input, assets.path());
        QVERIFY2(result.isValid(), qPrintable(result.errorCode));
        QCOMPARE(result.config.orderedShaderPaths.size(), 1);

        input.insert(QStringLiteral("orderedShaderPaths"), QStringList({shader}));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_paths_ambiguous"));
    }

    void pathShapeExistenceExtensionAndCountAreValidated()
    {
        QTemporaryDir assets;
        QTemporaryDir outside;
        QVERIFY(assets.isValid());
        QVERIFY(outside.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        const QString text = animeFilePath(
            assets.path(), QStringLiteral("model.txt"));
        const QString directory = animeFilePath(
            assets.path(), QStringLiteral("folder.glsl"));
        const QString escaped = filePath(outside.path(), QStringLiteral("escaped.glsl"));
        QVERIFY(createFile(shader));
        QVERIFY(createFile(text));
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(createFile(escaped));

        QVariantMap input = validInput({QStringLiteral("model.glsl")});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_path_not_absolute"));

        input = validInput({animeFilePath(
            assets.path(), QStringLiteral("missing.glsl"))});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_file_missing"));

        input = validInput({text});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_extension_invalid"));

        input = validInput({directory});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_file_not_regular"));

        input = validInput({escaped});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_path_escape"));

        input = validInput({shader, shader});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_path_duplicate"));

        input = validInput({});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_paths_invalid"));

        QStringList tooMany;
        for (int index = 0; index <= YanamiUpscaling::kMaximumShaderCount; ++index)
            tooMany.push_back(shader);
        input = validInput(tooMany);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_paths_invalid"));

        input = validInput({shader});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input,
                     QStringLiteral("relative-assets")).errorCode,
            QStringLiteral("asset_root_invalid"));
    }

    void canonicalPathCannotEscapeThroughSymlink()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString assets = filePath(temporary.path(), QStringLiteral("assets"));
        const QString outside = filePath(temporary.path(), QStringLiteral("outside"));
        QVERIFY(QDir().mkpath(assets));
        QVERIFY(QDir().mkpath(outside));
        const QString target = filePath(outside, QStringLiteral("target.glsl"));
        const QString link = animeFilePath(
            assets, QStringLiteral("linked.glsl"));
        QVERIFY(createFile(target));

        std::error_code error;
        std::filesystem::create_symlink(
            nativePath(target), nativePath(link), error);
        if (error)
            QSKIP("Creating a file symlink is not permitted on this host");

        const QVariantMap input = validInput({link});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets).errorCode,
            QStringLiteral("shader_path_escape"));
    }

    void duplicateCanonicalTargetsAreRejected()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString target = animeFilePath(
            assets.path(), QStringLiteral("target.glsl"));
        const QString link = animeFilePath(
            assets.path(), QStringLiteral("alias.glsl"));
        QVERIFY(createFile(target));

        std::error_code error;
        std::filesystem::create_symlink(
            nativePath(target), nativePath(link), error);
        if (error)
            QSKIP("Creating a file symlink is not permitted on this host");

        const QVariantMap input = validInput({target, link});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(
                     input, assets.path()).errorCode,
            QStringLiteral("shader_path_duplicate"));
    }

    void shaderFileAndAggregateSizesAreBounded()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString oversized = animeFilePath(
            assets.path(), QStringLiteral("oversized.glsl"));
        QVERIFY(QDir().mkpath(QFileInfo(oversized).absolutePath()));
        QFile oversizedFile(oversized);
        QVERIFY(oversizedFile.open(QIODevice::WriteOnly));
        QVERIFY(oversizedFile.resize(
            YanamiUpscaling::kMaximumShaderFileBytes + 1));
        oversizedFile.close();

        QVariantMap input = validInput({oversized});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_file_too_large"));

        QStringList aggregate;
        constexpr qint64 eachSize = 14 * 1024 * 1024;
        for (int index = 0; index < 5; ++index) {
            const QString path = animeFilePath(assets.path(),
                QStringLiteral("aggregate-%1.glsl").arg(index));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize(eachSize));
            file.close();
            aggregate.push_back(path);
        }
        input = validInput(aggregate);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("shader_total_too_large"));
    }

    void mpvOptionsAreStrictlyWhitelisted()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        QVERIFY(createFile(shader));

        QVariantMap input = validInput({shader});
        input.insert(QStringLiteral("options"), QVariantMap {
            {QStringLiteral("glsl-shaders"), shader},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("option_not_allowed"));

        input.insert(QStringLiteral("options"), QVariantMap {
            {QStringLiteral("scale"), QStringLiteral("ewa_lanczos;quit")},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("option_value_invalid"));

        input.insert(QStringLiteral("options"), QVariantMap {
            {QStringLiteral("correct-downscaling"), QStringLiteral("yes")},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("option_value_invalid"));

        input.insert(QStringLiteral("options"), QVariantMap {
            {QStringLiteral("scale-antiring"), QStringLiteral("0.5")},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("option_value_invalid"));

        input.insert(QStringLiteral("options"), QVariantMap {
            {QStringLiteral("cscale-antiring"), 1.01},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("option_value_invalid"));

        input.insert(QStringLiteral("options"), QVariantMap {
            {QStringLiteral("scale-antiring"),
                std::numeric_limits<double>::quiet_NaN()},
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("option_value_invalid"));

        input.insert(QStringLiteral("options"), QVariantList {});
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("options_invalid"));
    }

    void reservedHeadroomUsesCanonicalFieldAndStrictRange()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        QVERIFY(createFile(shader));

        QVariantMap input = validInput({shader});
        input.remove(QStringLiteral("reservedHeadroomPercent"));
        auto result = YanamiUpscaling::validateRuntimeConfig(input, assets.path());
        QVERIFY2(result.isValid(), qPrintable(result.errorCode));
        QCOMPARE(result.config.reservedHeadroomPercent,
            YanamiUpscaling::kDefaultReservedHeadroomPercent);

        input.insert(QStringLiteral("headroom"), 40);
        result = YanamiUpscaling::validateRuntimeConfig(input, assets.path());
        QVERIFY2(result.isValid(), qPrintable(result.errorCode));
        QCOMPARE(result.config.reservedHeadroomPercent, 40);

        input.insert(QStringLiteral("reservedHeadroomPercent"), 10);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("headroom_ambiguous"));

        input.remove(QStringLiteral("headroom"));
        input.insert(QStringLiteral("reservedHeadroomPercent"), 9);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("headroom_invalid"));

        input.insert(QStringLiteral("reservedHeadroomPercent"), 41);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("headroom_invalid"));

        input.insert(QStringLiteral("reservedHeadroomPercent"), 20.5);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("headroom_invalid"));

        input.insert(QStringLiteral("reservedHeadroomPercent"), QStringLiteral("20"));
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("headroom_invalid"));
    }

    void fallbacksRemainOpaqueMapsAndAreStrictlyBounded()
    {
        QTemporaryDir assets;
        QVERIFY(assets.isValid());
        const QString shader = animeFilePath(
            assets.path(), QStringLiteral("model.glsl"));
        QVERIFY(createFile(shader));

        const QVariantMap first {
            {QStringLiteral("schema"), 99},
            {QStringLiteral("providerId"), QStringLiteral("untrusted")},
        };
        const QVariantMap second {
            {QStringLiteral("schema"), 1},
            {QStringLiteral("enabled"), false},
        };
        QVariantMap input = validInput({shader});
        input.insert(QStringLiteral("fallbacks"), QVariantList {
            first,
            QStringLiteral("not-a-map"),
            second,
        });
        auto result = YanamiUpscaling::validateRuntimeConfig(input, assets.path());
        QCOMPARE(result.errorCode, QStringLiteral("fallbacks_invalid"));

        input.insert(QStringLiteral("fallbacks"), QVariantList {first, second});
        result = YanamiUpscaling::validateRuntimeConfig(input, assets.path());
        QVERIFY2(result.isValid(), qPrintable(result.errorCode));
        QCOMPARE(result.config.fallbacks, QList<QVariantMap>({first, second}));

        input.insert(QStringLiteral("fallbacks"), QVariantList {
            first, second, first, second,
        });
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("fallbacks_too_many"));

        input.insert(QStringLiteral("fallbacks"), first);
        QCOMPARE(YanamiUpscaling::validateRuntimeConfig(input, assets.path()).errorCode,
            QStringLiteral("fallbacks_invalid"));
    }
};

QTEST_MAIN(UpscalingRuntimeConfigTests)
#include "UpscalingRuntimeConfigTests.moc"
