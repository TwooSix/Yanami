#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "InstallerShortcuts.hpp"

#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace yanami::installer {
namespace {

constexpr LONGLONG kMaximumShortcutBytes = 1024 * 1024;
constexpr PROPERTYKEY kAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39,
     {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            close();
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }
    void close() {
        if (*this) { CloseHandle(value_); }
        value_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_;
};

template <typename T>
class ComPointer final {
public:
    ~ComPointer() { if (value_) { value_->Release(); } }
    ComPointer() = default;
    ComPointer(const ComPointer&) = delete;
    ComPointer& operator=(const ComPointer&) = delete;
    T* operator->() const { return value_; }
    T** put() { return &value_; }
private:
    T* value_ = nullptr;
};

class ComScope final {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() { if (SUCCEEDED(result_)) { CoUninitialize(); } }
    bool ok() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
private:
    HRESULT result_;
};

bool equalPath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE)
        == CSTR_EQUAL;
}

std::optional<std::filesystem::path> absolutePath(
    const std::filesystem::path& value) {
    if (value.empty() || !value.is_absolute()
        || value.native().find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }
    const DWORD length = GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
    if (length == 0) { return std::nullopt; }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
    const DWORD written = GetFullPathNameW(
        value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (written == 0 || written >= buffer.size()) { return std::nullopt; }
    auto normalized = std::filesystem::path(buffer.data()).lexically_normal();
    // A directory trailing separator does not change its identity.
    while (normalized.has_relative_path() && normalized.filename().empty()) {
        normalized = normalized.parent_path();
    }
    return normalized;
}

bool readBytes(HANDLE file, std::vector<std::byte>& bytes) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > kMaximumShortcutBytes) {
        return false;
    }
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) { return false; }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    return ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                    &read, nullptr)
        && read == bytes.size();
}

bool sameFile(
    const BY_HANDLE_FILE_INFORMATION& left,
    const BY_HANDLE_FILE_INFORMATION& right) {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber
        && left.nFileIndexHigh == right.nFileIndexHigh
        && left.nFileIndexLow == right.nFileIndexLow;
}

struct ExistingShortcut {
    Handle guard;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::vector<std::byte> bytes;
    bool regularFile = false;
    bool owned = false;
    bool matches = false;
};

ExistingShortcut inspectShortcut(
    const std::filesystem::path& path,
    const ShortcutRequest& request) {
    ExistingShortcut result;
    // Keep the bytes/path stable while the Shell decodes the link. Opening a
    // reparse point itself prevents a foreign symlink from establishing owner
    // status through its destination.
    result.guard = Handle(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!result.guard
        || !GetFileInformationByHandle(result.guard.get(), &result.identity)
        || (result.identity.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        return result;
    }
    result.regularFile = true;

    ComPointer<IShellLinkW> link;
    ComPointer<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(link.put())))
        || FAILED(link->QueryInterface(
            IID_IPersistFile, reinterpret_cast<void**>(persist.put())))
        || FAILED(persist->Load(path.c_str(), STGM_READ))) {
        return result;
    }
    std::array<wchar_t, 32768> buffer{};
    if (FAILED(link->GetPath(buffer.data(), static_cast<int>(buffer.size()),
                            nullptr, SLGP_RAWPATH))) {
        return result;
    }
    const auto target = absolutePath(std::filesystem::path(buffer.data()));
    if (!target || !equalPath(target->native(), request.target.native())
        || !readBytes(result.guard.get(), result.bytes)) {
        return result;
    }
    result.owned = true;

    if (FAILED(link->GetArguments(buffer.data(), static_cast<int>(buffer.size())))
        || buffer[0] != L'\0') {
        return result;
    }
    if (FAILED(link->GetWorkingDirectory(
            buffer.data(), static_cast<int>(buffer.size())))) {
        return result;
    }
    const auto directory = absolutePath(std::filesystem::path(buffer.data()));
    if (!directory
        || !equalPath(directory->native(), request.workingDirectory.native())) {
        return result;
    }
    if (FAILED(link->GetDescription(buffer.data(), static_cast<int>(buffer.size())))
        || std::wstring(buffer.data()) != request.description) {
        return result;
    }
    int iconIndex = -1;
    if (FAILED(link->GetIconLocation(buffer.data(), static_cast<int>(buffer.size()),
                                    &iconIndex)) || iconIndex != 0) {
        return result;
    }
    const auto icon = absolutePath(std::filesystem::path(buffer.data()));
    if (!icon || !equalPath(icon->native(), request.target.native())) { return result; }
    ComPointer<IPropertyStore> properties;
    if (FAILED(link->QueryInterface(IID_IPropertyStore,
                                   reinterpret_cast<void**>(properties.put())))) {
        return result;
    }
    PROPVARIANT value{};
    const HRESULT propertyResult = properties->GetValue(kAppUserModelId, &value);
    result.matches = SUCCEEDED(propertyResult) && value.vt == VT_LPWSTR
        && value.pwszVal != nullptr && request.appUserModelId == value.pwszVal;
    PropVariantClear(&value);
    return result;
}

