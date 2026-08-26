#include "UpscalingRuntimeConfig.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QVariantList>

#include <cmath>
#include <utility>

namespace {

using YanamiUpscaling::ValidationResult;

ValidationResult failure(const QString &errorCode)
{
    ValidationResult result;
    result.errorCode = errorCode;
    return result;
}

bool isExactBoolean(const QVariant &value)
{
    return value.metaType().id() == QMetaType::Bool;
}

bool isExactString(const QVariant &value)
{
    return value.metaType().id() == QMetaType::QString;
}

bool numericValue(const QVariant &value, double *result)
{
    const int type = value.metaType().id();
    switch (type) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        break;
    default:
        return false;
    }

    bool converted = false;
    const double convertedValue = value.toDouble(&converted);
    if (!converted || !std::isfinite(convertedValue))
        return false;
    *result = convertedValue;
    return true;
}

bool boundedInteger(
    const QVariant &value,
    int minimum,
    int maximum,
    int *result)
{
    double converted = 0.0;
    if (!numericValue(value, &converted)
        || std::trunc(converted) != converted
        || converted < static_cast<double>(minimum)
        || converted > static_cast<double>(maximum)) {
        return false;
    }
    *result = static_cast<int>(converted);
    return true;
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString normalizedCanonicalPath(const QFileInfo &info)
{
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        return {};
    return QDir::fromNativeSeparators(QDir::cleanPath(canonical));
}

bool isStrictDescendant(const QString &path, const QString &root)
{
    if (path.isEmpty() || root.isEmpty())
        return false;
    const QString prefix = root.endsWith(QLatin1Char('/'))
        ? root
        : root + QLatin1Char('/');
    return path.startsWith(prefix, pathCaseSensitivity());
}

QString duplicateKey(const QString &canonicalPath)
{
#ifdef Q_OS_WIN
    return canonicalPath.toCaseFolded();
#else
    return canonicalPath;
#endif
}

bool extractStringList(const QVariant &value, QStringList *result)
{
    const int type = value.metaType().id();
    if (type == QMetaType::QStringList) {
        *result = value.toStringList();
        return true;
    }
    if (type != QMetaType::QVariantList)
        return false;

    QStringList strings;
    const QVariantList values = value.toList();
    strings.reserve(values.size());
    for (const QVariant &entry : values) {
        if (!isExactString(entry))
            return false;
        strings.push_back(entry.toString());
    }
    *result = strings;
    return true;
}

bool validProfileId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^[a-z0-9](?:[a-z0-9_/-]{0,62}[a-z0-9])?$"));
    return value.size() <= 64
        && !value.contains(QStringLiteral("//"))
        && !value.contains(QStringLiteral(".."))
        && pattern.match(value).hasMatch();
}

bool validModelVersion(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^[A-Za-z0-9](?:[A-Za-z0-9._+-]{0,62}[A-Za-z0-9])?$"));
    return value.size() <= 64
        && !value.contains(QStringLiteral(".."))
        && pattern.match(value).hasMatch();
}

bool isResamplerOption(const QString &name)
{
    return name == QLatin1String("scale")
        || name == QLatin1String("cscale")
        || name == QLatin1String("dscale");
}

bool isBooleanOption(const QString &name)
{
    return name == QLatin1String("sigmoid-upscaling")
        || name == QLatin1String("correct-downscaling")
        || name == QLatin1String("linear-downscaling");
}

bool isAntiringOption(const QString &name)
{
    return name == QLatin1String("scale-antiring")
        || name == QLatin1String("cscale-antiring");
}

bool allowedResampler(const QString &value)
{
    static const QSet<QString> allowed {
        QStringLiteral("bilinear"),
        QStringLiteral("bicubic_fast"),
        QStringLiteral("oversample"),
        QStringLiteral("spline16"),
        QStringLiteral("spline36"),
        QStringLiteral("spline64"),
        QStringLiteral("lanczos"),
        QStringLiteral("ewa_lanczos"),
        QStringLiteral("ewa_lanczossharp"),
        QStringLiteral("mitchell"),
        QStringLiteral("catmull_rom"),
    };
    return allowed.contains(value);
}

