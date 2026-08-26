#include "UpscalingCapabilityProbe.hpp"
#include "UpscalingCatalog.hpp"
#include "UpscalingPerformancePolicy.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QUrl>
#include <QUuid>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <optional>

using YanamiUpscaling::HealthWindow;
using YanamiUpscaling::PerformanceProtection;
using YanamiUpscaling::ResolvedProfile;
using YanamiUpscaling::ShaderArtifact;
using YanamiUpscaling::UpscalingCatalog;

namespace {

constexpr auto fixtureId = "UpscalingCapability-v1";
constexpr auto schemaVersion = "1.0";
constexpr int capabilityIterations = 100;
// Keep at least 1,000 samples after the production catalog was reduced to the
// three Anime4K presets. Four hundred iterations provide 1,200 observations,
// preserving the PullRequest SLO's minimum-sample contract.
constexpr int catalogIterations = 400;

struct JsonFile
{
    QByteArray bytes;
    QJsonObject object;
};

void appendFailure(QStringList *failures, const QString &message)
{
    if (!failures->contains(message))
        failures->append(message);
}

std::optional<JsonFile> readJsonObject(
    const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorMessage = QStringLiteral("Cannot read %1: %2")
                            .arg(QDir::toNativeSeparators(path), file.errorString());
        return std::nullopt;
    }

    JsonFile result;
    result.bytes = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        result.bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *errorMessage = QStringLiteral("Invalid JSON object in %1: %2")
                            .arg(
                                QDir::toNativeSeparators(path),
                                parseError.errorString());
        return std::nullopt;
    }
    result.object = document.object();
    return result;
}

QStringList jsonStrings(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray())
        result.append(item.toString());
    return result;
}

std::optional<QSGRendererInterface::GraphicsApi> graphicsApi(
    const QString &name)
{
    if (name == QStringLiteral("Software"))
        return QSGRendererInterface::Software;
    if (name == QStringLiteral("OpenVG"))
        return QSGRendererInterface::OpenVG;
    if (name == QStringLiteral("OpenGL"))
        return QSGRendererInterface::OpenGL;
    if (name == QStringLiteral("Direct3D11"))
        return QSGRendererInterface::Direct3D11;
    if (name == QStringLiteral("Vulkan"))
        return QSGRendererInterface::Vulkan;
    if (name == QStringLiteral("Metal"))
        return QSGRendererInterface::Metal;
    if (name == QStringLiteral("Null"))
        return QSGRendererInterface::Null;
    if (name == QStringLiteral("Direct3D12"))
        return QSGRendererInterface::Direct3D12;
    if (name == QStringLiteral("Unknown"))
        return QSGRendererInterface::Unknown;
    return std::nullopt;
}

QStringList supportedProviderIds(const QVariantMap &result)
{
    QStringList supported;
    for (const QVariant &value : result.value(
             QStringLiteral("providers")).toList()) {
        const QVariantMap provider = value.toMap();
        if (provider.value(QStringLiteral("supported")).toBool())
            supported.append(provider.value(QStringLiteral("id")).toString());
    }
    std::sort(supported.begin(), supported.end());
    return supported;
}

QStringList providerIds(const QVariantMap &result)
{
    QStringList ids;
    for (const QVariant &value : result.value(
             QStringLiteral("providers")).toList()) {
        ids.append(value.toMap().value(QStringLiteral("id")).toString());
    }
    return ids;
}

