#include "UpscalingCapabilityProbe.hpp"

#include <QTest>
#include <QVariantList>

namespace {

QVariantMap anime4kProvider(const QVariantMap &result)
{
    const QVariantList providers = result.value(
        QStringLiteral("providers")).toList();
    for (const QVariant &entry : providers) {
        const QVariantMap provider = entry.toMap();
        if (provider.value(QStringLiteral("id")).toString()
            == QStringLiteral("anime4k")) {
            return provider;
        }
    }
    return {};
}

QVariantMap evaluate(
    QSGRendererInterface::GraphicsApi api,
    int glMajor = 4,
    int glMinor = 6,
    const QString &vendor = QStringLiteral("NVIDIA Corporation"),
    const QString &renderer = QStringLiteral("NVIDIA GeForce RTX"),
    int maximumTextureSize = 32'768)
{
    return UpscalingCapabilityProbe::evaluate(
        api, glMajor, glMinor, vendor, renderer, maximumTextureSize);
}

void verifyRecommendationMetadataAbsent(const QVariantMap &result)
{
    const QStringList removedKeys {
        QStringLiteral("adapter"),
        QStringLiteral("hardwareClass"),
        QStringLiteral("recommendationConfidence"),
        QStringLiteral("maximumRecommendedPreset"),
        QStringLiteral("classificationReason"),
        QStringLiteral("recommendationSummary"),
        QStringLiteral("matchedCapabilityEntryId"),
        QStringLiteral("capabilityTableSchemaVersion"),
        QStringLiteral("capabilityTableRevision"),
        QStringLiteral("referenceScenario"),
        QStringLiteral("recommendedProviderId"),
        QStringLiteral("recommendedPresetId"),
    };
    for (const QString &key : removedKeys)
        QVERIFY2(!result.contains(key), qPrintable(key));
    QVERIFY(!anime4kProvider(result).contains(QStringLiteral("recommended")));
}

} // namespace

class UpscalingCapabilityProbeTests final : public QObject
{
    Q_OBJECT

private slots:
    void openGlVersionBoundary()
    {
        QVariantMap result = evaluate(
            QSGRendererInterface::OpenGL, 4, 2);
        QVERIFY(!anime4kProvider(result)
                     .value(QStringLiteral("supported")).toBool());
        QVERIFY(anime4kProvider(result)
                    .value(QStringLiteral("unavailableReason"))
                    .toString()
                    .contains(QStringLiteral("4.3")));

        result = evaluate(QSGRendererInterface::OpenGL, 4, 3);
        QVERIFY(anime4kProvider(result)
                    .value(QStringLiteral("supported")).toBool());
        QVERIFY(anime4kProvider(result)
                    .value(QStringLiteral("unavailableReason"))
                    .toString().isEmpty());
        verifyRecommendationMetadataAbsent(result);
    }

    void textureSizeBoundary()
    {
        QVariantMap result = evaluate(
            QSGRendererInterface::OpenGL,
            4,
            6,
            QStringLiteral("Intel"),
            QStringLiteral("Intel Arc"),
            4095);
        QVERIFY(!anime4kProvider(result)
                     .value(QStringLiteral("supported")).toBool());
        QVERIFY(anime4kProvider(result)
                    .value(QStringLiteral("unavailableReason"))
                    .toString()
                    .contains(QStringLiteral("4096")));

        result = evaluate(
            QSGRendererInterface::OpenGL,
            4,
            3,
            QStringLiteral("Intel"),
            QStringLiteral("Intel Arc"),
            4096);
        QVERIFY(anime4kProvider(result)
                    .value(QStringLiteral("supported")).toBool());
    }

    void softwareRenderingIsRejected()
    {
        QVariantMap result = evaluate(QSGRendererInterface::Software);
        QVERIFY(result.value(QStringLiteral("softwareRenderer")).toBool());
        QVERIFY(!anime4kProvider(result)
                     .value(QStringLiteral("supported")).toBool());

        result = evaluate(
            QSGRendererInterface::OpenGL,
            4,
            6,
            QStringLiteral("Mesa"),
            QStringLiteral("llvmpipe (LLVM 18.1)"));
        QVERIFY(result.value(QStringLiteral("softwareRenderer")).toBool());
        QVERIFY(!anime4kProvider(result)
                     .value(QStringLiteral("supported")).toBool());
    }

    void nonOpenGlBackendsAreRejected_data()
    {
        QTest::addColumn<int>("api");
        QTest::newRow("d3d11")
            << static_cast<int>(QSGRendererInterface::Direct3D11);
        QTest::newRow("vulkan")
            << static_cast<int>(QSGRendererInterface::Vulkan);
        QTest::newRow("metal")
            << static_cast<int>(QSGRendererInterface::Metal);
    }

    void nonOpenGlBackendsAreRejected()
    {
        QFETCH(int, api);
        const QVariantMap result = evaluate(
            static_cast<QSGRendererInterface::GraphicsApi>(api));
        QVERIFY(!anime4kProvider(result)
                     .value(QStringLiteral("supported")).toBool());
        QVERIFY(anime4kProvider(result)
                    .value(QStringLiteral("unavailableReason"))
                    .toString()
                    .contains(QStringLiteral("OpenGL")));
    }

    void resultIsNormalizedAndSupportOnly()
    {
        const QVariantMap result = UpscalingCapabilityProbe::evaluate(
            QSGRendererInterface::OpenGL,
            -4,
            -3,
            QStringLiteral("  Vendor  "),
            QStringLiteral("  Renderer  "),
            -1);

        QCOMPARE(result.value(QStringLiteral("glMajor")).toInt(), 0);
        QCOMPARE(result.value(QStringLiteral("glMinor")).toInt(), 0);
        QCOMPARE(result.value(QStringLiteral("maximumTextureSize")).toInt(), 0);
        QCOMPARE(result.value(QStringLiteral("vendor")).toString(),
                 QStringLiteral("Vendor"));
        QCOMPARE(result.value(QStringLiteral("renderer")).toString(),
                 QStringLiteral("Renderer"));
        verifyRecommendationMetadataAbsent(result);
    }
};

QTEST_APPLESS_MAIN(UpscalingCapabilityProbeTests)

#include "UpscalingCapabilityProbeTests.moc"