class StagedShortcut final {
public:
    ~StagedShortcut() {
        if (!path_.empty()) { DeleteFileW(path_.c_str()); }
        if (!directory_.empty()) { RemoveDirectoryW(directory_.c_str()); }
    }
    const std::filesystem::path& path() const { return path_; }
    bool save(const ShortcutRequest& request) {
        GUID identifier{};
        std::array<wchar_t, 40> text{};
        if (FAILED(CoCreateGuid(&identifier))
            || StringFromGUID2(identifier, text.data(), static_cast<int>(text.size())) == 0) {
            return false;
        }
        const auto candidate = request.shortcutPath.parent_path()
            / (std::wstring(L".yanami-shortcut-") + text.data());
        if (!CreateDirectoryW(candidate.c_str(), nullptr)) { return false; }
        directory_ = candidate;
        path_ = directory_ / L"shortcut.lnk";
        ComPointer<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(link.put())))
            || FAILED(link->SetPath(request.target.c_str()))
            || FAILED(link->SetArguments(L""))
            || FAILED(link->SetWorkingDirectory(request.workingDirectory.c_str()))
            || FAILED(link->SetIconLocation(request.target.c_str(), 0))
            || FAILED(link->SetDescription(request.description.c_str()))) {
            return false;
        }
        ComPointer<IPropertyStore> properties;
        if (FAILED(link->QueryInterface(
                IID_IPropertyStore, reinterpret_cast<void**>(properties.put())))) {
            return false;
        }
        PROPVARIANT value{};
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<wchar_t*>(request.appUserModelId.c_str());
        if (FAILED(properties->SetValue(kAppUserModelId, value))
            || FAILED(properties->Commit())) {
            return false;
        }
        ComPointer<IPersistFile> persist;
        return SUCCEEDED(link->QueryInterface(
                IID_IPersistFile, reinterpret_cast<void**>(persist.put())))
            && SUCCEEDED(persist->Save(path_.c_str(), TRUE));
    }
private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

