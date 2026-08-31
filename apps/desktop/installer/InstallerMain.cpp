#include "InstallerCore.hpp"
#include "InstallerLayout.hpp"
#include "InstallerPainter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace yanami::installer;
using yanami::installer::ui::TextAlign;

constexpr wchar_t kWindowClass[] = L"YanamiInstallerWindow";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\io.github.TwooSix.Yanami";
constexpr wchar_t kAumid[] = L"io.github.TwooSix.Yanami.Yanami";
constexpr UINT kInstallFinishedMessage = WM_APP + 41;
constexpr UINT kRenderRecoveryMessage = WM_APP + 43;
constexpr UINT_PTR kProgressTimer = 42;
constexpr int kIconResource = 101;
constexpr int kBaselineClientWidth = 680;
constexpr int kBaselineClientHeight = 560;
constexpr DWORD kWindowStyle =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

constexpr COLORREF kBackground = RGB(0x09, 0x0b, 0x10);
constexpr COLORREF kPanel = RGB(0x15, 0x18, 0x20);
constexpr COLORREF kField = RGB(0x1b, 0x1e, 0x27);
constexpr COLORREF kForeground = RGB(0xf7, 0xf8, 0xfc);
constexpr COLORREF kMuted = RGB(0xa2, 0xaa, 0xbc);
constexpr COLORREF kAccent = RGB(0xff, 0x66, 0x87);
constexpr COLORREF kOutline = RGB(0x2d, 0x31, 0x3d);
constexpr COLORREF kError = RGB(0xff, 0x7d, 0x94);

enum ControlId : int {
    IdContinue = 1001,
    IdBack = 1002,
    IdBrowse = 1003,
    IdInstall = 1004,
    IdInstallPath = 1005,
    IdStartMenu = 1006,
    IdDesktop = 1007,
    IdOpenFolder = 1008,
    IdFinish = 1009,
    IdLater = 1010,
};

enum class UiPage {
    Welcome,
    Options,
    Installing,
    Complete,
};

enum class UiLanguage {
    English,
    SimplifiedChinese,
};

struct InstallRequest {
    std::wstring selfPath;
    std::wstring installDirectory;
    std::optional<std::wstring> registeredInstall;
    PathPolicyRoots roots;
    std::wstring localAppData;
    bool startMenu = true;
    bool desktop = false;
    bool launchAfterInstall = false;
};

struct InstallOutcome {
    bool success = false;
    std::wstring installDirectory;
    std::wstring launchTarget;
    std::wstring error;
    std::wstring preservedLog;
};

template<typename T>
class ComPointer final {
public:
    ComPointer() = default;
    ~ComPointer() {
        if (value_ != nullptr) {
            value_->Release();
        }
    }
    ComPointer(const ComPointer&) = delete;
    ComPointer& operator=(const ComPointer&) = delete;
    [[nodiscard]] T* get() const noexcept { return value_; }
    [[nodiscard]] T** put() noexcept {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
        return &value_;
    }
    T* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    T* value_ = nullptr;
};

class WinHandle final {
public:
    explicit WinHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~WinHandle() {
        if (valid()) {
            CloseHandle(value_);
        }
    }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void close() noexcept {
        if (valid()) {
            CloseHandle(value_);
            value_ = nullptr;
        }
    }

private:
    HANDLE value_;
};

UiLanguage detectLanguage() {
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) != 0) {
        const std::wstring locale(localeName);
        if (_wcsnicmp(locale.c_str(), L"zh-CN", 5) == 0
            || _wcsnicmp(locale.c_str(), L"zh-SG", 5) == 0
            || _wcsnicmp(locale.c_str(), L"zh-Hans", 7) == 0) {
            return UiLanguage::SimplifiedChinese;
        }
    }
    const LANGID language = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(language) == LANG_CHINESE
        && (SUBLANGID(language) == SUBLANG_CHINESE_SIMPLIFIED
            || SUBLANGID(language) == SUBLANG_CHINESE_SINGAPORE)) {
        return UiLanguage::SimplifiedChinese;
    }
    return UiLanguage::English;
}

