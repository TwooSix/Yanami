#pragma once

#include <filesystem>
#include <string>

namespace yanami::installer {

struct ShortcutRequest {
    std::filesystem::path shortcutPath;
    std::filesystem::path target;
    std::filesystem::path workingDirectory;
    bool selected = true;
    std::wstring appUserModelId = L"io.github.TwooSix.Yanami.Yanami";
    std::wstring description = L"Yanami";
};

struct ShortcutResult {
    // A shortcut warning must not turn an otherwise successful installation
    // into an installation failure. The caller owns the completion-page UX.
    bool ok = false;
    bool changed = false;
    // The selected shortcut, or the requested canonical path on failure.
    // Empty after successful deselection.
    std::filesystem::path shortcutPath;
    std::wstring warning;
};

// Selection atomically replaces the requested canonical .lnk, including stale
// or invalid links; directories/reparse points are never replaced. Deselection
// removes only a link with this exact absolute target, not one merely sharing
// its installation directory. No alternative names are scanned or allocated.
// COM is initialized on the calling thread; no Desktop, Start-menu, registry,
// installation root, or global settings are discovered or modified implicitly.
ShortcutResult synchronizeInstallerShortcut(const ShortcutRequest& request);

} // namespace yanami::installer