bool validateCapabilityResult(
    const QJsonObject &testCase,
    const QVariantMap &result,
    const QStringList &expectedProviderIds,
    QStringList *failures)
{
    const QString caseId = testCase.value(QStringLiteral("id")).toString();
    bool valid = true;
    const auto fail = [&](const QString &reason) {
        valid = false;
        appendFailure(
            failures,
            QStringLiteral("%1: %2").arg(caseId, reason));
    };

    static const QSet<QString> allowedCaseFields {
        QStringLiteral("id"),
        QStringLiteral("graphicsApi"),
        QStringLiteral("glMajor"),
        QStringLiteral("glMinor"),
        QStringLiteral("vendor"),
        QStringLiteral("renderer"),
        QStringLiteral("maximumTextureSize"),
        QStringLiteral("expectedSupportedProviders"),
        QStringLiteral("expectedSoftwareRenderer"),
        QStringLiteral("expectedUnavailableReasonContains"),
    };
    for (auto iterator = testCase.constBegin(); iterator != testCase.constEnd();
         ++iterator) {
        if (!allowedCaseFields.contains(iterator.key())) {
            fail(QStringLiteral("fixture contains non-capability field %1")
                     .arg(iterator.key()));
        }
    }
    static const QStringList requiredOracleFields {
        QStringLiteral("expectedSupportedProviders"),
        QStringLiteral("expectedSoftwareRenderer"),
        QStringLiteral("expectedUnavailableReasonContains"),
    };
    for (const QString &field : requiredOracleFields) {
        if (!testCase.contains(field))
            fail(QStringLiteral("fixture is missing oracle field %1").arg(field));
    }

    if (providerIds(result) != expectedProviderIds)
        fail(QStringLiteral("production provider set/order differs from fixture"));

    QStringList actualSupported = supportedProviderIds(result);
    QStringList expectedSupported = jsonStrings(
        testCase.value(QStringLiteral("expectedSupportedProviders")));
    std::sort(expectedSupported.begin(), expectedSupported.end());
    if (actualSupported != expectedSupported)
        fail(QStringLiteral("supported provider decision differs from oracle"));

    static const QStringList forbiddenResultFields {
        QStringLiteral("recommendedProviderId"),
        QStringLiteral("recommendedPresetId"),
        QStringLiteral("maximumRecommendedPreset"),
        QStringLiteral("recommendationConfidence"),
        QStringLiteral("classificationReason"),
        QStringLiteral("recommendationSummary"),
        QStringLiteral("matchedCapabilityEntryId"),
        QStringLiteral("capabilityTableSchemaVersion"),
        QStringLiteral("capabilityTableRevision"),
        QStringLiteral("referenceScenario"),
        QStringLiteral("adapter"),
        QStringLiteral("hardwareClass"),
    };
    for (const QString &field : forbiddenResultFields) {
        if (result.contains(field)) {
            fail(QStringLiteral("production result still exposes removed field %1")
                     .arg(field));
        }
    }

    if (result.value(QStringLiteral("graphicsApiName")).toString()
        != testCase.value(QStringLiteral("graphicsApi")).toString()) {
        fail(QStringLiteral("graphics API identity was not preserved"));
    }
    if (result.value(QStringLiteral("glMajor")).toInt()
            != std::max(0, testCase.value(QStringLiteral("glMajor")).toInt())
        || result.value(QStringLiteral("glMinor")).toInt()
            != std::max(0, testCase.value(QStringLiteral("glMinor")).toInt())
        || result.value(QStringLiteral("maximumTextureSize")).toInt()
            != std::max(
                0,
                testCase.value(QStringLiteral("maximumTextureSize")).toInt())) {
        fail(QStringLiteral("normalized capability values differ from input"));
    }
    if (result.value(QStringLiteral("vendor")).toString()
            != testCase.value(QStringLiteral("vendor")).toString().trimmed()
        || result.value(QStringLiteral("renderer")).toString()
            != testCase.value(QStringLiteral("renderer")).toString().trimmed()) {
        fail(QStringLiteral("renderer identity was not preserved"));
    }
    if (result.value(QStringLiteral("softwareRenderer")).toBool()
        != testCase.value(QStringLiteral("expectedSoftwareRenderer")).toBool()) {
        fail(QStringLiteral("software-renderer decision differs from oracle"));
    }

    const QVariantList providers = result.value(
        QStringLiteral("providers")).toList();
    for (const QVariant &value : providers) {
        const QVariantMap provider = value.toMap();
        const QString id = provider.value(QStringLiteral("id")).toString();
        if (provider.value(QStringLiteral("requiredBackend")).toString().isEmpty())
            fail(QStringLiteral("provider %1 has no backend contract").arg(id));
        if (provider.contains(QStringLiteral("recommended"))) {
            fail(QStringLiteral("provider %1 still exposes a recommendation field")
                     .arg(id));
        }
        if (!provider.value(QStringLiteral("supported")).toBool()
            && provider.value(QStringLiteral("unavailableReason")).toString().isEmpty()) {
            fail(QStringLiteral("provider %1 has no unavailable reason").arg(id));
        }
    }
    const QVariantMap anime4k = providers.isEmpty()
        ? QVariantMap{} : providers.constFirst().toMap();
    const QString expectedReasonFragment = testCase.value(
        QStringLiteral("expectedUnavailableReasonContains")).toString();
    const QString actualReason = anime4k.value(
        QStringLiteral("unavailableReason")).toString();
    if (expectedReasonFragment.isEmpty()) {
        if (!actualReason.isEmpty())
            fail(QStringLiteral("supported provider unexpectedly has an unavailable reason"));
    } else if (!actualReason.contains(expectedReasonFragment)) {
        fail(QStringLiteral("unavailable reason differs from oracle"));
    }
    return valid;
}