const wchar_t* text(
    UiLanguage language,
    const wchar_t* simplifiedChinese,
    const wchar_t* english) {
    return language == UiLanguage::SimplifiedChinese
        ? simplifiedChinese : english;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(count, L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(count, '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result.append("\\\\"); break;
        case '"': result.append("\\\""); break;
        case '\b': result.append("\\b"); break;
        case '\f': result.append("\\f"); break;
        case '\n': result.append("\\n"); break;
        case '\r': result.append("\\r"); break;
        case '\t': result.append("\\t"); break;
        default:
            if (character < 0x20) {
                char escaped[7]{};
                wsprintfA(escaped, "\\u%04x", character);
                result.append(escaped);
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    return result;
}

void writeHandleUtf8(DWORD standardHandle, const std::string& value) {
    const HANDLE handle = GetStdHandle(standardHandle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(handle, value.data(), static_cast<DWORD>(value.size()),
              &written, nullptr);
}

std::wstring modulePath() {
    std::vector<wchar_t> buffer(1024);
    while (buffer.size() < 32768) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}

std::optional<std::wstring> knownFolder(REFKNOWNFOLDERID identifier) {
    wchar_t* value = nullptr;
    if (FAILED(SHGetKnownFolderPath(identifier, KF_FLAG_DEFAULT, nullptr,
                                    &value))) {
        return std::nullopt;
    }
    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
}

std::optional<std::wstring> readRegisteredInstallLocation() {
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation",
        RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes <= sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(
        HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation",
        RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value.empty() ? std::nullopt
                         : std::optional<std::wstring>(std::move(value));
}

PathPolicyRoots pathPolicyRoots() {
    PathPolicyRoots roots;
    std::array<wchar_t, 32768> windows{};
    const UINT windowsLength = GetWindowsDirectoryW(
        windows.data(), static_cast<UINT>(windows.size()));
    if (windowsLength > 0 && windowsLength < windows.size()) {
        roots.windowsDirectory.assign(windows.data(), windowsLength);
    }
    roots.programFiles = knownFolder(FOLDERID_ProgramFiles).value_or(L"");
    roots.programFilesX86 = knownFolder(FOLDERID_ProgramFilesX86).value_or(L"");
    return roots;
}

std::wstring defaultInstallDirectory() {
    const auto local = knownFolder(FOLDERID_LocalAppData);
    if (!local.has_value()) {
        return {};
    }
    return (std::filesystem::path(*local) / L"Programs" / L"Yanami").wstring();
}

std::wstring localizedCoreError(UiLanguage language, const std::wstring& error) {
    if (language == UiLanguage::English || error.empty()) {
        return error;
    }
    struct Mapping { const wchar_t* english; const wchar_t* chinese; };
    static constexpr Mapping mappings[] = {
        {L"The installation directory cannot be empty.", L"安装位置不能为空。"},
        {L"The installation directory must be an absolute path on a local drive.", L"安装位置必须是本机磁盘上的绝对路径。"},
        {L"The installation directory cannot contain an alternate data stream.", L"安装位置不能包含备用数据流。"},
        {L"Installation directory names cannot end with a space or period.", L"目录名不能以空格或句点结尾。"},
        {L"A drive root cannot be used as the installation directory.", L"不能把磁盘根目录作为安装位置。"},
        {L"An existing Yanami installation must be upgraded or repaired in its registered directory.", L"检测到现有 Yanami；升级或修复必须使用已注册的位置。"},
        {L"The existing installation directory is not accessible.", L"现有安装位置不可访问。"},
        {L"Yanami cannot be installed inside Windows or Program Files.", L"不能安装到 Windows 或 Program Files 系统目录。"},
        {L"The installation directory is occupied by a file.", L"安装位置已被同名文件占用。"},
        {L"A new installation directory must be empty.", L"新安装位置必须为空目录。"},
        {L"The installation directory cannot be inspected.", L"无法检查安装位置。"},
        {L"The installation directory cannot be inspected for write access.", L"无法检查安装位置的写入权限。"},
        {L"The installation directory has no writable parent.", L"安装位置没有可写入的上级目录。"},
        {L"The installation directory parent is not accessible.", L"无法访问安装位置的上级目录。"},
        {L"The current user cannot create the installation directory.", L"当前用户无法创建安装目录。"},
        {L"The current user cannot write to the installation directory.", L"当前用户无法写入安装目录。"},
        {L"The installation directory write probe failed.", L"安装目录的写入检查失败。"},
        {L"The installation directory write probe could not be rolled back safely.", L"无法安全回滚安装目录的写入检查。"},
        {L"A private temporary installer directory could not be created.", L"无法创建安全的临时安装目录。"},
        {L"The Velopack backend could not be opened securely.", L"无法安全打开 Velopack 安装后端。"},
        {L"The Velopack backend could not be started.", L"无法启动 Velopack 安装后端。"},
        {L"Waiting for the Velopack backend failed.", L"等待 Velopack 安装后端时出错。"},
        {L"The Velopack backend exit code could not be read.", L"无法读取 Velopack 安装后端的退出状态。"},
        {L"The shortcut directory could not be created.", L"无法创建快捷方式目录。"},
        {L"Windows Shell Link could not be initialized.", L"无法初始化 Windows 快捷方式服务。"},
        {L"The Yanami shortcut properties could not be set.", L"无法设置 Yanami 快捷方式的属性。"},
        {L"The Yanami shortcut AppUserModelID store is unavailable.", L"无法访问 Yanami 快捷方式的应用标识存储。"},
        {L"The Yanami shortcut AppUserModelID could not be written.", L"无法写入 Yanami 快捷方式的应用标识。"},
        {L"The Yanami shortcut could not be saved.", L"无法保存 Yanami 快捷方式。"},
        {L"Yanami.lnk already exists but does not belong to this Yanami installation.", L"同名 Yanami.lnk 不属于本次安装，为避免覆盖已停止操作。"},
        {L"The unselected Yanami shortcut could not be removed.", L"无法移除未选中的 Yanami 快捷方式。"},
        {L"Windows could not resolve the shortcut folders.", L"Windows 无法确定快捷方式目录。"},
        {L"Velopack completed but current\\Yanami.exe is missing.", L"Velopack 完成后未找到 current\\Yanami.exe。"},
        {L"COM could not be initialized for shortcut creation.", L"无法初始化创建快捷方式所需的 Windows 组件。"},
        {L"Installer payload footer is missing", L"安装包缺少 payload footer。"},
        {L"Installer payload footer has an invalid size", L"安装包 footer 大小无效。"},
        {L"Installer payload footer magic is missing", L"安装包 footer 标识无效。"},
        {L"Installer payload format is unsupported", L"安装包 payload 格式不受支持。"},
        {L"Installer payload footer length is invalid", L"安装包 footer 长度无效。"},
        {L"Installer payload is empty", L"安装包 payload 为空。"},
        {L"Installer payload size exceeds the executable bounds", L"安装包 payload 大小越界。"},
        {L"Embedded Velopack backend SHA-256 does not match the footer", L"内置 Velopack backend 的 SHA-256 校验失败。"},
    };
    for (const Mapping& mapping : mappings) {
        if (error == mapping.english) {
            return mapping.chinese;
        }
    }
    if (error.starts_with(L"Unable to normalize the installation directory")) {
        return L"无法规范化安装位置。";
    }
    if (error.starts_with(L"The registered installation directory is invalid")) {
        return L"Windows 中记录的现有安装位置无效。";
    }
    if (error.starts_with(L"The Velopack backend failed with exit code")) {
        return L"Velopack 安装后端执行失败，详细信息已写入诊断日志。";
    }
    if (error.starts_with(L"Unable to ")
        || error.starts_with(L"Installer payload ")
        || error.starts_with(L"BCrypt")) {
        return L"安装包校验或解压失败，请重新下载安装包后再试。";
    }
    return L"安装未完成。请重试；若问题持续，请查看诊断日志。";
}

bool validateInstallDirectory(
    const std::wstring& candidate,
    const std::optional<std::wstring>& registered,
    const PathPolicyRoots& roots,
    PathValidationResult& result) {
    std::wstring normalizeError;
    const auto normalized = normalizeAbsolutePath(candidate, normalizeError);
    const ExistingPathState state = normalized.has_value()
        ? inspectExistingPath(*normalized)
        : ExistingPathState::Missing;
    result = validateInstallPathPolicy(candidate, registered, roots, state);
    return result.ok;
}

bool rollbackWritableProbe(
    const std::wstring& directory,
    std::wstring& error) {
    std::vector<std::filesystem::path> missing;
    std::filesystem::path cursor(directory);
    std::error_code filesystemError;
    while (!std::filesystem::exists(cursor, filesystemError)) {
        if (filesystemError) {
            error = L"The installation directory cannot be inspected for write access.";
            return false;
        }
        missing.push_back(cursor);
        const std::filesystem::path parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) {
            error = L"The installation directory has no writable parent.";
            return false;
        }
        cursor = parent;
    }
    if (!std::filesystem::is_directory(cursor, filesystemError)
        || filesystemError) {
        error = L"The installation directory parent is not accessible.";
        return false;
    }
    const auto removeCreatedDirectories = [&missing]() {
        bool removed = true;
        // missing is recorded deepest-first, which is the safe removal order.
        for (const auto& created : missing) {
            if (GetFileAttributesW(created.c_str()) != INVALID_FILE_ATTRIBUTES
                && !RemoveDirectoryW(created.c_str())) {
                removed = false;
            }
        }
        return removed;
    };
    if (!std::filesystem::create_directories(directory, filesystemError)
        && filesystemError) {
        if (!removeCreatedDirectories()) {
            error = L"The installation directory write probe could not be rolled back safely.";
        } else {
            error = L"The current user cannot create the installation directory.";
        }
        return false;
    }

    const std::wstring probePath =
        (std::filesystem::path(directory)
         / (L".yanami-write-probe-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()))).wstring();
    bool probeSucceeded = false;
    {
        WinHandle probe(CreateFileW(
            probePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (!probe.valid()) {
            error = L"The current user cannot write to the installation directory.";
        } else {
            constexpr char marker[] = "Yanami installer write probe\n";
            DWORD written = 0;
            if (!WriteFile(
                    probe.get(), marker, sizeof(marker) - 1, &written, nullptr)
                || written != sizeof(marker) - 1) {
                error = L"The installation directory write probe failed.";
            } else {
                probeSucceeded = true;
            }
        }
    }

    if (!removeCreatedDirectories()) {
        error = L"The installation directory write probe could not be rolled back safely.";
        return false;
    }
    return probeSucceeded;
}

bool createPrivateDirectory(const std::filesystem::path& directory) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    WinHandle token(rawToken);
    DWORD tokenBytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &tokenBytes);
    if (tokenBytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    std::vector<std::uint8_t> tokenBuffer(tokenBytes);
    if (!GetTokenInformation(
            token.get(), TokenUser, tokenBuffer.data(), tokenBytes,
            &tokenBytes)) {
        return false;
    }
    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(
        tokenBuffer.data());
    wchar_t* sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText)
        || sidText == nullptr) {
        return false;
    }
    const std::wstring sid(sidText);
    LocalFree(sidText);

    // Protect the directory from other interactive users and make the ACEs
    // inheritable so the extracted backend and its log have the same boundary.
    const std::wstring sddl = L"O:" + sid
        + L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)
        || descriptor == nullptr) {
        return false;
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;
    const bool created = CreateDirectoryW(directory.c_str(), &security) != FALSE;
    LocalFree(descriptor);
    if (!created) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        RemoveDirectoryW(directory.c_str());
        return false;
    }
    WinHandle directoryHandle(CreateFileW(
        directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directoryHandle.valid()) {
        RemoveDirectoryW(directory.c_str());
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            directoryHandle.get(), FileAttributeTagInfo, &tag, sizeof(tag))
        || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        directoryHandle.close();
        RemoveDirectoryW(directory.c_str());
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> createUniqueTempDirectory() {
    std::array<wchar_t, 32768> tempBuffer{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(tempBuffer.size()), tempBuffer.data());
    if (length == 0 || length >= tempBuffer.size()) {
        return std::nullopt;
    }
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return std::nullopt;
    }
    wchar_t guidText[64]{};
    StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText)));
    std::wstring name = L"Yanami-Installer-";
    for (const wchar_t character : std::wstring(guidText)) {
        if (character != L'{' && character != L'}') {
            name.push_back(character);
        }
    }
    const std::filesystem::path result =
        std::filesystem::path(tempBuffer.data()) / name;
    if (!createPrivateDirectory(result)) {
        return std::nullopt;
    }
    return result;
}

void cleanupTemporaryDirectory(const std::filesystem::path& directory) {
    DeleteFileW((directory / L"Velopack-Setup.exe").c_str());
    DeleteFileW((directory / L"velopack-install.log").c_str());
    RemoveDirectoryW(directory.c_str());
}

bool appendUtf8File(const std::filesystem::path& path, const std::string& value) {
    WinHandle file(CreateFileW(
        path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }
    DWORD written = 0;
    return WriteFile(file.get(), value.data(), static_cast<DWORD>(value.size()),
                     &written, nullptr)
        && written == value.size();
}

std::wstring preserveFailureLog(
    const std::filesystem::path& temporaryLog,
    const std::wstring& localAppData,
    const std::wstring& outerError) {
    if (localAppData.empty()) {
        return {};
    }
    const std::filesystem::path logDirectory =
        std::filesystem::path(localAppData) / L"Yanami" / L"InstallerLogs";
    std::error_code error;
    std::filesystem::create_directories(logDirectory, error);
    if (error) {
        return {};
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[96]{};
    swprintf_s(
        name, L"Yanami-Installer-%04u%02u%02u-%02u%02u%02u-%lu.log",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        GetCurrentProcessId());
    const std::filesystem::path destination = logDirectory / name;
    if (std::filesystem::exists(temporaryLog)
        && !CopyFileW(temporaryLog.c_str(), destination.c_str(), FALSE)) {
        return {};
    }
    const std::string detail = "\r\nYanami installer: "
        + wideToUtf8(outerError) + "\r\n";
    if (!appendUtf8File(destination, detail)) {
        return {};
    }
    return destination.wstring();
}

bool runBackend(
    const std::filesystem::path& backend,
    const std::wstring& installDirectory,
    const std::filesystem::path& log,
    WinHandle& verifiedBackendLock,
    std::wstring& error) {
    std::wstring command = quoteWindowsArgument(backend.wstring());
    command.append(L" --silent --installto ");
    command.append(quoteWindowsArgument(installDirectory));
    command.append(L" --log ");
    command.append(quoteWindowsArgument(log.wstring()));
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            backend.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
            backend.parent_path().c_str(), &startup, &process)) {
        error = L"The Velopack backend could not be started.";
        return false;
    }
    // CreateProcess has opened/mapped the verified image. Releasing only now
    // preserves the verification-to-execution boundary without preventing the
    // backend from performing its own normal cleanup while it runs.
    verifiedBackendLock.close();
    WinHandle processHandle(process.hProcess);
    WinHandle threadHandle(process.hThread);
    if (WaitForSingleObject(processHandle.get(), INFINITE) != WAIT_OBJECT_0) {
        error = L"Waiting for the Velopack backend failed.";
        return false;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
        error = L"The Velopack backend exit code could not be read.";
        return false;
    }
    if (exitCode != 0) {
        error = L"The Velopack backend failed with exit code "
            + std::to_wstring(exitCode) + L".";
        return false;
    }
    return true;
}

bool loadShortcutTarget(
    const std::filesystem::path& shortcut,
    std::wstring& target) {
    ComPointer<IShellLinkW> link;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(link.put())))) {
        return false;
    }
    ComPointer<IPersistFile> persist;
    if (FAILED(link->QueryInterface(
            IID_IPersistFile, reinterpret_cast<void**>(persist.put())))
        || FAILED(persist->Load(shortcut.c_str(), STGM_READ))) {
        return false;
    }
    std::array<wchar_t, 32768> buffer{};
    WIN32_FIND_DATAW data{};
    if (FAILED(link->GetPath(
            buffer.data(), static_cast<int>(buffer.size()), &data,
            SLGP_RAWPATH))) {
        return false;
    }
    target.assign(buffer.data());
    return !target.empty();
}