ValidationResult validateShaders(
    const QVariantMap &input,
    const QString &allowedAssetRoot,
    YanamiUpscaling::ValidatedConfig *validated)
{
    if (input.contains(QStringLiteral("orderedShaderPaths"))
        && input.contains(QStringLiteral("shaderPaths"))) {
        return failure(QStringLiteral("shader_paths_ambiguous"));
    }

    const QString pathKey = input.contains(QStringLiteral("orderedShaderPaths"))
        ? QStringLiteral("orderedShaderPaths")
        : QStringLiteral("shaderPaths");
    QStringList shaderPaths;
    if (!input.contains(pathKey)
        || !extractStringList(input.value(pathKey), &shaderPaths)
        || shaderPaths.isEmpty()
        || shaderPaths.size() > YanamiUpscaling::kMaximumShaderCount) {
        return failure(QStringLiteral("shader_paths_invalid"));
    }

    const QFileInfo rootInfo(QDir::cleanPath(allowedAssetRoot));
    if (!rootInfo.isAbsolute() || !rootInfo.exists() || !rootInfo.isDir())
        return failure(QStringLiteral("asset_root_invalid"));
    const QString canonicalRoot = normalizedCanonicalPath(rootInfo);
    if (canonicalRoot.isEmpty())
        return failure(QStringLiteral("asset_root_invalid"));
    const QFileInfo providerRootInfo(
        QDir(canonicalRoot).filePath(validated->providerId));
    if (!providerRootInfo.exists() || !providerRootInfo.isDir())
        return failure(QStringLiteral("provider_asset_root_invalid"));
    const QString canonicalProviderRoot = normalizedCanonicalPath(
        providerRootInfo);
    if (canonicalProviderRoot.isEmpty()
        || !isStrictDescendant(canonicalProviderRoot, canonicalRoot)) {
        return failure(QStringLiteral("provider_asset_root_invalid"));
    }

    QSet<QString> seen;
    qint64 totalBytes = 0;
    QStringList canonicalPaths;
    canonicalPaths.reserve(shaderPaths.size());
    for (const QString &shaderPath : shaderPaths) {
        const QFileInfo requestedInfo(QDir::cleanPath(shaderPath));
        if (!requestedInfo.isAbsolute())
            return failure(QStringLiteral("shader_path_not_absolute"));
        if (!requestedInfo.fileName().endsWith(
                QStringLiteral(".glsl"), Qt::CaseInsensitive)) {
            return failure(QStringLiteral("shader_extension_invalid"));
        }
        if (!requestedInfo.exists())
            return failure(QStringLiteral("shader_file_missing"));
        if (!requestedInfo.isFile())
            return failure(QStringLiteral("shader_file_not_regular"));

        const QString canonicalPath = normalizedCanonicalPath(requestedInfo);
        if (canonicalPath.isEmpty())
            return failure(QStringLiteral("shader_file_missing"));
        if (!isStrictDescendant(canonicalPath, canonicalRoot))
            return failure(QStringLiteral("shader_path_escape"));
        if (!isStrictDescendant(canonicalPath, canonicalProviderRoot))
            return failure(QStringLiteral("shader_provider_mismatch"));

        const QFileInfo canonicalInfo(canonicalPath);
        if (!canonicalInfo.exists())
            return failure(QStringLiteral("shader_file_missing"));
        if (!canonicalInfo.isFile())
            return failure(QStringLiteral("shader_file_not_regular"));
        if (!canonicalInfo.fileName().endsWith(
                QStringLiteral(".glsl"), Qt::CaseInsensitive)) {
            return failure(QStringLiteral("shader_extension_invalid"));
        }

        const QString key = duplicateKey(canonicalPath);
        if (seen.contains(key))
            return failure(QStringLiteral("shader_path_duplicate"));
        seen.insert(key);

        const qint64 size = canonicalInfo.size();
        if (size < 0 || size > YanamiUpscaling::kMaximumShaderFileBytes)
            return failure(QStringLiteral("shader_file_too_large"));
        totalBytes += size;
        if (totalBytes > YanamiUpscaling::kMaximumShaderTotalBytes)
            return failure(QStringLiteral("shader_total_too_large"));
        canonicalPaths.push_back(canonicalPath);
    }

    validated->orderedShaderPaths = canonicalPaths;
    return {};
}

