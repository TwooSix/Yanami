#pragma once

#include <filesystem>
#include <string_view>

namespace YanamiBootstrap {

enum class UiLanguage {
    English,
    SimplifiedChinese,
};

[[nodiscard]] UiLanguage uiLanguageFromSetting(
    std::string_view setting) noexcept;
[[nodiscard]] UiLanguage uiLanguageFromSetting(
    std::wstring_view setting) noexcept;

[[nodiscard]] std::filesystem::path isolatedLanguageSettingsPath(
    const std::filesystem::path &profileRoot);
[[nodiscard]] UiLanguage uiLanguageFromQtIni(std::string_view contents);

#ifdef _WIN32
[[nodiscard]] UiLanguage persistedUiLanguage();
#endif

} // namespace YanamiBootstrap