bool createShortcut(
    const std::filesystem::path& shortcut,
    const std::filesystem::path& target,
    const std::filesystem::path& workingDirectory,
    std::wstring& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(shortcut.parent_path(), filesystemError);
    if (filesystemError) {
        error = L"The shortcut directory could not be created.";
        return false;
    }
    ComPointer<IShellLinkW> link;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(link.put())))) {
        error = L"Windows Shell Link could not be initialized.";
        return false;
    }
    if (FAILED(link->SetPath(target.c_str()))
        || FAILED(link->SetWorkingDirectory(workingDirectory.c_str()))
        || FAILED(link->SetIconLocation(target.c_str(), 0))
        || FAILED(link->SetDescription(L"Yanami"))) {
        error = L"The Yanami shortcut properties could not be set.";
        return false;
    }
    ComPointer<IPropertyStore> properties;
    if (FAILED(link->QueryInterface(
            IID_IPropertyStore, reinterpret_cast<void**>(properties.put())))) {
        error = L"The Yanami shortcut AppUserModelID store is unavailable.";
        return false;
    }
    static constexpr PROPERTYKEY appUserModelId = {
        {0x9F4C2855, 0x9F79, 0x4B39,
         {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
        5};
    PROPVARIANT value{};
    value.vt = VT_LPWSTR;
    value.pwszVal = const_cast<wchar_t*>(kAumid);
    if (FAILED(properties->SetValue(appUserModelId, value))
        || FAILED(properties->Commit())) {
        error = L"The Yanami shortcut AppUserModelID could not be written.";
        return false;
    }
    ComPointer<IPersistFile> persist;
    if (FAILED(link->QueryInterface(
            IID_IPersistFile, reinterpret_cast<void**>(persist.put())))
        || FAILED(persist->Save(shortcut.c_str(), TRUE))) {
        error = L"The Yanami shortcut could not be saved.";
        return false;
    }
    return true;
}

bool synchronizeShortcut(
    const std::filesystem::path& shortcut,
    bool selected,
    const std::filesystem::path& installRoot,
    const std::filesystem::path& target,
    const std::filesystem::path& workingDirectory,
    std::wstring& error) {
    if (std::filesystem::exists(shortcut)) {
        std::wstring existingTarget;
        std::wstring normalizeError;
        const auto normalizedRoot = normalizeAbsolutePath(
            installRoot.wstring(), normalizeError);
        const auto normalizedTarget = loadShortcutTarget(shortcut, existingTarget)
            ? normalizeAbsolutePath(existingTarget, normalizeError)
            : std::nullopt;
        if (!normalizedRoot.has_value() || !normalizedTarget.has_value()
            || !pathWithin(*normalizedTarget, *normalizedRoot, true)) {
            error = L"Yanami.lnk already exists but does not belong to this Yanami installation.";
            return false;
        }
        if (!selected) {
            if (!DeleteFileW(shortcut.c_str())) {
                error = L"The unselected Yanami shortcut could not be removed.";
                return false;
            }
            return true;
        }
    } else if (!selected) {
        return true;
    }
    return createShortcut(shortcut, target, workingDirectory, error);
}

bool synchronizeShortcuts(
    const InstallRequest& request,
    const std::filesystem::path& currentDirectory,
    const std::filesystem::path& target,
    std::wstring& error) {
    const auto roaming = knownFolder(FOLDERID_RoamingAppData);
    const auto desktop = knownFolder(FOLDERID_Desktop);
    if (!roaming.has_value() || !desktop.has_value()) {
        error = L"Windows could not resolve the shortcut folders.";
        return false;
    }
    const std::filesystem::path startShortcut =
        std::filesystem::path(*roaming) / L"Microsoft" / L"Windows"
        / L"Start Menu" / L"Programs" / L"Yanami.lnk";
    const std::filesystem::path desktopShortcut =
        std::filesystem::path(*desktop) / L"Yanami.lnk";
    const std::filesystem::path root(request.installDirectory);
    return synchronizeShortcut(
               startShortcut, request.startMenu, root, target,
               currentDirectory, error)
        && synchronizeShortcut(
               desktopShortcut, request.desktop, root, target,
               currentDirectory, error);
}

InstallOutcome performInstall(const InstallRequest& request) {
    InstallOutcome outcome;
    PathValidationResult validation;
    if (!validateInstallDirectory(
            request.installDirectory, request.registeredInstall,
            request.roots, validation)) {
        outcome.error = validation.error;
        return outcome;
    }
    outcome.installDirectory = validation.normalizedPath;

    if (!rollbackWritableProbe(validation.normalizedPath, outcome.error)) {
        return outcome;
    }
    const auto tempDirectory = createUniqueTempDirectory();
    if (!tempDirectory.has_value()) {
        outcome.error = L"A private temporary installer directory could not be created.";
        return outcome;
    }
    const std::filesystem::path backend =
        *tempDirectory / L"Velopack-Setup.exe";
    const std::filesystem::path temporaryLog =
        *tempDirectory / L"velopack-install.log";

    const PayloadResult extracted = extractVerifiedPayload(
        request.selfPath, backend.wstring());
    if (!extracted.ok) {
        outcome.error = extracted.error;
    } else {
        // Deny write/delete sharing, then re-hash the exact on-disk backend.
        // Keeping this handle alive through CreateProcess closes the gap where
        // a verified executable could otherwise be replaced before launch.
        WinHandle backendLock(CreateFileW(
            backend.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!backendLock.valid()) {
            outcome.error = L"The Velopack backend could not be opened securely.";
        } else if (!verifyExtractedPayloadHandle(
                       backendLock.get(), extracted.metadata, outcome.error)) {
            // The handle verifier supplied the error.
        } else if (!runBackend(
                       backend, validation.normalizedPath, temporaryLog,
                       backendLock,
                       outcome.error)) {
            // runBackend supplied the error.
        } else {
            const std::filesystem::path current =
                std::filesystem::path(validation.normalizedPath) / L"current";
            const std::filesystem::path target = current / L"Yanami.exe";
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES
                || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                outcome.error = L"Velopack completed but current\\Yanami.exe is missing.";
            } else {
                const HRESULT initialized = CoInitializeEx(
                    nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                const bool shouldUninitialize = SUCCEEDED(initialized);
                if (initialized == RPC_E_CHANGED_MODE || SUCCEEDED(initialized)) {
                    if (synchronizeShortcuts(
                            request, current, target, outcome.error)) {
                        outcome.success = true;
                        outcome.launchTarget = target.wstring();
                    }
                } else {
                    outcome.error = L"COM could not be initialized for shortcut creation.";
                }
                if (shouldUninitialize) {
                    CoUninitialize();
                }
            }
        }
    }

    if (!outcome.success) {
        outcome.preservedLog = preserveFailureLog(
            temporaryLog, request.localAppData, outcome.error);
    }
    cleanupTemporaryDirectory(*tempDirectory);

    if (outcome.success && request.launchAfterInstall) {
        ShellExecuteW(
            nullptr, L"open", outcome.launchTarget.c_str(), nullptr,
            std::filesystem::path(outcome.launchTarget).parent_path().c_str(),
            SW_SHOWNORMAL);
    }
    return outcome;
}

bool isHighContrast() {
    HIGHCONTRASTW highContrast{};
    highContrast.cbSize = sizeof(highContrast);
    return SystemParametersInfoW(
        SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0)
        && (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

class InstallerWindow final {
public:
    InstallerWindow(
        UiLanguage language,
        std::wstring selfPath,
        std::wstring localAppData,
        PathPolicyRoots roots,
        std::optional<std::wstring> registeredInstall)
        : language_(language),
          selfPath_(std::move(selfPath)),
          localAppData_(std::move(localAppData)),
          roots_(std::move(roots)),
          registeredInstall_(std::move(registeredInstall)) {
        installDirectory_ = registeredInstall_.value_or(defaultInstallDirectory());
    }

    ~InstallerWindow() {
        destroyPageIcons();
        destroyFontsAndBrushes();
    }

    bool create(HINSTANCE instance) {
        instance_ = instance;
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &InstallerWindow::windowProcedure;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIconW(
            instance, MAKEINTRESOURCEW(kIconResource));
        windowClass.hIconSm = windowClass.hIcon;
        appIcon_ = windowClass.hIcon;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClass;
        if (RegisterClassExW(&windowClass) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const wchar_t* title = text(
            language_, L"Yanami 安装程序", L"Yanami Installer");
        dpi_ = initialDpi();
        const SIZE outerSize = baselineWindowSize(dpi_);
        window_ = CreateWindowExW(
            0, kWindowClass, title,
            kWindowStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy,
            nullptr, nullptr, instance, this);
        if (window_ == nullptr) {
            return false;
        }
        enableModernWindowBehavior();
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        return true;
    }

    HWND handle() const noexcept { return window_; }

    bool preprocessKeyboardMessage(const MSG& message) {
        if (message.message == WM_LBUTTONDOWN
            || message.message == WM_RBUTTONDOWN
            || message.message == WM_POINTERDOWN) {
            setKeyboardFocusVisible(false);
        }
        if (message.message != WM_KEYDOWN) {
            return false;
        }
        if (message.wParam == VK_TAB || message.wParam == VK_LEFT
            || message.wParam == VK_RIGHT || message.wParam == VK_UP
            || message.wParam == VK_DOWN || message.wParam == VK_SPACE) {
            setKeyboardFocusVisible(true);
        }
        if (message.wParam == VK_ESCAPE) {
            SendMessageW(window_, WM_COMMAND, IDCANCEL, 0);
            return true;
        }
        if (message.wParam != VK_RETURN) {
            return false;
        }
        if (imeComposing_ && GetFocus() == pathEdit_) {
            // Candidate confirmation belongs to the edit control, not the
            // installer's default action or IsDialogMessage's Enter handling.
            TranslateMessage(&message);
            DispatchMessageW(&message);
            return true;
        }
        if ((message.lParam & (static_cast<LPARAM>(1) << 30)) != 0) {
            return true;
        }
        if (installing_) {
            MessageBeep(MB_ICONINFORMATION);
            return true;
        }

        const HWND focused = GetFocus();
        std::array<wchar_t, 32> className{};
        if (focused != nullptr && IsChild(window_, focused)
            && IsWindowEnabled(focused)
            && GetClassNameW(
                   focused, className.data(),
                   static_cast<int>(className.size())) > 0
            && _wcsicmp(className.data(), WC_BUTTONW) == 0) {
            SendMessageW(focused, BM_CLICK, 0, 0);
            return true;
        }

        HWND primary = nullptr;
        switch (page_) {
        case UiPage::Welcome:
            primary = GetDlgItem(window_, IdContinue);
            break;
        case UiPage::Options:
            primary = installButton_;
            break;
        case UiPage::Complete:
            primary = GetDlgItem(window_, IdFinish);
            break;
        case UiPage::Installing:
            break;
        }
        if (primary != nullptr && IsWindowEnabled(primary)) {
            SendMessageW(primary, BM_CLICK, 0, 0);
        } else {
            MessageBeep(MB_ICONINFORMATION);
        }
        return true;
    }

private:
    static LRESULT CALLBACK windowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        InstallerWindow* self = reinterpret_cast<InstallerWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<InstallerWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self != nullptr
            ? self->handleMessage(message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), 96);
    }

    static UINT initialDpi() {
        using GetDpiForSystemFunction = UINT(WINAPI*)();
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            const auto getDpi = reinterpret_cast<GetDpiForSystemFunction>(
                GetProcAddress(user32, "GetDpiForSystem"));
            if (getDpi != nullptr) {
                return std::max<UINT>(96, getDpi());
            }
        }
        HDC screen = GetDC(nullptr);
        const UINT dpi = screen != nullptr
            ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
        if (screen != nullptr) {
            ReleaseDC(nullptr, screen);
        }
        return std::max<UINT>(96, dpi);
    }

    static SIZE baselineWindowSize(UINT dpi) {
        RECT rectangle{
            0, 0,
            MulDiv(kBaselineClientWidth, static_cast<int>(dpi), 96),
            MulDiv(kBaselineClientHeight, static_cast<int>(dpi), 96)};
        using AdjustForDpiFunction = BOOL(WINAPI*)(
            LPRECT, DWORD, BOOL, DWORD, UINT);
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        const auto adjustForDpi = user32 != nullptr
            ? reinterpret_cast<AdjustForDpiFunction>(
                  GetProcAddress(user32, "AdjustWindowRectExForDpi"))
            : nullptr;
        if (adjustForDpi != nullptr) {
            adjustForDpi(&rectangle, kWindowStyle, FALSE, 0, dpi);
        } else {
            AdjustWindowRectEx(&rectangle, kWindowStyle, FALSE, 0);
        }
        return SIZE{
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top};
    }

    void createFontsAndBrushes() {
        destroyPageIcons();
        destroyFontsAndBrushes();
        const auto& resolvedFace = painter_.fontFamily(isChinese());
        const wchar_t* face = resolvedFace.empty()
            ? L"Segoe UI" : resolvedFace.c_str();
        normalFont_ = CreateFontW(
            -scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        titleFont_ = CreateFontW(
            -scale(30), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        smallFont_ = CreateFontW(
            -scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        buttonFont_ = CreateFontW(
            -scale(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        backgroundBrush_ = CreateSolidBrush(
            highContrast_ ? GetSysColor(COLOR_WINDOW) : kBackground);
        panelBrush_ = CreateSolidBrush(highContrast_ ? GetSysColor(COLOR_WINDOW) : kPanel);
        fieldBrush_ = CreateSolidBrush(highContrast_ ? GetSysColor(COLOR_WINDOW) : kField);
        welcomeIcon_ = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON,
            scale(96), scale(96), LR_DEFAULTCOLOR));
        installingIcon_ = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON,
            scale(72), scale(72), LR_DEFAULTCOLOR));
    }

    void destroyPageIcons() {
        for (HICON* icon : {&welcomeIcon_, &installingIcon_}) {
            if (*icon != nullptr) {
                DestroyIcon(*icon);
                *icon = nullptr;
            }
        }
    }

    void destroyFontsAndBrushes() {
        for (HFONT* font : {
                 &normalFont_, &titleFont_, &smallFont_, &buttonFont_}) {
            if (*font != nullptr) {
                DeleteObject(*font);
                *font = nullptr;
            }
        }
        for (HBRUSH* brush : {&backgroundBrush_, &panelBrush_, &fieldBrush_}) {
            if (*brush != nullptr) {
                DeleteObject(*brush);
                *brush = nullptr;
            }
        }
    }

    void enableModernWindowBehavior() {
        highContrast_ = isHighContrast();
        dpi_ = GetDpiForWindow(window_);
        const SIZE outerSize = baselineWindowSize(dpi_);
        SetWindowPos(
            window_, nullptr, 0, 0, outerSize.cx, outerSize.cy,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        createFontsAndBrushes();
        if (!highContrast_) {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
            int corner = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute(window_, 33, &corner, sizeof(corner));
        }
        renderPage();
    }

    enum class ButtonSurface : INT_PTR {
        Root = 0,
        Panel = 1,
        Field = 2,
    };

    RECT logicalRect(int left, int top, int width, int height) const {
        return RECT{
            scale(left), scale(top), scale(left + width), scale(top + height)};
    }

    bool isChinese() const noexcept {
        return language_ == UiLanguage::SimplifiedChinese;
    }

    float fontPixels(int logicalPixels) const noexcept {
        return static_cast<float>(logicalPixels) * static_cast<float>(dpi_) / 96.0f;
    }

    void setKeyboardFocusVisible(bool visible) {
        if (keyboardFocusVisible_ == visible) {
            return;
        }
        keyboardFocusVisible_ = visible;
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void requestRenderRecovery() const {
        renderHadFailure_ = true;
        if (!renderRecoveryQueued_ && !renderUnavailable_) {
            renderRecoveryQueued_ = true;
            PostMessageW(window_, kRenderRecoveryMessage, 0, 0);
        }
    }

    bool beginDrawing(HDC device, const RECT& bounds) const {
        if (painter_.begin(device, bounds)) {
            return true;
        }
        requestRenderRecovery();
        return false;
    }

    void finishDrawing() const {
        if (!painter_.end()) {
            requestRenderRecovery();
        }
    }

    void drawRoundedSurface(
        const RECT& rectangle,
        COLORREF fill,
        COLORREF outline,
        int radius,
        int penWidth = 1) const {
        painter_.roundedRect(rectangle, static_cast<float>(scale(radius)),
                             fill, outline, static_cast<float>(scale(penWidth)));
    }

    static void beginMouseTracking(HWND control) {
        if (GetPropW(control, L"YanamiHot") != nullptr) {
            return;
        }
        SetPropW(control, L"YanamiHot", reinterpret_cast<HANDLE>(
            static_cast<INT_PTR>(1)));
        TRACKMOUSEEVENT tracking{
            sizeof(tracking), TME_LEAVE, control, HOVER_DEFAULT};
        TrackMouseEvent(&tracking);
        InvalidateRect(control, nullptr, TRUE);
    }

    static LRESULT CALLBACK buttonSubclassProcedure(
        HWND control,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR) {
        switch (message) {
        case WM_MOUSEMOVE:
            beginMouseTracking(control);
            break;
        case WM_MOUSELEAVE:
            RemovePropW(control, L"YanamiHot");
            InvalidateRect(control, nullptr, TRUE);
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(control, nullptr, TRUE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(
                control, buttonSubclassProcedure, subclassId);
            break;
        default:
            break;
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    static LRESULT CALLBACK editSubclassProcedure(
        HWND control,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData) {
        auto* self = reinterpret_cast<InstallerWindow*>(referenceData);
        if (self != nullptr && message == WM_IME_STARTCOMPOSITION) {
            self->imeComposing_ = true;
        } else if (self != nullptr && (message == WM_IME_ENDCOMPOSITION
                                     || message == WM_KILLFOCUS
                                     || message == WM_NCDESTROY)) {
            self->imeComposing_ = false;
        }
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS
            || message == WM_ENABLE) {
            InvalidateRect(GetParent(control), nullptr, FALSE);
        } else if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, editSubclassProcedure, subclassId);
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    void drawSwitch(HWND control, HDC device) const {
        RECT client{};
        GetClientRect(control, &client);
        if (!beginDrawing(device, client)) {
            return;
        }
        painter_.clear(kPanel);
        const bool enabled = IsWindowEnabled(control) != FALSE;
        const bool hot = GetPropW(control, L"YanamiHot") != nullptr;
        const bool focused = keyboardFocusVisible_ && GetFocus() == control;
        const bool checked = SendMessageW(
            control, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool pressed = (SendMessageW(
            control, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;

        RECT row = client;
        InflateRect(&row, -scale(1), -scale(1));
        if (hot || pressed || focused) {
            const COLORREF rowFill = pressed ? RGB(0x20, 0x23, 0x2d)
                : (hot ? kField : kPanel);
            drawRoundedSurface(row, rowFill, focused ? kAccent : rowFill, 10);
        }

        std::array<wchar_t, 256> title{};
        GetWindowTextW(
            control, title.data(), static_cast<int>(title.size()));
        const auto* subtitle = reinterpret_cast<const wchar_t*>(
            GetPropW(control, L"YanamiSwitchSubtitle"));
        RECT titleRect{
            scale(8), scale(3), client.right - scale(88), scale(27)};
        RECT subtitleRect{
            scale(8), scale(27), client.right - scale(88), scale(49)};
        painter_.text(title.data(), titleRect, fontPixels(14),
                      false, enabled ? kForeground : RGB(0x72, 0x78, 0x86),
                      TextAlign::Left, true, false, true, isChinese());
        if (subtitle != nullptr) {
            painter_.text(subtitle, subtitleRect, fontPixels(13),
                          false, enabled ? kMuted : RGB(0x72, 0x78, 0x86),
                          TextAlign::Left, true, false, true, isChinese());
        }

        RECT track{
            client.right - scale(60),
            (client.bottom - scale(28)) / 2,
            client.right - scale(8),
            (client.bottom + scale(28)) / 2};
        const COLORREF trackFill = !enabled
            ? RGB(0x2a, 0x2d, 0x36)
            : (checked ? kAccent : RGB(0x31, 0x35, 0x41));
        drawRoundedSurface(
            track, trackFill,
            checked && enabled ? kAccent : RGB(0x42, 0x47, 0x55),
            14);
        const int thumbSize = scale(20);
        const int thumbTop = track.top + (track.bottom - track.top - thumbSize) / 2;
        const int thumbLeft = checked
            ? track.right - scale(4) - thumbSize
            : track.left + scale(4);
        painter_.ellipse(RECT{thumbLeft, thumbTop,
                             thumbLeft + thumbSize, thumbTop + thumbSize},
                         enabled ? kForeground : RGB(0x83, 0x88, 0x94));
        finishDrawing();
    }

    static LRESULT CALLBACK switchSubclassProcedure(
        HWND control,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData) {
        auto* self = reinterpret_cast<InstallerWindow*>(referenceData);
        switch (message) {
        case WM_MOUSEMOVE:
            beginMouseTracking(control);
            break;
        case WM_MOUSELEAVE:
            RemovePropW(control, L"YanamiHot");
            InvalidateRect(control, nullptr, TRUE);
            break;
        case WM_ERASEBKGND:
            if (self != nullptr && !self->highContrast_) {
                return 1;
            }
            break;
        case WM_PAINT:
            if (self != nullptr && !self->highContrast_) {
                PAINTSTRUCT paint{};
                HDC device = BeginPaint(control, &paint);
                self->drawSwitch(control, device);
                EndPaint(control, &paint);
                return 0;
            }
            break;
        case WM_PRINTCLIENT:
            if (self != nullptr && !self->highContrast_) {
                self->drawSwitch(control, reinterpret_cast<HDC>(wParam));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
        case WM_KEYUP: {
            const LRESULT result = DefSubclassProc(
                control, message, wParam, lParam);
            InvalidateRect(control, nullptr, TRUE);
            return result;
        }
        case BM_SETCHECK: {
            const LRESULT result = DefSubclassProc(
                control, message, wParam, lParam);
            InvalidateRect(control, nullptr, TRUE);
            return result;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(control, nullptr, TRUE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(
                control, switchSubclassProcedure, subclassId);
            break;
        default:
            break;
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    void drawStaticText(HWND control, HDC device) const {
        RECT client{};
        GetClientRect(control, &client);
        if (!beginDrawing(device, client)) {
            return;
        }
        const auto surface = static_cast<ButtonSurface>(
            reinterpret_cast<INT_PTR>(GetPropW(control, L"YanamiTextSurface")));
        painter_.clear(surface == ButtonSurface::Panel ? kPanel
            : (surface == ButtonSurface::Field ? kField : kBackground));
        const bool error = GetPropW(control, L"YanamiError") != nullptr;
        const bool muted = GetPropW(control, L"YanamiMuted") != nullptr;
        const HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(control, WM_GETFONT, 0, 0));
        const int size = font == titleFont_ ? 30 : (font == smallFont_ ? 13 : 14);
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(control, GWL_STYLE));
        const DWORD alignment = style & SS_TYPEMASK;
        const TextAlign align = alignment == SS_CENTER ? TextAlign::Center
            : (alignment == SS_RIGHT ? TextAlign::Right : TextAlign::Left);
        const bool ellipsis = (style & SS_ELLIPSISMASK) != 0;
        const bool centered = (style & SS_CENTERIMAGE) != 0;
        painter_.text(editText(control), client, fontPixels(size),
                      font == titleFont_, error ? kError : (muted ? kMuted : kForeground),
                      align, centered, !centered && !ellipsis, ellipsis, isChinese(),
                      (style & SS_ELLIPSISMASK) == SS_PATHELLIPSIS);
        finishDrawing();
    }

    static LRESULT CALLBACK textSubclassProcedure(
        HWND control, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData) {
        auto* self = reinterpret_cast<InstallerWindow*>(referenceData);
        if (self != nullptr && !self->highContrast_) {
            if (message == WM_ERASEBKGND) {
                return 1;
            }
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{};
                HDC device = BeginPaint(control, &paint);
                self->drawStaticText(control, device);
                EndPaint(control, &paint);
                return 0;
            }
            if (message == WM_PRINTCLIENT) {
                self->drawStaticText(control, reinterpret_cast<HDC>(wParam));
                return 0;
            }
            if (message == WM_SETTEXT || message == WM_SETFONT) {
                // STATIC may draw synchronously inside its default handler.
                // Repaint through DirectWrite after updating the native value.
                const LRESULT result = DefSubclassProc(control, message, wParam, lParam);
                RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                return result;
            }
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, textSubclassProcedure, subclassId);
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    HWND addControl(
        const wchar_t* className,
        const wchar_t* label,
        DWORD style,
        int identifier,
        int x, int y, int width, int height,
        DWORD extendedStyle = 0,
        HFONT font = nullptr,
        bool initiallyVisible = true) {
        HWND control = CreateWindowExW(
            extendedStyle, className, label,
            WS_CHILD | (initiallyVisible ? WS_VISIBLE : 0) | style,
            scale(x), scale(y), scale(width), scale(height),
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            instance_, nullptr);
        SendMessageW(
            control, WM_SETFONT,
            reinterpret_cast<WPARAM>(font != nullptr ? font : normalFont_), TRUE);
        return control;
    }

    HWND addButton(
        const wchar_t* label, int identifier,
        int x, int y, int width, bool primary = false,
        ButtonSurface surface = ButtonSurface::Root,
        bool folderIcon = false) {
        HWND button = addControl(
            WC_BUTTONW, label, WS_TABSTOP | BS_OWNERDRAW,
            identifier, x, y, width, folderIcon ? 36 : 44, 0, buttonFont_, false);
        SetPropW(button, L"YanamiPrimary", reinterpret_cast<HANDLE>(
            static_cast<INT_PTR>(primary ? 1 : 0)));
        SetPropW(button, L"YanamiButtonSurface", reinterpret_cast<HANDLE>(
            static_cast<INT_PTR>(surface)));
        if (folderIcon) {
            SetPropW(button, L"YanamiFolderIcon", reinterpret_cast<HANDLE>(
                static_cast<INT_PTR>(1)));
        }
        SetWindowSubclass(
            button, buttonSubclassProcedure, 1,
            reinterpret_cast<DWORD_PTR>(this));
        ShowWindow(button, SW_SHOWNOACTIVATE);
        return button;
    }

    void addBrowseTooltip(const wchar_t* label) {
        browseTooltip_ = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            window_, nullptr, instance_, nullptr);
        if (browseTooltip_ == nullptr) {
            return;
        }
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(browseButton_);
        tool.lpszText = const_cast<wchar_t*>(label);
        SendMessageW(browseTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(browseTooltip_, TTM_SETMAXTIPWIDTH, 0, scale(260));
    }

    void centerPathEdit() {
        // Match the real native edit font, including CJK ascent/descent and DPI,
        // instead of leaving top-aligned text in a fixed-height child window.
        TEXTMETRICW metrics{};
        HDC device = GetDC(pathEdit_);
        bool measured = false;
        if (device != nullptr) {
            HGDIOBJ previous = SelectObject(device, normalFont_);
            measured = GetTextMetricsW(device, &metrics) != FALSE;
            SelectObject(device, previous);
            ReleaseDC(pathEdit_, device);
        }
        const RECT bounds = ui::centeredTextLineRect(
            logicalRect(80, 172, 476, 52), measured ? metrics.tmHeight : scale(20));
        SetWindowPos(pathEdit_, nullptr, bounds.left, bounds.top,
                     bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HWND addSwitch(
        const wchar_t* title,
        const wchar_t* subtitle,
        int identifier,
        int y,
        bool checked) {
        HWND control = addControl(
            WC_BUTTONW, title, WS_TABSTOP | BS_AUTOCHECKBOX,
            identifier, 76, y, 528, 52, 0, nullptr, false);
        SetPropW(
            control, L"YanamiSwitchSubtitle",
            reinterpret_cast<HANDLE>(const_cast<wchar_t*>(subtitle)));
        SetWindowSubclass(
            control, switchSubclassProcedure, 1,
            reinterpret_cast<DWORD_PTR>(this));
        if (!highContrast_) {
            SetWindowTheme(control, L"", L"");
        }
        SendMessageW(
            control, BM_SETCHECK,
            checked ? BST_CHECKED : BST_UNCHECKED, 0);
        ShowWindow(control, SW_SHOWNOACTIVATE);
        return control;
    }

    HWND addText(
        const wchar_t* label, int x, int y, int width, int height,
        HFONT font = nullptr, DWORD style = SS_LEFT) {
        HWND control = addControl(
            WC_STATICW, label, style, 0, x, y, width, height, 0, font, false);
        SetWindowSubclass(control, textSubclassProcedure, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        if (font == smallFont_) {
            SetPropW(control, L"YanamiMuted", reinterpret_cast<HANDLE>(
                static_cast<INT_PTR>(1)));
        }
        ShowWindow(control, SW_SHOWNOACTIVATE);
        InvalidateRect(control, nullptr, FALSE);
        return control;
    }

    void destroyChildren() {
        if (browseTooltip_ != nullptr) {
            DestroyWindow(browseTooltip_);
            browseTooltip_ = nullptr;
        }
        std::vector<HWND> children;
        EnumChildWindows(
            window_,
            [](HWND child, LPARAM parameter) -> BOOL {
                reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(child);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&children));
        for (HWND child : children) {
            DestroyWindow(child);
        }
        pathEdit_ = nullptr;
        pathReadOnlyText_ = nullptr;
        pathErrorText_ = nullptr;
        browseButton_ = nullptr;
        installButton_ = nullptr;
        startMenuCheck_ = nullptr;
        desktopCheck_ = nullptr;
    }

    void captureControlState() {
        if (pathEdit_ != nullptr) {
            installDirectory_ = editText(pathEdit_);
        }
        if (startMenuCheck_ != nullptr) {
            startMenu_ = SendMessageW(
                startMenuCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
        if (desktopCheck_ != nullptr) {
            desktop_ = SendMessageW(
                desktopCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
    }

    void renderPage() {
        captureControlState();
        KillTimer(window_, kProgressTimer);
        destroyChildren();
        switch (page_) {
        case UiPage::Welcome: renderWelcome(); break;
        case UiPage::Options: renderOptions(); break;
        case UiPage::Installing:
            progressPhase_ = 0;
            SetTimer(window_, kProgressTimer, 35, nullptr);
            renderInstalling();
            break;
        case UiPage::Complete: renderComplete(); break;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void renderWelcome() {
        addText(
            text(language_, L"步骤 1 / 2", L"Step 1 of 2"),
            500, 24, 116, 22, smallFont_, SS_RIGHT);
        addText(
            text(language_, L"安装 Yanami", L"Install Yanami"),
            64, 198, 552, 52, titleFont_, SS_CENTER);
        addText(
            text(language_,
                 L"选择安装位置和快捷方式，之后可在应用内直接更新。",
                 L"Choose the install location and shortcuts. Future updates happen directly in the app."),
            80, 256, 520, 48, normalFont_, SS_CENTER);
        addText(
            text(language_,
                 L"仅为当前用户安装 · 无需管理员权限",
                 L"Current user only  ·  No administrator access"),
            64, 310, 552, 26, smallFont_, SS_CENTER);
        addButton(text(language_, L"取消", L"Cancel"), IDCANCEL, 376, 488, 96);
        HWND next = addButton(
            text(language_, L"继续", L"Continue"), IdContinue,
            484, 488, 132, true);
        SetFocus(next);
    }

    void renderOptions() {
        addText(
            text(language_, L"步骤 2 / 2", L"Step 2 of 2"),
            500, 24, 116, 22, smallFont_, SS_RIGHT);
        addText(
            text(language_, L"选择安装方式", L"Choose how Yanami is installed"),
            64, 50, 500, 46, titleFont_);
        addText(
            registeredInstall_.has_value()
                ? text(language_,
                       L"已检测到现有安装，将在原位置更新或修复。",
                       L"An existing installation will be updated or repaired in place.")
                : text(language_,
                       L"确认安装位置和快捷方式。",
                       L"Confirm the install location and shortcuts."),
            64, 100, 552, 30, normalFont_);
        addText(text(language_, L"安装位置", L"Installation directory"),
                64, 144, 300, 22, smallFont_);
        if (registeredInstall_.has_value()) {
            pathReadOnlyText_ = addText(
                installDirectory_.c_str(), 80, 184, 516, 28,
                normalFont_, SS_PATHELLIPSIS | SS_CENTERIMAGE);
            SetPropW(pathReadOnlyText_, L"YanamiTextSurface",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(
                         ButtonSurface::Field)));
        } else {
            pathEdit_ = addControl(
                WC_EDITW, installDirectory_.c_str(),
                WS_TABSTOP | ES_AUTOHSCROLL,
                IdInstallPath, 80, 172, 476, 52, 0, nullptr, false);
            SetWindowSubclass(
                pathEdit_, editSubclassProcedure, 1,
                reinterpret_cast<DWORD_PTR>(this));
            if (!highContrast_) {
                SetWindowTheme(pathEdit_, L"", L"");
            }
            centerPathEdit();
            ShowWindow(pathEdit_, SW_SHOWNOACTIVATE);
            const wchar_t* browseLabel = text(
                language_, L"选择安装文件夹", L"Choose installation folder");
            browseButton_ = addButton(
                browseLabel, IdBrowse, 568, 180, 36, false, ButtonSurface::Field, true);
            addBrowseTooltip(browseLabel);
        }
        pathErrorText_ = addText(L"", 64, 230, 552, 28, smallFont_);
        SetPropW(pathErrorText_, L"YanamiError", reinterpret_cast<HANDLE>(
            static_cast<INT_PTR>(1)));

        startMenuCheck_ = addSwitch(
            text(language_, L"开始菜单", L"Start menu"),
            text(language_,
                 L"在“所有应用”中显示，之后可手动固定",
                 L"Show in All apps; you can pin it later"),
            IdStartMenu, 269, startMenu_);
        desktopCheck_ = addSwitch(
            text(language_, L"桌面快捷方式", L"Desktop shortcut"),
            text(language_,
                 L"在桌面创建快捷方式",
                 L"Create a shortcut on the desktop"),
            IdDesktop, 335, desktop_);

        addText(
            text(language_,
                 L"可在 Windows 设置中随时卸载",
                 L"You can uninstall anytime from Windows Settings"),
            64, 469, 300, 22, smallFont_);
        addButton(text(language_, L"返回", L"Back"), IdBack, 376, 488, 96);
        installButton_ = addButton(
            registeredInstall_.has_value()
                ? text(language_, L"更新 / 修复", L"Update / Repair")
                : text(language_, L"安装 Yanami", L"Install Yanami"),
            IdInstall, 484, 488, 132, true);
        updatePathValidationUi();
        if (pathValid_) {
            SetFocus(installButton_);
        } else if (pathEdit_ != nullptr) {
            SetFocus(pathEdit_);
        }
    }

    void renderInstalling() {
        addText(
            text(language_, L"正在安装 Yanami", L"Installing Yanami"),
            64, 196, 552, 50, titleFont_, SS_CENTER);
        addText(
            text(language_,
                 L"正在验证并安装应用文件…",
                 L"Verifying and installing application files…"),
            64, 254, 552, 30, normalFont_, SS_CENTER);
        addText(
            installDirectory_.c_str(), 112, 356, 456, 28,
            smallFont_, SS_CENTER | SS_PATHELLIPSIS);
    }

    void renderComplete() {
        addText(
            text(language_, L"Yanami 已准备就绪", L"Yanami is ready"),
            64, 168, 552, 50, titleFont_, SS_CENTER);
        HWND installedLabel = addText(
            text(language_, L"安装位置", L"Installed at"),
            84, 254, 160, 22, smallFont_);
        HWND installedPath = addText(
            installDirectory_.c_str(), 84, 282, 376, 28,
            normalFont_, SS_PATHELLIPSIS | SS_CENTERIMAGE);
        for (HWND control : {installedLabel, installedPath}) {
            SetPropW(control, L"YanamiTextSurface", reinterpret_cast<HANDLE>(
                static_cast<INT_PTR>(ButtonSurface::Panel)));
        }
        addButton(
            text(language_, L"打开目录", L"Open folder"),
            IdOpenFolder, 480, 266, 116, false, ButtonSurface::Panel);
        addText(
            text(language_,
                 L"卸载：Windows 设置 → 应用 → 已安装的应用",
                 L"Uninstall from Windows Settings → Apps → Installed apps"),
            64, 360, 552, 32, smallFont_, SS_CENTER);
        addButton(
            text(language_, L"稍后", L"Later"), IdLater,
            376, 488, 96);
        HWND launch = addButton(
            text(language_, L"启动 Yanami", L"Launch Yanami"), IdFinish,
            484, 488, 132, true);
        SetFocus(launch);
    }

    void updatePathValidationUi() {
        if (pathEdit_ != nullptr) {
            installDirectory_ = editText(pathEdit_);
        }
        PathValidationResult validation;
        pathValid_ = validateInstallDirectory(
            installDirectory_, registeredInstall_, roots_, validation);
        pathValidationMessage_ = pathValid_
            ? std::wstring{} : localizedCoreError(language_, validation.error);
        if (pathErrorText_ != nullptr) {
            SetWindowTextW(
                pathErrorText_, pathValidationMessage_.c_str());
        }
        if (installButton_ != nullptr) {
            EnableWindow(installButton_, pathValid_ ? TRUE : FALSE);
        }
        if (window_ != nullptr) {
            const RECT field = logicalRect(62, 170, 556, 56);
            InvalidateRect(window_, &field, FALSE);
        }
    }

    void browseForDirectory() {
        ComPointer<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(
                CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_IFileOpenDialog,
                reinterpret_cast<void**>(dialog.put())))) {
            showError(text(language_, L"无法打开目录选择器。", L"The folder picker could not be opened."));
            return;
        }
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
                           | FOS_PATHMUSTEXIST);
        ComPointer<IShellItem> initial;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                installDirectory_.c_str(), nullptr, IID_IShellItem,
                reinterpret_cast<void**>(initial.put())))) {
            dialog->SetFolder(initial.get());
        }
        if (FAILED(dialog->Show(window_))) {
            return;
        }
        ComPointer<IShellItem> selected;
        if (FAILED(dialog->GetResult(selected.put()))) {
            return;
        }
        wchar_t* path = nullptr;
        if (SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))
            && path != nullptr) {
            std::filesystem::path preview(path);
            std::wstring normalizeError;
            const auto normalized = normalizeAbsolutePath(
                preview.wstring(), normalizeError);
            if (normalized.has_value()
                && inspectExistingPath(*normalized)
                    == ExistingPathState::NonEmptyDirectory
                && _wcsicmp(preview.filename().c_str(), L"Yanami") != 0) {
                preview /= L"Yanami";
            }
            installDirectory_ = preview.wstring();
            if (pathEdit_ != nullptr) {
                SetWindowTextW(pathEdit_, installDirectory_.c_str());
                SendMessageW(
                    pathEdit_, EM_SETSEL,
                    installDirectory_.size(), installDirectory_.size());
            }
            CoTaskMemFree(path);
            updatePathValidationUi();
        }
    }

    std::wstring editText(HWND edit) const {
        const int length = GetWindowTextLengthW(edit);
        std::wstring value(length + 1, L'\0');
        GetWindowTextW(edit, value.data(), length + 1);
        value.resize(length);
        return value;
    }

    void beginInstall() {
        if (pathEdit_ != nullptr) {
            installDirectory_ = editText(pathEdit_);
        }
        startMenu_ = SendMessageW(startMenuCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        desktop_ = SendMessageW(desktopCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        PathValidationResult validation;
        if (!validateInstallDirectory(
                installDirectory_, registeredInstall_, roots_, validation)) {
            pathValid_ = false;
            pathValidationMessage_ = localizedCoreError(
                language_, validation.error);
            if (pathErrorText_ != nullptr) {
                SetWindowTextW(
                    pathErrorText_, pathValidationMessage_.c_str());
            }
            if (installButton_ != nullptr) {
                EnableWindow(installButton_, FALSE);
            }
            InvalidateRect(window_, nullptr, FALSE);
            if (pathEdit_ != nullptr) {
                SetFocus(pathEdit_);
            }
            return;
        }
        installDirectory_ = validation.normalizedPath;
        page_ = UiPage::Installing;
        installing_ = true;
        renderPage();

        InstallRequest request;
        request.selfPath = selfPath_;
        request.installDirectory = installDirectory_;
        request.registeredInstall = registeredInstall_;
        request.roots = roots_;
        request.localAppData = localAppData_;
        request.startMenu = startMenu_;
        request.desktop = desktop_;
        request.launchAfterInstall = false;
        const HWND destination = window_;
        std::thread([request = std::move(request), destination]() mutable {
            auto outcome = std::make_unique<InstallOutcome>(performInstall(request));
            if (!PostMessageW(
                    destination, kInstallFinishedMessage, 0,
                    reinterpret_cast<LPARAM>(outcome.get()))) {
                return;
            }
            outcome.release();
        }).detach();
    }

    void showError(const std::wstring& message) const {
        MessageBoxW(
            window_, message.c_str(),
            text(language_, L"Yanami 安装程序", L"Yanami Installer"),
            MB_OK | MB_ICONERROR);
    }

    void handleInstallFinished(std::unique_ptr<InstallOutcome> outcome) {
        installing_ = false;
        if (!outcome->success) {
            page_ = UiPage::Options;
            renderPage();
            std::wstring message = localizedCoreError(language_, outcome->error);
            if (!outcome->preservedLog.empty()) {
                message.append(text(
                    language_, L"\n\n诊断日志已保存到：\n",
                    L"\n\nA diagnostic log was saved to:\n"));
                message.append(outcome->preservedLog);
            }
            showError(message);
            if (renderUnavailable_) {
                DestroyWindow(window_);
            }
            return;
        }
        installDirectory_ = outcome->installDirectory;
        launchTarget_ = outcome->launchTarget;
        if (renderUnavailable_) {
            const std::wstring result = std::wstring(text(language_,
                L"Yanami 已安装到：\n", L"Yanami was installed at:\n"))
                + installDirectory_ + text(language_,
                    L"\n\n可在 Windows 设置 → 应用中卸载。",
                    L"\n\nYou can uninstall it from Windows Settings → Apps.");
            MessageBoxW(window_, result.c_str(), L"Yanami", MB_OK | MB_ICONINFORMATION);
            DestroyWindow(window_);
            return;
        }
        page_ = UiPage::Complete;
        renderPage();
    }

    void drawOwnerButton(const DRAWITEMSTRUCT& item) const {
        const bool primary = reinterpret_cast<INT_PTR>(
            GetPropW(item.hwndItem, L"YanamiPrimary")) != 0;
        const auto surface = static_cast<ButtonSurface>(
            reinterpret_cast<INT_PTR>(GetPropW(
                item.hwndItem, L"YanamiButtonSurface")));
        const bool folderIcon = GetPropW(
            item.hwndItem, L"YanamiFolderIcon") != nullptr;
        const bool hot = GetPropW(item.hwndItem, L"YanamiHot") != nullptr;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool focused = keyboardFocusVisible_
            && (item.itemState & ODS_FOCUS) != 0;
        COLORREF base = kBackground;
        if (surface == ButtonSurface::Panel) {
            base = kPanel;
        } else if (surface == ButtonSurface::Field) {
            base = kField;
        }
        if (highContrast_) {
            base = GetSysColor(COLOR_WINDOW);
        }
        if (!beginDrawing(item.hDC, item.rcItem)) {
            return;
        }
        painter_.clear(base);

        COLORREF fill = base;
        COLORREF outline = highContrast_
            ? GetSysColor(COLOR_WINDOWTEXT) : kOutline;
        if (highContrast_) {
            fill = GetSysColor(
                disabled ? COLOR_WINDOW : (pressed ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
        } else if (primary) {
            fill = disabled
                ? RGB(0x5d, 0x35, 0x43)
                : (pressed ? RGB(0xe9, 0x54, 0x77)
                           : (hot ? RGB(0xff, 0x78, 0x98) : kAccent));
            outline = fill;
        } else if (pressed) {
            fill = RGB(0x22, 0x25, 0x30);
            outline = RGB(0x48, 0x4d, 0x5c);
        } else if (hot) {
            fill = kField;
            outline = RGB(0x48, 0x4d, 0x5c);
        }
        if (folderIcon && !highContrast_) {
            fill = pressed ? RGB(0x30, 0x34, 0x41)
                : (hot ? RGB(0x26, 0x2a, 0x36) : base);
            outline = fill;
        }
        if (focused) {
            outline = highContrast_
                ? GetSysColor(COLOR_HIGHLIGHT) : RGB(0xff, 0xb3, 0xc5);
        }
        drawRoundedSurface(
            item.rcItem, fill, outline, folderIcon ? 9 : 22,
            focused ? 2 : 1);

        std::array<wchar_t, 256> label{};
        GetWindowTextW(item.hwndItem, label.data(), static_cast<int>(label.size()));
        const COLORREF textColor = highContrast_
            ? GetSysColor(pressed ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT)
            : (disabled ? RGB(0x72, 0x78, 0x86)
                        : (primary ? kBackground : kForeground));
        if (folderIcon) {
            const int centerX = (item.rcItem.left + item.rcItem.right) / 2;
            const int centerY = (item.rcItem.top + item.rcItem.bottom) / 2;
            painter_.folderIcon(RECT{
                centerX - scale(10), centerY - scale(10),
                centerX + scale(10), centerY + scale(10)}, textColor, fontPixels(1) * 1.5f);
        } else {
            painter_.text(label.data(), item.rcItem, fontPixels(14), true,
                          textColor, TextAlign::Center, true, false, false, isChinese());
        }
        finishDrawing();
    }

    void drawFooter() const {
        painter_.fillRect(logicalRect(64, 456, 552, 1),
                         highContrast_ ? GetSysColor(COLOR_WINDOWTEXT) : kOutline);
    }

    void drawPageSurface(HDC device) const {
        RECT client{};
        GetClientRect(window_, &client);
        if (!beginDrawing(device, client)) {
            return;
        }
        painter_.clear(highContrast_ ? GetSysColor(COLOR_WINDOW) : kBackground);
        const COLORREF surface = highContrast_
            ? GetSysColor(COLOR_WINDOW) : kPanel;
        const COLORREF field = highContrast_
            ? GetSysColor(COLOR_WINDOW) : kField;
        const COLORREF outline = highContrast_
            ? GetSysColor(COLOR_WINDOWTEXT) : kOutline;

        if (page_ == UiPage::Welcome) {
            drawFooter();
            finishDrawing();
            HICON icon = welcomeIcon_ != nullptr ? welcomeIcon_ : appIcon_;
            if (icon != nullptr) {
                DrawIconEx(
                    device, scale(292), scale(78), icon,
                    scale(96), scale(96), 0, nullptr, DI_NORMAL);
            }
            return;
        }

        if (page_ == UiPage::Options) {
            COLORREF pathOutline = outline;
            if (!pathValid_) {
                pathOutline = highContrast_
                    ? GetSysColor(COLOR_HIGHLIGHT) : kError;
            } else if (GetFocus() == pathEdit_
                       || (keyboardFocusVisible_ && GetFocus() == browseButton_)) {
                pathOutline = highContrast_
                    ? GetSysColor(COLOR_HIGHLIGHT) : kAccent;
            }
            drawRoundedSurface(
                logicalRect(64, 172, 552, 52),
                field, pathOutline, 12,
                (GetFocus() == pathEdit_
                 || (keyboardFocusVisible_ && GetFocus() == browseButton_)) ? 2 : 1);
            drawRoundedSurface(
                logicalRect(64, 262, 552, 132),
                surface, outline, 20);
            painter_.fillRect(logicalRect(84, 328, 512, 1), outline);
            drawFooter();
            finishDrawing();
            return;
        }

        if (page_ == UiPage::Installing) {
            const RECT track = logicalRect(200, 316, 280, 6);
            drawRoundedSurface(
                track,
                highContrast_ ? GetSysColor(COLOR_BTNFACE)
                              : RGB(0x27, 0x2b, 0x35),
                highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                              : RGB(0x27, 0x2b, 0x35),
                3);
            painter_.pushClip(track);
            const int segmentLeft = 200 + progressPhase_ - 80;
            drawRoundedSurface(
                logicalRect(segmentLeft, 316, 80, 6),
                highContrast_ ? GetSysColor(COLOR_HIGHLIGHT) : kAccent,
                highContrast_ ? GetSysColor(COLOR_HIGHLIGHT) : kAccent,
                3);
            painter_.popClip();
            finishDrawing();
            HICON icon = installingIcon_ != nullptr
                ? installingIcon_ : appIcon_;
            if (icon != nullptr) {
                DrawIconEx(device, scale(304), scale(96), icon,
                           scale(72), scale(72), 0, nullptr, DI_NORMAL);
            }
            return;
        }

        if (page_ == UiPage::Complete) {
            painter_.ellipse(logicalRect(308, 78, 64, 64),
                             highContrast_ ? GetSysColor(COLOR_HIGHLIGHT) : kAccent);
            const COLORREF check = highContrast_
                ? GetSysColor(COLOR_HIGHLIGHTTEXT) : kBackground;
            painter_.line(scale(325), scale(110), scale(337), scale(122),
                          check, fontPixels(4));
            painter_.line(scale(337), scale(122), scale(357), scale(98),
                          check, fontPixels(4));

            drawRoundedSurface(
                logicalRect(64, 242, 552, 88),
                surface, outline, 20);
            drawFooter();
        }
        finishDrawing();
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            return 0;
        case kRenderRecoveryMessage:
            // A failed EndDraw has already validated the update region. Retry
            // explicitly, but never leave a broken device in a busy paint loop.
            renderHadFailure_ = false;
            ++renderRecoveryAttempts_;
            RedrawWindow(window_, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            renderRecoveryQueued_ = false;
            if (!renderHadFailure_) {
                renderRecoveryAttempts_ = 0;
            } else if (renderRecoveryAttempts_ < 3) {
                requestRenderRecovery();
            } else {
                renderUnavailable_ = true;
                MessageBoxW(window_,
                    installing_
                        ? text(language_,
                               L"安装界面暂时无法绘制。文件安装仍在进行，完成后将显示结果。",
                               L"The installer display is unavailable. Installation is still running; its result will be shown when it finishes.")
                        : text(language_,
                               L"安装界面无法绘制。请重新打开安装程序；未开始新的安装。",
                               L"The installer display is unavailable. Please reopen setup; no new installation has been started."),
                    L"Yanami", MB_OK | MB_ICONERROR);
                if (!installing_) {
                    DestroyWindow(window_);
                }
            }
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            const SIZE outerSize = baselineWindowSize(dpi_);
            SetWindowPos(
                window_, nullptr, suggested->left, suggested->top,
                outerSize.cx, outerSize.cy,
                SWP_NOZORDER | SWP_NOACTIVATE);
            createFontsAndBrushes();
            renderPage();
            return 0;
        }
        case WM_SETTINGCHANGE:
            highContrast_ = isHighContrast();
            createFontsAndBrushes();
            renderPage();
            return 0;
        case WM_TIMER:
            if (wParam == kProgressTimer && page_ == UiPage::Installing) {
                progressPhase_ = (progressPhase_ + 5) % 360;
                const RECT progress = logicalRect(196, 312, 288, 14);
                InvalidateRect(window_, &progress, FALSE);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN: {
            const RECT field = logicalRect(64, 172, 552, 52);
            const POINT point{static_cast<short>(LOWORD(lParam)),
                              static_cast<short>(HIWORD(lParam))};
            if (page_ == UiPage::Options && pathEdit_ != nullptr
                && PtInRect(&field, point)) {
                SetFocus(pathEdit_);
                return 0;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC device = BeginPaint(window_, &paint);
            RECT client{};
            GetClientRect(window_, &client);
            FillRect(device, &client, backgroundBrush_);
            drawPageSurface(device);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC device = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(device, TRANSPARENT);
            const bool isError = message == WM_CTLCOLORSTATIC
                && GetPropW(control, L"YanamiError") != nullptr;
            SetTextColor(
                device,
                highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                              : (isError ? kError
                                 : (message == WM_CTLCOLORSTATIC
                                     && GetPropW(control, L"YanamiMuted") != nullptr
                                     ? kMuted : kForeground)));
            if (message == WM_CTLCOLORBTN) {
                return reinterpret_cast<LRESULT>(panelBrush_);
            }
            const auto surface = static_cast<ButtonSurface>(
                reinterpret_cast<INT_PTR>(GetPropW(
                    control, L"YanamiTextSurface")));
            if (surface == ButtonSurface::Panel) {
                return reinterpret_cast<LRESULT>(panelBrush_);
            }
            if (surface == ButtonSurface::Field) {
                return reinterpret_cast<LRESULT>(fieldBrush_);
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        case WM_CTLCOLOREDIT: {
            HDC device = reinterpret_cast<HDC>(wParam);
            SetBkColor(device, highContrast_ ? GetSysColor(COLOR_WINDOW) : kField);
            SetTextColor(device, highContrast_ ? GetSysColor(COLOR_WINDOWTEXT) : kForeground);
            return reinterpret_cast<LRESULT>(fieldBrush_);
        }
        case WM_DRAWITEM:
            drawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_COMMAND: {
            const int identifier = LOWORD(wParam);
            if (identifier == IdInstallPath && HIWORD(wParam) == EN_CHANGE) {
                updatePathValidationUi();
                return 0;
            }
            if (identifier == IDCANCEL) {
                if (!installing_) {
                    DestroyWindow(window_);
                } else {
                    MessageBeep(MB_ICONINFORMATION);
                }
                return 0;
            }
            switch (identifier) {
            case IdContinue:
                page_ = UiPage::Options;
                renderPage();
                return 0;
            case IdBack:
                page_ = UiPage::Welcome;
                renderPage();
                return 0;
            case IdBrowse:
                browseForDirectory();
                return 0;
            case IdInstall:
                beginInstall();
                return 0;
            case IdOpenFolder:
                ShellExecuteW(
                    window_, L"explore", installDirectory_.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case IdLater:
                DestroyWindow(window_);
                return 0;
            case IdFinish:
                ShellExecuteW(
                    window_, L"open", launchTarget_.c_str(), nullptr,
                    std::filesystem::path(launchTarget_).parent_path().c_str(),
                    SW_SHOWNORMAL);
                DestroyWindow(window_);
                return 0;
            default:
                break;
            }
            break;
        }
        case kInstallFinishedMessage:
            handleInstallFinished(std::unique_ptr<InstallOutcome>(
                reinterpret_cast<InstallOutcome*>(lParam)));
            return 0;
        case WM_CLOSE:
            if (installing_) {
                MessageBeep(MB_ICONINFORMATION);
                return 0;
            }
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kProgressTimer);
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    UiLanguage language_;
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND pathEdit_ = nullptr;
    HWND pathReadOnlyText_ = nullptr;
    HWND pathErrorText_ = nullptr;
    HWND browseButton_ = nullptr;
    HWND browseTooltip_ = nullptr;
    HWND installButton_ = nullptr;
    HWND startMenuCheck_ = nullptr;
    HWND desktopCheck_ = nullptr;
    HFONT normalFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT buttonFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HICON appIcon_ = nullptr;
    HICON welcomeIcon_ = nullptr;
    HICON installingIcon_ = nullptr;
    UINT dpi_ = 96;
    int progressPhase_ = 0;
    bool highContrast_ = false;
    bool keyboardFocusVisible_ = false;
    bool imeComposing_ = false;
    mutable yanami::installer::ui::InstallerPainter painter_;
    mutable bool renderHadFailure_ = false;
    mutable bool renderRecoveryQueued_ = false;
    bool renderUnavailable_ = false;
    int renderRecoveryAttempts_ = 0;
    bool installing_ = false;
    bool startMenu_ = true;
    bool desktop_ = false;
    bool pathValid_ = true;
    UiPage page_ = UiPage::Welcome;
    std::wstring selfPath_;
    std::wstring localAppData_;
    PathPolicyRoots roots_;
    std::optional<std::wstring> registeredInstall_;
    std::wstring installDirectory_;
    std::wstring pathValidationMessage_;
    std::wstring launchTarget_;
};

int runVerifyPayload(const std::wstring& selfPath) {
    const PayloadResult result = verifyPayloadFile(selfPath);
    std::ostringstream json;
    if (result.ok) {
        json << "{\"ok\":true,\"format\":" << result.metadata.format
             << ",\"payloadSize\":" << result.metadata.payloadSize
             << ",\"payloadSha256\":\""
             << hexLower(result.metadata.payloadSha256) << "\"}\n";
        writeHandleUtf8(STD_OUTPUT_HANDLE, json.str());
        return 0;
    }
    json << "{\"ok\":false,\"error\":\""
         << jsonEscape(wideToUtf8(result.error)) << "\"}\n";
    writeHandleUtf8(STD_OUTPUT_HANDLE, json.str());
    return 1;
}

int runSilentInstall(
    const CommandLineOptions& options,
    UiLanguage language,
    const std::wstring& selfPath,
    const std::wstring& localAppData) {
    InstallRequest request;
    request.selfPath = selfPath;
    request.installDirectory = options.installDirectory;
    request.registeredInstall = readRegisteredInstallLocation();
    request.roots = pathPolicyRoots();
    request.localAppData = localAppData;
    request.startMenu = options.startMenu;
    request.desktop = options.desktop;
    request.launchAfterInstall = options.launchAfterInstall;
    const InstallOutcome outcome = performInstall(request);
    if (!outcome.success) {
        std::wstring message = localizedCoreError(language, outcome.error);
        if (!outcome.preservedLog.empty()) {
            message.append(L"\nLog: ");
            message.append(outcome.preservedLog);
        }
        writeHandleUtf8(STD_ERROR_HANDLE, wideToUtf8(message) + "\n");
        return 1;
    }
    writeHandleUtf8(
        STD_OUTPUT_HANDLE,
        "Yanami installed at " + wideToUtf8(outcome.installDirectory) + "\n");
    return 0;
}

std::vector<std::wstring> commandLineArguments() {
    int count = 0;
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (values == nullptr) {
        return {};
    }
    std::vector<std::wstring> result;
    for (int index = 1; index < count; ++index) {
        result.emplace_back(values[index]);
    }
    LocalFree(values);
    return result;
}

void enablePerMonitorDpi() {
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        const auto setContext = reinterpret_cast<SetContext>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext != nullptr) {
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    enablePerMonitorDpi();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    const HRESULT com = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitializeCom = SUCCEEDED(com);

    const UiLanguage language = detectLanguage();
    const std::wstring selfPath = modulePath();
    const CommandLineResult parsed = parseCommandLine(commandLineArguments());
    if (!parsed.ok) {
        writeHandleUtf8(
            STD_ERROR_HANDLE,
            wideToUtf8(parsed.error) + "\n");
        if (uninitializeCom) {
            CoUninitialize();
        }
        return 2;
    }
    if (parsed.options.mode == LaunchMode::VerifyPayload) {
        const int result = runVerifyPayload(selfPath);
        if (uninitializeCom) {
            CoUninitialize();
        }
        return result;
    }

    const std::wstring localAppData =
        knownFolder(FOLDERID_LocalAppData).value_or(L"");
    if (parsed.options.mode == LaunchMode::SilentInstall) {
        const int result = runSilentInstall(
            parsed.options, language, selfPath, localAppData);
        if (uninitializeCom) {
            CoUninitialize();
        }
        return result;
    }

    InstallerWindow window(
        language, selfPath, localAppData, pathPolicyRoots(),
        readRegisteredInstallLocation());
    if (!window.create(instance)) {
        if (uninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!window.preprocessKeyboardMessage(message)
            && !IsDialogMessageW(window.handle(), &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (uninitializeCom) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
