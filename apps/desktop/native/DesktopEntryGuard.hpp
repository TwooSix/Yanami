#pragma once

#include <QString>
#include <QStringList>

namespace DesktopEntryGuard {

inline constexpr auto DirectDesktopOption = "--yanami-direct-desktop";
inline constexpr auto DirectDesktopEnvironment = "YANAMI_DIRECT_DESKTOP";

struct LaunchContext {
    bool usableBootstrapHandoff = false;
    bool explicitDirectDesktop = false;
    bool diagnosticOrPerformanceMode = false;
};

enum class Route {
    RunDesktop,
    RedirectToBootstrap,
};

[[nodiscard]] constexpr Route routeForLaunch(LaunchContext context) noexcept
{
    return context.usableBootstrapHandoff
            || context.explicitDirectDesktop
            || context.diagnosticOrPerformanceMode
        ? Route::RunDesktop
        : Route::RedirectToBootstrap;
}

enum class RedirectStatus {
    Started,
    LauncherMissing,
    StartFailed,
};

struct RedirectResult {
    RedirectStatus status = RedirectStatus::StartFailed;
    qint64 processId = 0;
    QString detail;
};

// Bootstrap handoff arguments contain a process-private path and inherited
// event handle. They must never be forwarded into a fresh launcher process.
[[nodiscard]] QStringList forwardedArguments(const QStringList &arguments);

// Starts the user-facing Yanami.exe beside yanami-desktop.exe. The caller owns
// the fallback policy when an unpackaged developer tree has no launcher.
[[nodiscard]] RedirectResult redirectToSiblingBootstrap(
    const QStringList &arguments);

} // namespace DesktopEntryGuard
