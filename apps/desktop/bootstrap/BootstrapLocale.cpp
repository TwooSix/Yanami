#include "BootstrapLocale.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace YanamiBootstrap {
namespace {

template<typename Character>
bool asciiSpace(Character character) noexcept
{
    return character == static_cast<Character>(' ')
        || character == static_cast<Character>('\t')
        || character == static_cast<Character>('\r')
        || character == static_cast<Character>('\n');
}

template<typename Character>
Character asciiLower(Character character) noexcept
{
    if (character >= static_cast<Character>('A')
        && character <= static_cast<Character>('Z')) {
        return static_cast<Character>(
            character - static_cast<Character>('A')
            + static_cast<Character>('a'));
    }
    return character;
}

template<typename Character>
UiLanguage languageFromSetting(
    std::basic_string_view<Character> setting) noexcept
{
    while (!setting.empty() && asciiSpace(setting.front()))
        setting.remove_prefix(1);
    while (!setting.empty() && asciiSpace(setting.back()))
        setting.remove_suffix(1);

    if (setting.size() == 5
        && asciiLower(setting[0]) == static_cast<Character>('z')
        && asciiLower(setting[1]) == static_cast<Character>('h')
        && (setting[2] == static_cast<Character>('_')
            || setting[2] == static_cast<Character>('-'))
        && asciiLower(setting[3]) == static_cast<Character>('c')
        && asciiLower(setting[4]) == static_cast<Character>('n')) {
        return UiLanguage::SimplifiedChinese;
    }
    return UiLanguage::English;
}

std::string trimAscii(std::string value)
{
    const auto first = std::find_if_not(
        value.begin(), value.end(),
        [](char character) { return asciiSpace(character); });
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(),
        [](char character) { return asciiSpace(character); }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

bool equalAsciiCaseInsensitive(
    std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (asciiLower(left[index]) != asciiLower(right[index]))
            return false;
    }
    return true;
}

UiLanguage languageFromIniFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return UiLanguage::English;
    const std::string contents {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return uiLanguageFromQtIni(contents);
}

#ifdef _WIN32
std::optional<std::filesystem::path> isolatedProfileRoot()
{
    constexpr wchar_t environmentName[] = L"YANAMI_ISOLATED_PROFILE_ROOT";
    const DWORD required = GetEnvironmentVariableW(
        environmentName, nullptr, 0);
    if (required == 0)
        return std::nullopt;

    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        environmentName, buffer.data(), required);
    if (copied == 0 || copied >= required)
        return std::nullopt;
    return std::filesystem::path(std::wstring(buffer.data(), copied));
}

UiLanguage languageFromRegistry()
{
    constexpr wchar_t key[] = L"Software\\Yanami\\Yanami\\ui";
    constexpr wchar_t valueName[] = L"language";
    wchar_t value[64] {};
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, key, valueName,
        RRF_RT_REG_SZ, nullptr, value, &bytes);
    if (status != ERROR_SUCCESS)
        return UiLanguage::English;
    const std::size_t characters = bytes >= sizeof(wchar_t)
        ? bytes / sizeof(wchar_t) - 1 : 0;
    return uiLanguageFromSetting(std::wstring_view(value, characters));
}
#endif

} // namespace

UiLanguage uiLanguageFromSetting(std::string_view setting) noexcept
{
    return languageFromSetting(setting);
}

UiLanguage uiLanguageFromSetting(std::wstring_view setting) noexcept
{
    return languageFromSetting(setting);
}

std::filesystem::path isolatedLanguageSettingsPath(
    const std::filesystem::path &profileRoot)
{
    return profileRoot / "settings" / "user" / "Yanami" / "Yanami.ini";
}

UiLanguage uiLanguageFromQtIni(std::string_view contents)
{
    bool inUiSection = false;
    std::size_t offset = 0;
    while (offset <= contents.size()) {
        const std::size_t lineEnd = contents.find('\n', offset);
        const std::size_t length = lineEnd == std::string_view::npos
            ? contents.size() - offset : lineEnd - offset;
        std::string line = trimAscii(std::string(contents.substr(offset, length)));
        if (line.size() >= 3
            && static_cast<unsigned char>(line[0]) == 0xef
            && static_cast<unsigned char>(line[1]) == 0xbb
            && static_cast<unsigned char>(line[2]) == 0xbf) {
            line.erase(0, 3);
        }
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            const std::string section = trimAscii(
                line.substr(1, line.size() - 2));
            inUiSection = equalAsciiCaseInsensitive(section, "ui");
        } else if (inUiSection && !line.empty()
                   && line.front() != ';' && line.front() != '#') {
            const std::size_t separator = line.find('=');
            if (separator != std::string::npos) {
                const std::string key = trimAscii(line.substr(0, separator));
                if (equalAsciiCaseInsensitive(key, "language")) {
                    std::string value = trimAscii(line.substr(separator + 1));
                    if (value.size() >= 2
                        && ((value.front() == '"' && value.back() == '"')
                            || (value.front() == '\'' && value.back() == '\''))) {
                        value = value.substr(1, value.size() - 2);
                    }
                    return uiLanguageFromSetting(value);
                }
            }
        }
        if (lineEnd == std::string_view::npos)
            break;
        offset = lineEnd + 1;
    }
    return UiLanguage::English;
}

#ifdef _WIN32
UiLanguage persistedUiLanguage()
{
    if (const auto root = isolatedProfileRoot())
        return languageFromIniFile(isolatedLanguageSettingsPath(*root));
    return languageFromRegistry();
}
#endif

} // namespace YanamiBootstrap
