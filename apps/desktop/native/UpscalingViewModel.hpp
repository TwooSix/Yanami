#pragma once

#include "UpscalingCatalog.hpp"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

class PreferencesViewModel;
class QQuickWindow;
class UpscalingAssetManager;
class UpscalingCapabilityProbe;

class UpscalingViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY stateChanged)
    Q_PROPERTY(QVariantList providers READ providers NOTIFY stateChanged)
    Q_PROPERTY(bool capabilityReady READ capabilityReady NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap capability READ capability NOTIFY stateChanged)
    Q_PROPERTY(QString resolvedProviderId READ resolvedProviderId NOTIFY stateChanged)
    Q_PROPERTY(QString resolvedPresetId READ resolvedPresetId NOTIFY stateChanged)
    Q_PROPERTY(QString resolvedProfileId READ resolvedProfileId NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedAssets READ selectedAssets NOTIFY stateChanged)
    Q_PROPERTY(bool effectiveEnabled READ effectiveEnabled NOTIFY stateChanged)
    Q_PROPERTY(QString fallbackReason READ fallbackReason NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap effectiveRuntimeConfig READ effectiveRuntimeConfig NOTIFY stateChanged)

public:
    explicit UpscalingViewModel(
        PreferencesViewModel *preferences,
        QObject *parent = nullptr);
    UpscalingViewModel(
        PreferencesViewModel *preferences,
        UpscalingAssetManager *assets,
        UpscalingCapabilityProbe *capabilities,
        QObject *parent = nullptr);

    QVariantMap settings() const;
    QVariantList providers() const { return m_providers; }
    bool capabilityReady() const;
    QVariantMap capability() const;
    QString resolvedProviderId() const { return m_resolved.providerId; }
    QString resolvedPresetId() const { return m_resolved.presetId; }
    QString resolvedProfileId() const { return m_resolved.profileId; }
    QVariantMap selectedAssets() const { return m_selectedAssets; }
    bool effectiveEnabled() const { return m_effectiveEnabled; }
    QString fallbackReason() const { return m_fallbackReason; }
    QVariantMap effectiveRuntimeConfig() const
    { return m_effectiveRuntimeConfig; }

    Q_INVOKABLE void observeWindow(QQuickWindow *window);
    Q_INVOKABLE void saveSettings(const QVariantMap &settings);
    Q_INVOKABLE void downloadSelected();
    Q_INVOKABLE void cancelSelected();

signals:
    void stateChanged();

private:
    void rebuild();
    QVariantMap providerCapability(const QString &providerId) const;
    bool providerSupported(const QString &providerId) const;
    QString selectionAssetSetId(
        const YanamiUpscaling::ResolvedProfile &profile) const;
    QVariantMap runtimeConfigFor(
        const YanamiUpscaling::ResolvedProfile &profile,
        bool includeFallbacks,
        const QString &verifiedAssetSetId = {}) const;
    QVariantMap anime4kCustom(const QVariantMap &settings) const;
    void ensureVerified(
        const YanamiUpscaling::ResolvedProfile &profile,
        const QString &selectionKey);

    QPointer<PreferencesViewModel> m_preferences;
    QPointer<UpscalingAssetManager> m_assets;
    QPointer<UpscalingCapabilityProbe> m_capabilities;
    YanamiUpscaling::ResolvedProfile m_resolved;
    QString m_selectedAssetSetId;
    QVariantList m_providers;
    QVariantMap m_selectedAssets;
    QVariantMap m_effectiveRuntimeConfig;
    QString m_fallbackReason;
    QSet<QString> m_verificationRequested;
    bool m_effectiveEnabled = false;
    bool m_rebuildInProgress = false;
    bool m_rebuildQueued = false;
};