ValidationResult validateOptions(
    const QVariantMap &input,
    YanamiUpscaling::ValidatedConfig *validated)
{
    if (!input.contains(QStringLiteral("options")))
        return {};
    const QVariant optionsValue = input.value(QStringLiteral("options"));
    if (optionsValue.metaType().id() != QMetaType::QVariantMap)
        return failure(QStringLiteral("options_invalid"));

    QVariantMap options;
    const QVariantMap supplied = optionsValue.toMap();
    for (auto iterator = supplied.cbegin(); iterator != supplied.cend(); ++iterator) {
        const QString &name = iterator.key();
        const QVariant &value = iterator.value();
        if (isResamplerOption(name)) {
            if (!isExactString(value) || !allowedResampler(value.toString()))
                return failure(QStringLiteral("option_value_invalid"));
            options.insert(name, value.toString());
            continue;
        }
        if (isBooleanOption(name)) {
            if (!isExactBoolean(value))
                return failure(QStringLiteral("option_value_invalid"));
            options.insert(name, value.toBool());
            continue;
        }
        if (isAntiringOption(name)) {
            double amount = 0.0;
            if (!numericValue(value, &amount) || amount < 0.0 || amount > 1.0)
                return failure(QStringLiteral("option_value_invalid"));
            options.insert(name, amount);
            continue;
        }
        return failure(QStringLiteral("option_not_allowed"));
    }

    validated->whitelistedOptions = options;
    return {};
}

ValidationResult validateBackend(
    const QVariantMap &input,
    YanamiUpscaling::ValidatedConfig *validated)
{
    using YanamiUpscaling::BackendKind;

    // Schema-1 shader configurations predate the backend field. Anime4K is
    // the only supported provider and always runs through GLSL shaders.
    if (!input.contains(QStringLiteral("backend"))) {
        validated->backendKind = BackendKind::GlslShaders;
        return {};
    }

    const QVariant backendValue = input.value(QStringLiteral("backend"));
    if (backendValue.metaType().id() != QMetaType::QVariantMap)
        return failure(QStringLiteral("backend_invalid"));
    const QVariantMap backend = backendValue.toMap();
    const QVariant kindValue = backend.value(QStringLiteral("kind"));
    if (!isExactString(kindValue))
        return failure(QStringLiteral("backend_kind_invalid"));

    const QString kind = kindValue.toString();
    if (kind == QLatin1String(YanamiUpscaling::Id::BackendGlslShaders)) {
        if (backend.size() != 1)
            return failure(QStringLiteral("backend_options_invalid"));
        validated->backendKind = BackendKind::GlslShaders;
        return {};
    }

    return failure(QStringLiteral("backend_kind_invalid"));
}

ValidationResult validateFallbacks(
    const QVariantMap &input,
    YanamiUpscaling::ValidatedConfig *validated)
{
    if (!input.contains(QStringLiteral("fallbacks")))
        return {};
    const QVariant fallbackValue = input.value(QStringLiteral("fallbacks"));
    if (fallbackValue.metaType().id() != QMetaType::QVariantList)
        return failure(QStringLiteral("fallbacks_invalid"));

    const QVariantList supplied = fallbackValue.toList();
    if (supplied.size() > 3)
        return failure(QStringLiteral("fallbacks_too_many"));
    for (const QVariant &entry : supplied) {
        if (entry.metaType().id() != QMetaType::QVariantMap)
            return failure(QStringLiteral("fallbacks_invalid"));
        validated->fallbacks.push_back(entry.toMap());
    }
    return {};
}

} // namespace

