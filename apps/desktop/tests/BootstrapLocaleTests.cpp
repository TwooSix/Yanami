#include "BootstrapLocale.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

void expect(bool condition, std::string_view message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main()
{
    using YanamiBootstrap::UiLanguage;
    using YanamiBootstrap::isolatedLanguageSettingsPath;
    using YanamiBootstrap::uiLanguageFromQtIni;
    using YanamiBootstrap::uiLanguageFromSetting;

    expect(uiLanguageFromSetting("") == UiLanguage::English,
           "a missing preference must use the product English default");
    expect(uiLanguageFromSetting("en") == UiLanguage::English,
           "the English preference must remain English");
    expect(uiLanguageFromSetting("zh_CN") == UiLanguage::SimplifiedChinese,
           "the canonical Simplified Chinese preference must be recognized");
    expect(uiLanguageFromSetting(" ZH-cn ") == UiLanguage::SimplifiedChinese,
           "the compatibility spelling must be case-insensitive and trimmed");
    expect(uiLanguageFromSetting("zh_TW") == UiLanguage::English,
           "unsupported languages must use the same fallback as the desktop");
    expect(uiLanguageFromSetting(L"zh-CN") == UiLanguage::SimplifiedChinese,
           "wide registry values must follow the same normalization contract");

    expect(uiLanguageFromQtIni(
               "[playback]\nvalue=true\n\n[ui]\nlanguage=zh_CN\n")
               == UiLanguage::SimplifiedChinese,
           "the isolated QSettings INI must expose the saved language");
    expect(uiLanguageFromQtIni(
               "\xef\xbb\xbf[UI]\r\n language = \"zh-CN\" \r\n")
               == UiLanguage::SimplifiedChinese,
           "BOM, CRLF, quoting, and harmless whitespace must be accepted");
    expect(uiLanguageFromQtIni("[other]\nlanguage=zh_CN\n")
               == UiLanguage::English,
           "a similarly named key outside the UI group must be ignored");
    expect(uiLanguageFromQtIni("[ui]\nlanguage=damaged\n")
               == UiLanguage::English,
           "a damaged persisted value must fail safely to English");

    const auto path = isolatedLanguageSettingsPath(
        std::filesystem::path("profile-root"));
    expect(path == std::filesystem::path("profile-root") / "settings"
               / "user" / "Yanami" / "Yanami.ini",
           "the launcher and isolated QSettings must share the exact path");

#ifdef _WIN32
    const std::filesystem::path profileRoot =
        std::filesystem::temp_directory_path()
        / ("yanami-bootstrap-locale-tests-"
           + std::to_string(GetCurrentProcessId()));
    std::error_code cleanupError;
    std::filesystem::remove_all(profileRoot, cleanupError);
    const std::filesystem::path settingsPath =
        isolatedLanguageSettingsPath(profileRoot);
    std::filesystem::create_directories(settingsPath.parent_path());
    {
        std::ofstream settings(settingsPath, std::ios::binary);
        settings << "[ui]\nmeaning=ignored\nlanguage=zh_CN\n";
    }
    expect(SetEnvironmentVariableW(
               L"YANAMI_ISOLATED_PROFILE_ROOT",
               profileRoot.c_str()) != FALSE,
           "the test must be able to select an isolated profile");
    expect(YanamiBootstrap::persistedUiLanguage()
               == UiLanguage::SimplifiedChinese,
           "the launcher must read the actual isolated QSettings file");
    std::filesystem::remove(settingsPath);
    expect(YanamiBootstrap::persistedUiLanguage() == UiLanguage::English,
           "a fresh isolated profile must ignore the host registry and default to English");
    SetEnvironmentVariableW(L"YANAMI_ISOLATED_PROFILE_ROOT", nullptr);
    std::filesystem::remove_all(profileRoot, cleanupError);
#endif
    return EXIT_SUCCESS;
}