QStringList uniqueInOrder(const QStringList &values)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &value : values) {
        if (!seen.contains(value)) {
            result.append(value);
            seen.insert(value);
        }
    }
    return result;
}

bool validArtifact(const ShaderArtifact &artifact, QString *reason)
{
    if (artifact.id.isEmpty() || artifact.providerId.isEmpty()
        || artifact.version.isEmpty() || artifact.assetSetId.isEmpty()) {
        *reason = QStringLiteral("has incomplete identity metadata");
        return false;
    }
    if (artifact.fileName.isEmpty()
        || QFileInfo(artifact.fileName).fileName() != artifact.fileName) {
        *reason = QStringLiteral("has an unsafe file name");
        return false;
    }
    const QString cleanRelativePath = QDir::cleanPath(
        artifact.installRelativePath);
    if (artifact.installRelativePath.isEmpty()
        || QDir::isAbsolutePath(artifact.installRelativePath)
        || cleanRelativePath == QStringLiteral("..")
        || cleanRelativePath.startsWith(QStringLiteral("../"))
        || QFileInfo(cleanRelativePath).fileName() != artifact.fileName) {
        *reason = QStringLiteral("has an unsafe install-relative path");
        return false;
    }
    const QUrl downloadUrl(artifact.downloadUrl);
    if (!downloadUrl.isValid()
        || downloadUrl.scheme() != QStringLiteral("https")
        || downloadUrl.host().isEmpty() || !downloadUrl.userInfo().isEmpty()) {
        *reason = QStringLiteral("has an unsafe download URL");
        return false;
    }
    static const QRegularExpression sha256Pattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (artifact.sizeBytes <= 0
        || !sha256Pattern.match(artifact.sha256).hasMatch()) {
        *reason = QStringLiteral("has an invalid size or SHA-256");
        return false;
    }
    if (artifact.licenseSpdx.isEmpty()) {
        *reason = QStringLiteral("has no SPDX license identifier");
        return false;
    }
    const QUrl licenseUrl(artifact.licenseUrl);
    if (!licenseUrl.isValid()
        || licenseUrl.scheme() != QStringLiteral("https")
        || licenseUrl.host().isEmpty()) {
        *reason = QStringLiteral("has an invalid license URL");
        return false;
    }
    return true;
}

bool validateProfile(
    const ResolvedProfile &profile,
    const QString &providerId,
    const QString &presetId,
    const QStringList &expectedChain,
    const QStringList &expectedFallbacks,
    QStringList *failures)
{
    const QString profileId = providerId + QLatin1Char('/') + presetId;
    bool valid = true;
    const auto fail = [&](const QString &reason) {
        valid = false;
        appendFailure(failures, QStringLiteral("%1: %2").arg(profileId, reason));
    };

    if (profile.requestedProviderId != providerId
        || profile.requestedPresetId != presetId
        || profile.providerId != providerId || profile.presetId != presetId
        || profile.profileId != profileId) {
        fail(QStringLiteral("resolved identity differs from request"));
    }
    if (profile.usedDefaultProvider || profile.usedDefaultPreset
        || profile.usedDefaultCustom) {
        fail(QStringLiteral("standard profile unexpectedly used a default"));
    }
    if (profile.version.isEmpty() || profile.assetSetId.isEmpty())
        fail(QStringLiteral("versioned asset-set identity is incomplete"));
    if (profile.orderedShaderArtifactIds != expectedChain)
        fail(QStringLiteral("ordered shader chain differs from the pinned profile"));
    if (profile.fallbackProfileIds != expectedFallbacks)
        fail(QStringLiteral("fallback order differs from the bounded profile ladder"));
    if (!profile.normalizedCustom.isEmpty())
        fail(QStringLiteral("standard profile unexpectedly contains custom input"));

    const QStringList expectedRequired = uniqueInOrder(expectedChain);
    QStringList actualRequired;
    QSet<QString> actualRequiredSet;
    for (const ShaderArtifact &artifact : profile.requiredArtifacts) {
        actualRequired.append(artifact.id);
        if (actualRequiredSet.contains(artifact.id))
            fail(QStringLiteral("required artifact list contains duplicates"));
        actualRequiredSet.insert(artifact.id);

        const ShaderArtifact *catalogArtifact = UpscalingCatalog::findArtifact(
            artifact.id);
        if (!catalogArtifact) {
            fail(QStringLiteral("required artifact %1 is absent from catalog")
                     .arg(artifact.id));
            continue;
        }
        if (catalogArtifact->providerId != providerId
            || catalogArtifact->version != profile.version
            || catalogArtifact->assetSetId != profile.assetSetId) {
            fail(QStringLiteral("artifact %1 crosses profile ownership/version")
                     .arg(artifact.id));
        }
        QString artifactReason;
        if (!validArtifact(*catalogArtifact, &artifactReason)) {
            fail(QStringLiteral("artifact %1 %2")
                     .arg(artifact.id, artifactReason));
        }
    }
    if (actualRequired != expectedRequired)
        fail(QStringLiteral("required artifact closure differs from shader chain"));

    for (const QString &artifactId : profile.orderedShaderArtifactIds) {
        if (!actualRequiredSet.contains(artifactId)) {
            fail(QStringLiteral("shader %1 is missing from required artifacts")
                     .arg(artifactId));
        }
    }
    for (auto it = profile.mpvOptions.cbegin(); it != profile.mpvOptions.cend(); ++it) {
        if (!UpscalingCatalog::isWhitelistedMpvOption(it.key())) {
            fail(QStringLiteral("mpv option %1 is not catalog-whitelisted")
                     .arg(it.key()));
        }
    }
    return valid;
}

