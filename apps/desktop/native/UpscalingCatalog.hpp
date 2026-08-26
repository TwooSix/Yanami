#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

namespace YanamiUpscaling {

// Stable identifiers persisted by the UI. Do not rename existing values.
namespace Id {
inline constexpr char ProviderAnime4k[] = "anime4k";

inline constexpr char BackendGlslShaders[] = "glsl-shaders";

inline constexpr char PresetPerformance[] = "performance";
inline constexpr char PresetBalanced[] = "balanced";
inline constexpr char PresetQuality[] = "quality";
inline constexpr char PresetCustom[] = "custom";
} // namespace Id

// BackendKind is deliberately independent from mpv option strings. Persisted
// configurations use the stable IDs above, while native consumers switch on
// this closed enum and construct their own backend commands.
enum class BackendKind : quint8 {
    None = 0,
    GlslShaders,
};

struct ShaderArtifact {
    QString id;
    QString providerId;
    QString version;
    QString assetSetId;
    QString fileName;
    QString installRelativePath;
    QString downloadUrl;
    qint64 sizeBytes = 0;
    QString sha256;
    QString licenseSpdx;
    QString licenseUrl;
};

struct ResolvedProfile {
    QString requestedProviderId;
    QString requestedPresetId;
    QString providerId;
    QString presetId;
    QString profileId;
    QString version;
    QString assetSetId;
    BackendKind backendKind = BackendKind::None;

    // The application maps these IDs to downloaded local files and passes the
    // paths to mpv in exactly this order. No shader payload is stored here.
    QStringList orderedShaderArtifactIds;
    QVector<ShaderArtifact> requiredArtifacts;

    // Only catalog-owned options are returned. User input is never copied into
    // this map, which makes it safe to apply at the mpv boundary.
    QVariantMap mpvOptions;
    QStringList fallbackProfileIds;

    // A complete, normalized custom configuration. Empty for standard presets.
    QVariantMap normalizedCustom;
    bool usedDefaultProvider = false;
    bool usedDefaultPreset = false;
    bool usedDefaultCustom = false;
};

class UpscalingCatalog final {
public:
    UpscalingCatalog() = delete;

    static QStringList providerIds();
    static QStringList presetIds();
    static QString backendKindId(BackendKind backendKind);
    static bool parseBackendKindId(
        const QString &serialized,
        BackendKind *backendKind);
    static const QVector<ShaderArtifact> &artifacts();
    static const ShaderArtifact *findArtifact(const QString &artifactId);

    // The catalog never probes hardware and therefore stays deterministic and
    // independent of QObject, networking, and platform APIs. Unknown provider
    // IDs fail closed; legacy provider migration belongs to Preferences.
    static ResolvedProfile resolve(
        const QString &providerId,
        const QString &presetId,
        const QVariantMap &custom = {});

    // Complete artifact closure for the selected profile and every bounded
    // runtime fallback. One user-initiated component download therefore
    // guarantees that scenario selection and live performance protection can
    // move to a lower tier without another network request.
    static QVector<ShaderArtifact> deploymentArtifacts(
        const ResolvedProfile &profile);

    static bool isWhitelistedMpvOption(const QString &name);
};

} // namespace YanamiUpscaling