namespace YanamiUpscaling {

ValidationResult validateRuntimeConfig(
    const QVariantMap &input,
    const QString &allowedAssetRoot)
{
    if (input.isEmpty())
        return {};

    int schema = 0;
    if (!input.contains(QStringLiteral("schema"))
        || !boundedInteger(input.value(QStringLiteral("schema")),
            kRuntimeConfigSchema, kRuntimeConfigSchema, &schema)) {
        return failure(QStringLiteral("schema_invalid"));
    }

    const QVariant enabledValue = input.value(QStringLiteral("enabled"));
    if (!input.contains(QStringLiteral("enabled"))
        || !isExactBoolean(enabledValue)) {
        return failure(QStringLiteral("enabled_invalid"));
    }
    if (!enabledValue.toBool())
        return {};

    ValidatedConfig validated;
    validated.enabled = true;

    const QVariant providerValue = input.value(QStringLiteral("providerId"));
    if (!isExactString(providerValue))
        return failure(QStringLiteral("provider_not_allowed"));
    validated.providerId = providerValue.toString();
    if (validated.providerId != QLatin1String(Id::ProviderAnime4k)) {
        return failure(QStringLiteral("provider_not_allowed"));
    }

    const QVariant profileValue = input.value(QStringLiteral("profileId"));
    if (!isExactString(profileValue)
        || !validProfileId(profileValue.toString())) {
        return failure(QStringLiteral("profile_id_invalid"));
    }
    validated.profileId = profileValue.toString();
    if (!validated.profileId.startsWith(QStringLiteral("anime4k/")))
        return failure(QStringLiteral("profile_provider_mismatch"));

    const QVariant versionValue = input.value(QStringLiteral("modelVersion"));
    if (!isExactString(versionValue)
        || !validModelVersion(versionValue.toString())) {
        return failure(QStringLiteral("model_version_invalid"));
    }
    validated.modelVersion = versionValue.toString();

    if (input.contains(QStringLiteral("performanceProtection"))) {
        const QVariant protection = input.value(
            QStringLiteral("performanceProtection"));
        if (!isExactBoolean(protection))
            return failure(QStringLiteral("performance_protection_invalid"));
        validated.performanceProtection = protection.toBool();
    }

    if (input.contains(QStringLiteral("reservedHeadroomPercent"))
        && input.contains(QStringLiteral("headroom"))) {
        return failure(QStringLiteral("headroom_ambiguous"));
    }
    const QString headroomKey = input.contains(
        QStringLiteral("reservedHeadroomPercent"))
        ? QStringLiteral("reservedHeadroomPercent")
        : QStringLiteral("headroom");
    if (input.contains(headroomKey)
        && !boundedInteger(input.value(headroomKey),
            kMinimumReservedHeadroomPercent,
            kMaximumReservedHeadroomPercent,
            &validated.reservedHeadroomPercent)) {
        return failure(QStringLiteral("headroom_invalid"));
    }

    ValidationResult step = validateBackend(input, &validated);
    if (!step.isValid())
        return step;
    if (validated.backendKind != BackendKind::GlslShaders)
        return failure(QStringLiteral("backend_invalid"));
    step = validateShaders(input, allowedAssetRoot, &validated);
    if (!step.isValid())
        return step;
    step = validateOptions(input, &validated);
    if (!step.isValid())
        return step;
    step = validateFallbacks(input, &validated);
    if (!step.isValid())
        return step;

    ValidationResult result;
    result.config = std::move(validated);
    return result;
}

} // namespace YanamiUpscaling