bool atomicallyReplaceExisting(
    const std::filesystem::path& staged,
    const std::filesystem::path& destination,
    ExistingShortcut& existing,
    DWORD& failure) {
    if (!existing.regularFile || !existing.guard
        || (existing.identity.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) {
        failure = ERROR_ACCESS_DENIED;
        return false;
    }
    Handle source(CreateFileW(staged.c_str(), GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!source || !FlushFileBuffers(source.get())) {
        failure = GetLastError();
        return false;
    }
    source.close();
    existing.guard.close();
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT
                          | FILE_ATTRIBUTE_READONLY))) {
        failure = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_ACCESS_DENIED;
        return false;
    }
    // Selected canonical shortcut replacement is explicitly authorized,
    // regardless of its previous owner. Stage on the same volume and rename
    // atomically; never delete/truncate the destination first. A locked or
    // unwritable destination fails without destroying its existing bytes.
    if (MoveFileExW(staged.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    failure = GetLastError();
    return false;
}

bool removeOwned(
    const std::filesystem::path& path, ExistingShortcut& existing) {
    if (!existing.owned) { return true; }
    existing.guard.close();
    Handle file(CreateFileW(path.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION identity{};
    std::vector<std::byte> bytes;
    if (!file || !GetFileInformationByHandle(file.get(), &identity)
        || (identity.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || !sameFile(identity, existing.identity)
        || !readBytes(file.get(), bytes) || bytes != existing.bytes) {
        return false;
    }
    // Delete the exact reopened, byte-verified file through its own handle;
    // never a path that could have been replaced after the ownership check.
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file.get(), FileDispositionInfo,
        &disposition, sizeof(disposition)) != FALSE;
}

ShortcutResult selectedResult(
    const std::filesystem::path& path, bool changed) {
    ShortcutResult result;
    result.ok = true;
    result.changed = changed;
    result.shortcutPath = path;
    return result;
}

ShortcutResult updateSelected(
    const std::filesystem::path& path,
    const ShortcutRequest& request,
    ExistingShortcut& existing) {
    if (existing.matches) { return selectedResult(path, false); }
    StagedShortcut stage;
    DWORD failure = ERROR_SUCCESS;
    if (!stage.save(request) || !atomicallyReplaceExisting(stage.path(), path, existing, failure)) {
        ShortcutResult result;
        result.warning = L"The selected shortcut could not be updated. The existing shortcut was kept. Windows error: "
            + std::to_wstring(failure) + L".";
        return result;
    }
    return selectedResult(path, true);
}

ShortcutResult synchronize(const ShortcutRequest& input) {
    ShortcutRequest request = input;
    const auto canonical = absolutePath(input.shortcutPath);
    const auto target = absolutePath(input.target);
    const auto working = absolutePath(input.workingDirectory);
    ShortcutResult result;
    if (!canonical || !target || !working || canonical->stem().empty()
        || !equalPath(canonical->extension().native(), L".lnk")
        || input.appUserModelId.empty()
        || input.appUserModelId.find(L'\0') != std::wstring::npos
        || input.description.find(L'\0') != std::wstring::npos) {
        result.warning = L"The shortcut paths or properties are invalid.";
        return result;
    }
    request.shortcutPath = *canonical;
    request.target = *target;
    request.workingDirectory = *working;

    std::error_code error;
    if (request.selected) {
        std::filesystem::create_directories(canonical->parent_path(), error);
        if (error) {
            result.warning = L"The selected shortcut directory is not writable.";
            return result;
        }
    } else if (!std::filesystem::exists(canonical->parent_path(), error) && !error) {
        result.ok = true;
        return result;
    }
    const DWORD attributes = GetFileAttributesW(canonical->c_str());
    const DWORD inspectionError = GetLastError();
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES
        && (inspectionError == ERROR_FILE_NOT_FOUND || inspectionError == ERROR_PATH_NOT_FOUND);
    if (attributes == INVALID_FILE_ATTRIBUTES && !missing) {
        result.warning = L"The shortcut could not be inspected. Existing files were kept.";
        return result;
    }
    if (missing && !request.selected) {
        result.ok = true;
        return result;
    }
    if (!missing && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        result.ok = !request.selected;
        if (request.selected) {
            result.warning = L"The shortcut path is a directory or reparse point. It was kept unchanged.";
        }
        return result;
    }

    ComScope com;
    if (!com.ok()) {
        result.warning = L"Windows could not initialize shortcut handling.";
        return result;
    }
    if (!missing) {
        auto existing = inspectShortcut(*canonical, request);
        if (!request.selected) {
            result.ok = !existing.owned || removeOwned(*canonical, existing);
            result.changed = existing.owned && result.ok;
            if (!result.ok) {
                result.warning = L"The unselected shortcut could not be removed. It was kept unchanged.";
            }
            return result;
        }
        if (existing.regularFile) { return updateSelected(*canonical, request, existing); }
        result.warning = L"The selected shortcut could not be inspected. It was kept unchanged.";
        return result;
    }

    StagedShortcut stage;
    if (!stage.save(request)) {
        result.warning = L"The selected shortcut could not be created. Existing files were kept.";
        return result;
    }
    // Creation is no-replace: if the destination appeared after inspection,
    // leave it intact and allow a subsequent request to inspect it normally.
    if (MoveFileExW(stage.path().c_str(), canonical->c_str(), MOVEFILE_WRITE_THROUGH)) {
        return selectedResult(*canonical, true);
    }
    result.warning = L"The selected shortcut could not be saved. Existing files were kept.";
    return result;
}

} // namespace

ShortcutResult synchronizeInstallerShortcut(const ShortcutRequest& request) {
    try {
        auto result = synchronize(request);
        if (!result.ok) {
            result.shortcutPath = absolutePath(request.shortcutPath).value_or(request.shortcutPath);
        }
        return result;
    } catch (const std::exception&) {
        ShortcutResult result;
        result.shortcutPath = request.shortcutPath;
        result.warning = L"The shortcut option could not be completed. Existing files were kept.";
        return result;
    }
}

} // namespace yanami::installer
