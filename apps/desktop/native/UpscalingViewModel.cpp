#include "UpscalingViewModel.hpp"

#include "ApplicationPaths.hpp"

#include "ApplicationViewModel.hpp"
#include "UpscalingAssetManager.hpp"
#include "UpscalingCapabilityProbe.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QQuickWindow>
#include <QScopedValueRollback>

#include <algorithm>

namespace {

QString defaultAssetRoot()
{
    return ApplicationPaths::upscalingAssetRoot();
}

QVariantMap disabledRuntimeConfig()
{
    return {
        {QStringLiteral("schema"), 1},
        {QStringLiteral("enabled"), false},
    };
}

bool usesShaderAssets(YanamiUpscaling::BackendKind backendKind)
{
    return backendKind == YanamiUpscaling::BackendKind::GlslShaders;
}

} // namespace

UpscalingViewModel::UpscalingViewModel(
    PreferencesViewModel *preferences, QObject *parent)
    : QObject(parent)
    , m_preferences(preferences)
    , m_assets(new UpscalingAssetManager(defaultAssetRoot(), this))
    , m_capabilities(new UpscalingCapabilityProbe(this))
{
    connect(preferences, &PreferencesViewModel::upscalingSettingsChanged,
        this, &UpscalingViewModel::rebuild);
    connect(m_assets, &UpscalingAssetManager::stateChanged,
        this, [this](const QString &) { rebuild(); });
    connect(m_capabilities, &UpscalingCapabilityProbe::resultChanged,
        this, &UpscalingViewModel::rebuild);
    rebuild();
}

UpscalingViewModel::UpscalingViewModel(
    PreferencesViewModel *preferences,
    UpscalingAssetManager *assets,
    UpscalingCapabilityProbe *capabilities,
    QObject *parent)
    : QObject(parent)
    , m_preferences(preferences)
    , m_assets(assets)
    , m_capabilities(capabilities)
{
    if (preferences) {
        connect(preferences, &PreferencesViewModel::upscalingSettingsChanged,
            this, &UpscalingViewModel::rebuild);
    }
    if (assets) {
        connect(assets, &UpscalingAssetManager::stateChanged,
            this, [this](const QString &) { rebuild(); });
    }
    if (capabilities) {
        connect(capabilities, &UpscalingCapabilityProbe::resultChanged,
            this, &UpscalingViewModel::rebuild);
    }
    rebuild();
}

QVariantMap UpscalingViewModel::settings() const
{
    return m_preferences ? m_preferences->upscalingSettings() : QVariantMap{};
}

bool UpscalingViewModel::capabilityReady() const
{
    return m_capabilities && m_capabilities->ready();
}

QVariantMap UpscalingViewModel::capability() const
{
    return m_capabilities ? m_capabilities->result() : QVariantMap{};
}

void UpscalingViewModel::observeWindow(QQuickWindow *window)
{
    if (m_capabilities)
        m_capabilities->observe(window);
}

void UpscalingViewModel::saveSettings(const QVariantMap &settings)
{
    if (m_preferences)
        m_preferences->saveUpscalingSettings(settings);
}

void UpscalingViewModel::downloadSelected()
{
    if (!m_assets || m_selectedAssetSetId.isEmpty()
        || m_resolved.requiredArtifacts.isEmpty()) {
        return;
    }
    m_assets->download(
        m_selectedAssetSetId,
        YanamiUpscaling::UpscalingCatalog::deploymentArtifacts(m_resolved));
}

void UpscalingViewModel::cancelSelected()
{
    if (m_assets && !m_selectedAssetSetId.isEmpty())
        m_assets->cancel(m_selectedAssetSetId);
}

QVariantMap UpscalingViewModel::providerCapability(
    const QString &providerId) const
{
    const QVariantList capabilityProviders = capability()
        .value(QStringLiteral("providers")).toList();
    for (const QVariant &value : capabilityProviders) {
        const QVariantMap provider = value.toMap();
        if (provider.value(QStringLiteral("id")).toString() == providerId)
            return provider;
    }
    return {};
}

bool UpscalingViewModel::providerSupported(const QString &providerId) const
{
    return capabilityReady()
        && providerCapability(providerId)
               .value(QStringLiteral("supported")).toBool();
}

QString UpscalingViewModel::selectionAssetSetId(
    const YanamiUpscaling::ResolvedProfile &profile) const
{
    if (profile.assetSetId.isEmpty()
        || profile.requiredArtifacts.isEmpty()) {
        return {};
    }
    QByteArray identity = profile.assetSetId.toUtf8();
    for (const auto &artifact :
         YanamiUpscaling::UpscalingCatalog::deploymentArtifacts(profile)) {
        identity.append('\0');
        identity.append(artifact.id.toUtf8());
    }
    const QByteArray suffix = QCryptographicHash::hash(
        identity, QCryptographicHash::Sha256).toHex().left(16);
    return profile.assetSetId + QLatin1Char('-')
        + QString::fromLatin1(suffix);
}

