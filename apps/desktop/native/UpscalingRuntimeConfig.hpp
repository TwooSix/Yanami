#pragma once

#include "UpscalingCatalog.hpp"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>

namespace YanamiUpscaling {

inline constexpr int kRuntimeConfigSchema = 1;
inline constexpr int kMaximumShaderCount = 16;
inline constexpr qint64 kMaximumShaderFileBytes = 16 * 1024 * 1024;
inline constexpr qint64 kMaximumShaderTotalBytes = 64 * 1024 * 1024;
inline constexpr int kMinimumReservedHeadroomPercent = 10;
inline constexpr int kMaximumReservedHeadroomPercent = 40;
inline constexpr int kDefaultReservedHeadroomPercent = 20;

struct ValidatedConfig
{
    bool enabled = false;
    QString providerId;
    QString profileId;
    QString modelVersion;
    BackendKind backendKind = BackendKind::None;
    QStringList orderedShaderPaths;
    QVariantMap whitelistedOptions;
    bool performanceProtection = true;
    int reservedHeadroomPercent = kDefaultReservedHeadroomPercent;
    QList<QVariantMap> fallbacks;
};

struct ValidationResult
{
    ValidatedConfig config;
    QString errorCode;

    bool isValid() const { return errorCode.isEmpty(); }
};

// Validates an untrusted, already materialized runtime configuration. Fallback
// maps are retained as opaque candidates and must be passed through this
// function individually before use.
ValidationResult validateRuntimeConfig(
    const QVariantMap &input,
    const QString &allowedAssetRoot);

} // namespace YanamiUpscaling