HealthWindow healthyWindow()
{
    return {
        .renderSamples = 60,
        .renderP95Ms = 7.0,
        .estimatedFps = 60.0,
        .outputDroppedFrames = 0,
        .mistimedFrames = 0,
        .delayedFrames = 0,
        .avSyncMs = 4.0,
        .playing = true,
    };
}

bool validatePerformanceProtection(QStringList *failures)
{
    bool valid = true;
    const auto check = [&](bool condition, const QString &reason) {
        if (!condition) {
            valid = false;
            appendFailure(failures, reason);
        }
    };

    PerformanceProtection policy;
    policy.reset(2, 20);
    check(policy.remainingFallbacks() == 2,
          QStringLiteral("reset did not preserve the bounded fallback count"));

    const HealthWindow healthy = healthyWindow();
    for (int index = 0; index < 3; ++index) {
        check(policy.evaluate(healthy) == PerformanceProtection::Action::None,
              QStringLiteral("initial warm-up produced an action"));
    }
    check(std::abs(policy.lastFrameBudgetMs() - 1000.0 / 60.0) < 0.001,
          QStringLiteral("frame budget is not derived from observed FPS"));
    check(std::abs(policy.lastGuardBudgetMs() - 1000.0 / 60.0 * 0.8) < 0.001,
          QStringLiteral("reserved headroom was not applied to the guard budget"));

    HealthWindow renderOverload = healthy;
    renderOverload.renderP95Ms = 15.0;
    check(policy.evaluate(renderOverload) == PerformanceProtection::Action::None,
          QStringLiteral("first overload window caused a premature action"));
    check(policy.evaluate(renderOverload) == PerformanceProtection::Action::None,
          QStringLiteral("second overload window caused a premature action"));
    check(policy.evaluate(renderOverload) == PerformanceProtection::Action::Downgrade,
          QStringLiteral("third render overload did not downgrade"));
    check(policy.remainingFallbacks() == 1,
          QStringLiteral("downgrade did not consume exactly one fallback"));

    for (int index = 0; index < 2; ++index) {
        check(policy.evaluate(healthy) == PerformanceProtection::Action::None,
              QStringLiteral("post-downgrade warm-up produced an action"));
    }
    HealthWindow outputOverload = healthy;
    outputOverload.outputDroppedFrames = 1;
    check(policy.evaluate(outputOverload) == PerformanceProtection::Action::None,
          QStringLiteral("first output overload caused a premature action"));
    check(policy.evaluate(outputOverload) == PerformanceProtection::Action::None,
          QStringLiteral("second output overload caused a premature action"));
    check(policy.evaluate(outputOverload) == PerformanceProtection::Action::Downgrade,
          QStringLiteral("third output overload did not downgrade"));
    check(policy.remainingFallbacks() == 0,
          QStringLiteral("second downgrade did not exhaust the fallback ladder"));

    for (int index = 0; index < 2; ++index) {
        check(policy.evaluate(healthy) == PerformanceProtection::Action::None,
              QStringLiteral("second post-downgrade warm-up produced an action"));
    }
    HealthWindow avSyncOverload = healthy;
    avSyncOverload.avSyncMs = 60.0;
    check(policy.evaluate(avSyncOverload) == PerformanceProtection::Action::None,
          QStringLiteral("first A/V overload caused a premature action"));
    check(policy.evaluate(avSyncOverload) == PerformanceProtection::Action::None,
          QStringLiteral("second A/V overload caused a premature action"));
    check(policy.evaluate(avSyncOverload) == PerformanceProtection::Action::Disable,
          QStringLiteral("overload with no fallback did not disable upscaling"));

    PerformanceProtection suppressionPolicy;
    suppressionPolicy.reset(1, 20);
    for (int index = 0; index < 3; ++index)
        suppressionPolicy.evaluate(healthy);
    HealthWindow suppressed = renderOverload;
    suppressed.paused = true;
    check(suppressionPolicy.evaluate(suppressed) == PerformanceProtection::Action::None,
          QStringLiteral("paused playback triggered protection"));
    suppressed.paused = false;
    suppressed.buffering = true;
    check(suppressionPolicy.evaluate(suppressed) == PerformanceProtection::Action::None,
          QStringLiteral("buffering playback triggered protection"));
    suppressed.buffering = false;
    suppressed.renderSamples = 3;
    check(suppressionPolicy.evaluate(suppressed) == PerformanceProtection::Action::None,
          QStringLiteral("an undersampled window triggered protection"));
    check(suppressionPolicy.consecutiveOverloadWindows() == 0,
          QStringLiteral("suppressed windows retained overload history"));

    check(suppressionPolicy.evaluate(renderOverload) == PerformanceProtection::Action::None,
          QStringLiteral("first overload after suppression caused a premature action"));
    check(suppressionPolicy.evaluate(renderOverload) == PerformanceProtection::Action::None,
          QStringLiteral("second overload after suppression caused a premature action"));
    check(suppressionPolicy.evaluate(healthy) == PerformanceProtection::Action::None,
          QStringLiteral("healthy recovery produced an action"));
    check(suppressionPolicy.consecutiveOverloadWindows() == 0,
          QStringLiteral("healthy recovery did not clear overload history"));
    return valid;
}