QVariantMap UpscalingViewModel::anime4kCustom(
    const QVariantMap &value) const
{
    const int passes = value.value(
        QStringLiteral("anime4kRestorePasses"), 1).toInt();
    const QString size = value.value(
        QStringLiteral("anime4kModelSize"),
        QStringLiteral("vl")).toString().toUpper();
    const QString secondary = size == QStringLiteral("S")
        ? QStringLiteral("M") : QStringLiteral("S");
    return {
        {QStringLiteral("mode"),
         value.value(QStringLiteral("anime4kMode"),
             QStringLiteral("a")).toString().toUpper()},
        {QStringLiteral("primarySize"), size},
        {QStringLiteral("secondarySize"), secondary},
        // The UI and catalog both expose one or two restore passes.
        {QStringLiteral("passes"), std::clamp(passes, 1, 2)},
        {QStringLiteral("autoDownscale"),
         value.value(QStringLiteral("anime4kAutoDownscale"), true)},
    };
}

void UpscalingViewModel::ensureVerified(
    const YanamiUpscaling::ResolvedProfile &profile,
    const QString &selectionKey)
{
    if (!m_assets || selectionKey.isEmpty()
        || profile.requiredArtifacts.isEmpty()
        || m_verificationRequested.contains(selectionKey)) {
        return;
    }
    m_verificationRequested.insert(selectionKey);
    m_assets->verify(
        selectionKey,
        YanamiUpscaling::UpscalingCatalog::deploymentArtifacts(profile));
}

QVariantMap UpscalingViewModel::runtimeConfigFor(
    const YanamiUpscaling::ResolvedProfile &profile,
    bool includeFallbacks,
    const QString &verifiedAssetSetId) const
{
    using YanamiUpscaling::BackendKind;

    if (profile.providerId
            != QLatin1String(YanamiUpscaling::Id::ProviderAnime4k)
        || profile.backendKind != BackendKind::GlslShaders) {
        return disabledRuntimeConfig();
    }

    QStringList orderedPaths;
    if (profile.requiredArtifacts.isEmpty()
        || profile.orderedShaderArtifactIds.isEmpty()) {
        return disabledRuntimeConfig();
    }
    if (!m_assets)
        return disabledRuntimeConfig();
    // The selected profile is verified against its complete deployment
    // closure (selected tier plus every bounded fallback). Reuse that single
    // ready state while materializing fallback configs so the published
    // "ready" state and the runtime ladder are atomic.
    const QString selectionKey = verifiedAssetSetId.isEmpty()
        ? selectionAssetSetId(profile) : verifiedAssetSetId;
    if (selectionKey.isEmpty()
        || m_assets->stateFor(selectionKey)
               .value(QStringLiteral("phase")).toString()
           != QStringLiteral("ready")) {
        return disabledRuntimeConfig();
    }

    QHash<QString, QString> localPaths;
    for (const auto &artifact : profile.requiredArtifacts)
        localPaths.insert(artifact.id, m_assets->absolutePath(artifact));
    orderedPaths.reserve(profile.orderedShaderArtifactIds.size());
    for (const QString &artifactId : profile.orderedShaderArtifactIds) {
        const QString path = localPaths.value(artifactId);
        if (path.isEmpty())
            return disabledRuntimeConfig();
        orderedPaths.append(QFileInfo(path).absoluteFilePath());
    }

    const QVariantMap backend {
        {QStringLiteral("kind"),
         QString::fromLatin1(YanamiUpscaling::Id::BackendGlslShaders)},
    };

    const QVariantMap currentSettings = settings();
    QVariantMap result {
        {QStringLiteral("schema"), 1},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("providerId"), profile.providerId},
        {QStringLiteral("profileId"), profile.profileId},
        {QStringLiteral("modelVersion"), profile.version},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("performanceProtection"),
         currentSettings.value(QStringLiteral("performanceProtection"), true)},
        {QStringLiteral("reservedHeadroomPercent"),
         currentSettings.value(QStringLiteral("autoHeadroom"), 20)},
    };
    result.insert(QStringLiteral("orderedShaderPaths"), orderedPaths);
    result.insert(QStringLiteral("options"), profile.mpvOptions);

    if (!includeFallbacks)
        return result;
    QVariantList fallbacks;
    for (const QString &fallbackId : profile.fallbackProfileIds) {
        if (fallbackId == QStringLiteral("original"))
            continue;
        const QStringList parts = fallbackId.split(QLatin1Char('/'));
        if (parts.size() != 2)
            continue;
        const auto fallback = YanamiUpscaling::UpscalingCatalog::resolve(
            parts.at(0), parts.at(1));
        const QVariantMap fallbackConfig = runtimeConfigFor(
            fallback, false, selectionKey);
        if (fallbackConfig.value(QStringLiteral("enabled")).toBool())
            fallbacks.append(fallbackConfig);
    }
    result.insert(QStringLiteral("fallbacks"), fallbacks);
    return result;
}

