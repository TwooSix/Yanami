#pragma once

#include <QString>

namespace ApplicationPaths {

inline constexpr auto isolatedProfileEnvironment =
    "YANAMI_ISOLATED_PROFILE_ROOT";

struct ConfigurationResult
{
    bool succeeded = true;
    bool isolated = false;
    QString profileRoot;
    QString error;
};

// Resolves and activates the optional process-local isolated profile before
// QGuiApplication or QSettings are constructed. An invalid requested profile
// is a hard failure: silently falling back would risk touching real user data.
[[nodiscard]] ConfigurationResult configureFromEnvironment();

// Pure validation helper used by the startup path and focused tests.
[[nodiscard]] bool validateIsolatedProfileRoot(
    const QString &requestedRoot,
    QString *resolvedRoot,
    QString *error);

[[nodiscard]] bool isolated();
[[nodiscard]] QString profileRoot();
[[nodiscard]] QString dataRoot();
[[nodiscard]] QString cacheRoot();
[[nodiscard]] QString upscalingAssetRoot();
[[nodiscard]] QString logFilePath();

} // namespace ApplicationPaths