QJsonArray samplesJson(const QList<double> &samples)
{
    QJsonArray result;
    for (double sample : samples)
        result.append(sample);
    return result;
}

QJsonObject invariantDetails(
    const QString &fixtureSha256, const QStringList &failures)
{
    QJsonArray failureArray;
    for (const QString &failure : failures)
        failureArray.append(failure);
    return {
        {QStringLiteral("evidence"),
         QStringLiteral("fixture-component-observation")},
        {QStringLiteral("fixtureSha256"), fixtureSha256},
        {QStringLiteral("producerPath"),
         QStringLiteral("production-cpp-policy")},
        {QStringLiteral("gpuCertified"), false},
        {QStringLiteral("presentCertified"), false},
        {QStringLiteral("failureCount"), failures.size()},
        {QStringLiteral("failures"), failureArray},
    };
}

QJsonObject metric(
    const QString &id,
    const QString &operation,
    const QList<double> &samples)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("unit"), QStringLiteral("ms")},
        {QStringLiteral("samples"), samplesJson(samples)},
        {QStringLiteral("attributes"), QJsonObject{
             {QStringLiteral("evidence"),
              QStringLiteral("fixture-component-observation")},
             {QStringLiteral("producerPath"),
              QStringLiteral("production-cpp-policy")},
             {QStringLiteral("operation"), operation},
             {QStringLiteral("renderer"), QStringLiteral("none")},
             {QStringLiteral("gpuCertified"), false},
             {QStringLiteral("presentCertified"), false},
         }},
    };
}