void UpscalingViewModel::rebuild()
{
    // Asset verification publishes its initial state synchronously. That
    // signal is connected back to rebuild(), so starting verification while
    // iterating m_resolved can otherwise recursively replace the profile and
    // invalidate the outer call's QString/QVector iterators. Coalesce any
    // nested request onto the event loop and let the current snapshot finish.
    if (m_rebuildInProgress) {
        if (!m_rebuildQueued) {
            m_rebuildQueued = true;
            QMetaObject::invokeMethod(
                this,
                [this] {
                    m_rebuildQueued = false;
                    rebuild();
                },
                Qt::QueuedConnection);
        }
        return;
    }
    QScopedValueRollback rebuilding(m_rebuildInProgress, true);

    const QVariantMap desired = settings();

    QVariantList providers;
    QVariantMap anime4k = providerCapability(
        QString::fromLatin1(YanamiUpscaling::Id::ProviderAnime4k));
    anime4k.insert(QStringLiteral("id"),
        QString::fromLatin1(YanamiUpscaling::Id::ProviderAnime4k));
    if (!capabilityReady()) {
        anime4k.insert(QStringLiteral("supported"), false);
        anime4k.insert(QStringLiteral("unavailableReason"),
            tr("Checking graphics capabilities…"));
    }
    providers.append(anime4k);
    m_providers = providers;

    m_resolved = {};
    m_selectedAssetSetId.clear();
    m_selectedAssets = {
        {QStringLiteral("phase"), QStringLiteral("unsupported")},
        {QStringLiteral("progress"), 0.0},
    };
    m_effectiveRuntimeConfig = disabledRuntimeConfig();
    m_effectiveEnabled = false;
    m_fallbackReason.clear();

    const QString desiredProvider = desired.value(
        QStringLiteral("providerId"),
        QString::fromLatin1(YanamiUpscaling::Id::ProviderAnime4k)).toString();
    if (!capabilityReady()) {
        m_fallbackReason = QStringLiteral("capability-checking");
        emit stateChanged();
        return;
    }
    if (desiredProvider
            != QLatin1String(YanamiUpscaling::Id::ProviderAnime4k)
        || !providerSupported(desiredProvider)) {
        m_fallbackReason = QStringLiteral("unsupported");
        emit stateChanged();
        return;
    }

    QString preset = desired.value(
        QStringLiteral("presetId"), QStringLiteral("balanced")).toString();
    if (!YanamiUpscaling::UpscalingCatalog::presetIds().contains(preset)) {
        preset = QStringLiteral("balanced");
    }
    QVariantMap custom;
    if (preset == QStringLiteral("custom"))
        custom = anime4kCustom(desired);
    m_resolved = YanamiUpscaling::UpscalingCatalog::resolve(
        desiredProvider, preset, custom);
    if (m_resolved.providerId.isEmpty()
        || m_resolved.backendKind
            == YanamiUpscaling::BackendKind::None) {
        m_fallbackReason = QStringLiteral("invalid-profile");
        emit stateChanged();
        return;
    }
    const bool shaderBackend = usesShaderAssets(m_resolved.backendKind);
    if (!shaderBackend
        || m_resolved.providerId
            != QLatin1String(YanamiUpscaling::Id::ProviderAnime4k)
        || m_resolved.requiredArtifacts.isEmpty()
        || m_resolved.orderedShaderArtifactIds.isEmpty()) {
        m_fallbackReason = QStringLiteral("invalid-profile");
        emit stateChanged();
        return;
    }
    m_selectedAssetSetId = selectionAssetSetId(m_resolved);
    ensureVerified(m_resolved, m_selectedAssetSetId);

    m_selectedAssets = m_assets
        ? m_assets->stateFor(m_selectedAssetSetId) : QVariantMap{};
    m_selectedAssets.insert(QStringLiteral("requiresDownload"), true);
    const QString phase = m_selectedAssets
        .value(QStringLiteral("phase")).toString();
    if (!desired.value(QStringLiteral("enabled"), false).toBool()) {
        m_fallbackReason = QStringLiteral("disabled");
    } else if (phase == QStringLiteral("ready")) {
        m_effectiveRuntimeConfig = runtimeConfigFor(
            m_resolved, true, m_selectedAssetSetId);
        m_effectiveEnabled = m_effectiveRuntimeConfig
            .value(QStringLiteral("enabled")).toBool();
    } else if (phase == QStringLiteral("failed")) {
        m_fallbackReason = m_selectedAssets
            .value(QStringLiteral("errorCode")).toString();
    } else {
        m_fallbackReason = QStringLiteral("components-required");
    }
    emit stateChanged();
}