bool writeManifest(
    const QString &path, const QJsonObject &manifest, QString *errorMessage)
{
    const QFileInfo outputInfo(path);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        *errorMessage = QStringLiteral("Cannot create output directory %1")
                            .arg(QDir::toNativeSeparators(
                                outputInfo.absolutePath()));
        return false;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        *errorMessage = QStringLiteral("Cannot open %1: %2")
                            .arg(
                                QDir::toNativeSeparators(path),
                                output.errorString());
        return false;
    }
    const QByteArray document = QJsonDocument(manifest).toJson(
        QJsonDocument::Indented);
    if (output.write(document) != document.size() || !output.commit()) {
        *errorMessage = QStringLiteral("Cannot commit %1: %2")
                            .arg(
                                QDir::toNativeSeparators(path),
                                output.errorString());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("yanami-upscaling-perf-probe"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Native hosted probe for production upscaling policies."));
    parser.addHelpOption();
    const QCommandLineOption fixtureOption(
        QStringLiteral("fixture"),
        QStringLiteral("Versioned upscaling capability fixture."),
        QStringLiteral("path"));
    const QCommandLineOption fixtureManifestOption(
        QStringLiteral("fixture-manifest"),
        QStringLiteral("Manifest containing the fixture SHA-256."),
        QStringLiteral("path"));
    const QCommandLineOption outputOption(
        QStringLiteral("output"),
        QStringLiteral("Destination run-manifest JSON path."),
        QStringLiteral("path"));
    const QCommandLineOption modeOption(
        QStringLiteral("mode"),
        QStringLiteral("Gate mode: collect, debt, or enforce."),
        QStringLiteral("mode"),
        QStringLiteral("collect"));
    parser.addOptions({
        fixtureOption,
        fixtureManifestOption,
        outputOption,
        modeOption,
    });
    parser.process(application);

    const QString fixturePath = parser.value(fixtureOption);
    const QString fixtureManifestPath = parser.value(fixtureManifestOption);
    const QString outputPath = parser.value(outputOption);
    const QString mode = parser.value(modeOption).trimmed().toLower();
    if (fixturePath.isEmpty() || fixtureManifestPath.isEmpty()
        || outputPath.isEmpty()) {
        QTextStream(stderr)
            << "--fixture, --fixture-manifest, and --output are required.\n";
        return 1;
    }
    if (mode != QStringLiteral("collect") && mode != QStringLiteral("debt")
        && mode != QStringLiteral("enforce")) {
        QTextStream(stderr)
            << "--mode must be collect, debt, or enforce.\n";
        return 1;
    }

    QString errorMessage;
    const std::optional<JsonFile> fixture = readJsonObject(
        fixturePath, &errorMessage);
    if (!fixture) {
        QTextStream(stderr) << errorMessage << '\n';
        return 2;
    }
    const std::optional<JsonFile> fixtureManifest = readJsonObject(
        fixtureManifestPath, &errorMessage);
    if (!fixtureManifest) {
        QTextStream(stderr) << errorMessage << '\n';
        return 2;
    }

    const QString fixtureSha256 = QString::fromLatin1(
        QCryptographicHash::hash(
            fixture->bytes, QCryptographicHash::Sha256).toHex());
    const QJsonObject fixtureManifestObject = fixtureManifest->object;
    if (fixtureManifestObject.value(QStringLiteral("schemaVersion")).toString()
            != QLatin1String(schemaVersion)
        || fixtureManifestObject.value(QStringLiteral("id")).toString()
            != QLatin1String(fixtureId)
        || fixtureManifestObject.value(QStringLiteral("sha256")).toString()
                .toLower()
            != fixtureSha256
        || fixtureManifestObject.value(QStringLiteral("file")).toString()
            != QFileInfo(fixturePath).fileName()) {
        QTextStream(stderr)
            << "Upscaling fixture identity or SHA-256 does not match its manifest.\n";
        return 3;
    }

    const QJsonObject contract = fixture->object;
    if (contract.value(QStringLiteral("schemaVersion")).toString()
            != QLatin1String(schemaVersion)
        || contract.value(QStringLiteral("cases")).toArray().isEmpty()) {
        QTextStream(stderr)
            << "Upscaling fixture schema is unsupported or has no cases.\n";
        return 3;
    }

    QStringList fixtureProviderIds;
    for (const QJsonValue &value : contract.value(
             QStringLiteral("providers")).toArray()) {
        fixtureProviderIds.append(
            value.toObject().value(QStringLiteral("id")).toString());
    }
    QStringList capabilityFailures;
    bool capabilityValid = fixtureProviderIds
            == QStringList{QStringLiteral("anime4k")}
        && jsonStrings(contract.value(QStringLiteral("presets")))
            == QStringList{
                QStringLiteral("performance"),
                QStringLiteral("balanced"),
                QStringLiteral("quality")};
    if (!capabilityValid) {
        appendFailure(
            &capabilityFailures,
            QStringLiteral("fixture provider or preset identity contract is invalid"));
    }

    const QString startedAtUtc = QDateTime::currentDateTimeUtc().toString(
        Qt::ISODateWithMs);
    QList<double> capabilitySamples;
    const QJsonArray capabilityCases = contract.value(
        QStringLiteral("cases")).toArray();
    capabilitySamples.reserve(capabilityIterations * capabilityCases.size());
    for (int iteration = 0; iteration < capabilityIterations; ++iteration) {
        for (const QJsonValue &value : capabilityCases) {
            const QJsonObject testCase = value.toObject();
            const std::optional<QSGRendererInterface::GraphicsApi> api =
                graphicsApi(testCase.value(
                    QStringLiteral("graphicsApi")).toString());
            if (!api) {
                capabilityValid = false;
                appendFailure(
                    &capabilityFailures,
                    QStringLiteral("%1: fixture has an unknown graphics API")
                        .arg(testCase.value(QStringLiteral("id")).toString()));
                continue;
            }

            QElapsedTimer timer;
            timer.start();
            const QVariantMap result = UpscalingCapabilityProbe::evaluate(
                *api,
                testCase.value(QStringLiteral("glMajor")).toInt(),
                testCase.value(QStringLiteral("glMinor")).toInt(),
                testCase.value(QStringLiteral("vendor")).toString(),
                testCase.value(QStringLiteral("renderer")).toString(),
                testCase.value(QStringLiteral("maximumTextureSize")).toInt());
            capabilitySamples.append(timer.nsecsElapsed() / 1'000'000.0);
            capabilityValid = validateCapabilityResult(
                                  testCase,
                                  result,
                                  fixtureProviderIds,
                                  &capabilityFailures)
                && capabilityValid;
        }
    }
    constexpr qsizetype minimumCapabilitySamples = 1'000;
    if (capabilitySamples.size() < minimumCapabilitySamples) {
        capabilityValid = false;
        appendFailure(
            &capabilityFailures,
            QStringLiteral("capability probe produced fewer than 1000 samples"));
    }

    struct CatalogScenario
    {
        QString providerId;
        QString presetId;
        QStringList expectedChain;
        QStringList expectedFallbacks;
    };
    const QList<CatalogScenario> catalogScenarios = {
        {
            QStringLiteral("anime4k"),
            QStringLiteral("performance"),
            {
                QStringLiteral("anime4k.clamp"),
                QStringLiteral("anime4k.restore.a.m"),
                QStringLiteral("anime4k.upscale.m"),
                QStringLiteral("anime4k.autodown.x2"),
                QStringLiteral("anime4k.autodown.x4"),
                QStringLiteral("anime4k.upscale.s"),
            },
            {QStringLiteral("original")},
        },
        {
            QStringLiteral("anime4k"),
            QStringLiteral("balanced"),
            {
                QStringLiteral("anime4k.clamp"),
                QStringLiteral("anime4k.restore.a.vl"),
                QStringLiteral("anime4k.upscale.vl"),
                QStringLiteral("anime4k.autodown.x2"),
                QStringLiteral("anime4k.autodown.x4"),
                QStringLiteral("anime4k.upscale.m"),
            },
            {QStringLiteral("anime4k/performance"), QStringLiteral("original")},
        },
        {
            QStringLiteral("anime4k"),
            QStringLiteral("quality"),
            {
                QStringLiteral("anime4k.clamp"),
                QStringLiteral("anime4k.restore.a.vl"),
                QStringLiteral("anime4k.upscale.vl"),
                QStringLiteral("anime4k.autodown.x2"),
                QStringLiteral("anime4k.autodown.x4"),
                QStringLiteral("anime4k.restore.a.m"),
                QStringLiteral("anime4k.upscale.m"),
            },
            {
                QStringLiteral("anime4k/balanced"),
                QStringLiteral("anime4k/performance"),
                QStringLiteral("original"),
            },
        },
    };

    QStringList catalogFailures;
    bool catalogValid = UpscalingCatalog::providerIds()
            == QStringList{QStringLiteral("anime4k")}
        && UpscalingCatalog::presetIds()
            == QStringList{
                QStringLiteral("performance"),
                QStringLiteral("balanced"),
                QStringLiteral("quality"),
                QStringLiteral("custom")};
    if (!catalogValid) {
        appendFailure(
            &catalogFailures,
            QStringLiteral("catalog provider or preset identity contract changed"));
    }
    for (const CatalogScenario &scenario : catalogScenarios) {
        // Initialize catalog-owned immutable tables outside the timed region.
        UpscalingCatalog::resolve(scenario.providerId, scenario.presetId);
    }

    QList<double> presetSamples;
    presetSamples.reserve(catalogIterations * catalogScenarios.size());
    for (int iteration = 0; iteration < catalogIterations; ++iteration) {
        for (const CatalogScenario &scenario : catalogScenarios) {
            QElapsedTimer timer;
            timer.start();
            const ResolvedProfile profile = UpscalingCatalog::resolve(
                scenario.providerId, scenario.presetId);
            presetSamples.append(timer.nsecsElapsed() / 1'000'000.0);
            catalogValid = validateProfile(
                               profile,
                               scenario.providerId,
                               scenario.presetId,
                               scenario.expectedChain,
                               scenario.expectedFallbacks,
                               &catalogFailures)
                && catalogValid;
        }
    }

    QStringList performanceFailures;
    const bool performanceProtectionValid = validatePerformanceProtection(
        &performanceFailures);

    QString fingerprint = qEnvironmentVariable(
        "YANAMI_PERF_RUN_FINGERPRINT").trimmed();
    if (fingerprint.isEmpty())
        fingerprint = QStringLiteral("hosted-upscaling-unclassified");
    QString runUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    runUuid.remove(QLatin1Char('-'));

    QJsonArray metrics;
    metrics.append(metric(
        QStringLiteral("upscaling.hosted_smoke.capability_resolve_ms"),
        QStringLiteral("UpscalingCapabilityProbe::evaluate"),
        capabilitySamples));
    metrics.append(metric(
        QStringLiteral("upscaling.hosted_smoke.preset_resolve_ms"),
        QStringLiteral("UpscalingCatalog::resolve"),
        presetSamples));

    QJsonArray invariants;
    invariants.append(QJsonObject{
        {QStringLiteral("id"),
         QStringLiteral("upscaling.capability_policy_valid")},
        {QStringLiteral("passed"), capabilityValid},
        {QStringLiteral("details"),
         invariantDetails(fixtureSha256, capabilityFailures)},
    });
    invariants.append(QJsonObject{
        {QStringLiteral("id"),
         QStringLiteral("upscaling.catalog_profiles_valid")},
        {QStringLiteral("passed"), catalogValid},
        {QStringLiteral("details"),
         invariantDetails(fixtureSha256, catalogFailures)},
    });
    invariants.append(QJsonObject{
        {QStringLiteral("id"),
         QStringLiteral("upscaling.performance_protection_valid")},
        {QStringLiteral("passed"), performanceProtectionValid},
        {QStringLiteral("details"),
         invariantDetails(fixtureSha256, performanceFailures)},
    });

    const QJsonObject manifest = {
        {QStringLiteral("schemaVersion"), QLatin1String(schemaVersion)},
        {QStringLiteral("runId"),
         QStringLiteral("upscaling-hosted-") + runUuid},
        {QStringLiteral("profile"), QStringLiteral("PullRequest")},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("startedAtUtc"), startedAtUtc},
        {QStringLiteral("finishedAtUtc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("environment"), QJsonObject{
             {QStringLiteral("fingerprint"), fingerprint},
             {QStringLiteral("referenceMatch"), false},
             {QStringLiteral("mismatchReasons"), QJsonArray{
                  QStringLiteral(
                      "Hosted production-policy probe cannot certify GPU or native Present evidence."),
              }},
             {QStringLiteral("details"), QJsonObject{
                  {QStringLiteral("gpuCertified"), false},
                  {QStringLiteral("presentCertified"), false},
                  {QStringLiteral("probeKind"),
                   QStringLiteral("native-upscaling-production-probe")},
              }},
         }},
        {QStringLiteral("fixtures"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("id"), QLatin1String(fixtureId)},
                 {QStringLiteral("version"), QStringLiteral("1")},
                 {QStringLiteral("sha256"), fixtureSha256},
                 {QStringLiteral("validated"), true},
             },
         }},
        {QStringLiteral("suites"), QJsonArray{QStringLiteral("upscaling")}},
        {QStringLiteral("metrics"), metrics},
        {QStringLiteral("invariants"), invariants},
    };

    if (!writeManifest(outputPath, manifest, &errorMessage)) {
        QTextStream(stderr) << errorMessage << '\n';
        return 4;
    }

    QTextStream(stdout)
        << "manifest=" << QDir::toNativeSeparators(
               QFileInfo(outputPath).absoluteFilePath())
        << " capability_samples=" << capabilitySamples.size()
        << " preset_samples=" << presetSamples.size()
        << " capability_valid=" << (capabilityValid ? "true" : "false")
        << " catalog_valid=" << (catalogValid ? "true" : "false")
        << " performance_protection_valid="
        << (performanceProtectionValid ? "true" : "false") << '\n';

    return capabilityValid && catalogValid && performanceProtectionValid ? 0 : 5;
}
