#include "InstallerShortcuts.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

using namespace yanami::installer;
int assertions = 0;

void expect(bool condition, const char* message) {
    ++assertions;
    // CTest kills a timed-out process before buffered stdout is flushed.
    // Keep the last completed operation visible for slow Shell/COM calls.
    std::cout << "CHECK " << assertions << ": " << message << '\n';
    if (!condition) { throw std::runtime_error(message); }
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) { return {}; }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring windowsPathSpelling(const std::filesystem::path& path) {
    const auto lexical = path.lexically_normal().native();
    std::array<wchar_t, 32768> longName{};
    const DWORD length = GetLongPathNameW(
        lexical.c_str(), longName.data(), static_cast<DWORD>(longName.size()));
    return length != 0 && length < longName.size()
        ? std::wstring(longName.data(), length) : lexical;
}

bool sameWindowsPath(const std::filesystem::path& actual,
                     const std::filesystem::path& expected) {
    // The Shell is allowed to expand an existing 8.3 name or normalize case.
    // Do not resolve junctions/symlinks or collapse different Unicode names.
    const auto actualName = windowsPathSpelling(actual);
    const auto expectedName = windowsPathSpelling(expected);
    const bool same = CompareStringOrdinal(actualName.c_str(), -1,
        expectedName.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (!same) {
        std::cerr << "Windows path mismatch (ACP=" << GetACP() << ")\nExpected raw: "
                  << utf8(expected.native()) << "\nActual raw: " << utf8(actual.native())
                  << "\nExpected long: " << utf8(expectedName)
                  << "\nActual long: " << utf8(actualName) << '\n';
    }
    return same;
}

template <typename T>
class ComPointer final {
public:
    ~ComPointer() { if (value_) { value_->Release(); } }
    T** put() { return &value_; }
    T* operator->() const { return value_; }
private:
    T* value_ = nullptr;
};

constexpr PROPERTYKEY kActualAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39,
     {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::array<wchar_t, 32768> buffer{};
        const auto length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        expect(length > 0 && length < buffer.size(), "temporary parent should resolve");
        parent_ = std::filesystem::path(buffer.data()).lexically_normal();
        while (parent_.has_relative_path() && parent_.filename().empty()) {
            parent_ = parent_.parent_path();
        }
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = parent_
                / (std::wstring(L"yanami-installer-shortcuts-tests-")
                   + std::to_wstring(GetCurrentProcessId()) + L"-"
                   + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("temporary test directory could not be created");
    }
    ~TemporaryDirectory() {
        if (path_.empty() || path_.parent_path() != parent_
            || !path_.filename().native().starts_with(L"yanami-installer-shortcuts-tests-")) {
            return;
        }
        std::error_code error;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path_, error)) {
            SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path parent_;
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    expect(output.good(), "isolated fixture file should be written");
}

std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.good(), "isolated fixture file should open");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

ShortcutRequest requestFor(const std::filesystem::path& directory) {
    ShortcutRequest request;
    request.shortcutPath = directory / L"links" / L"Yanami.lnk";
    request.target = directory / L"installed" / L"current" / L"Yanami.exe";
    request.workingDirectory = request.target.parent_path();
    writeFile(request.target, "isolated shortcut target; never executed");
    return request;
}

void fixtureShortcut(
    const std::filesystem::path& path, const ShortcutRequest& request,
    const std::wstring& arguments = {}) {
    std::filesystem::create_directories(path.parent_path());
    ComPointer<IShellLinkW> link;
    expect(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, reinterpret_cast<void**>(link.put()))), "fixture ShellLinkW should create");
    expect(SUCCEEDED(link->SetPath(request.target.c_str())), "fixture target should set");
    expect(SUCCEEDED(link->SetArguments(arguments.c_str())), "fixture arguments should set");
    expect(SUCCEEDED(link->SetWorkingDirectory(request.workingDirectory.c_str())),
        "fixture working directory should set");
    expect(SUCCEEDED(link->SetIconLocation(request.target.c_str(), 0)), "fixture icon should set");
    expect(SUCCEEDED(link->SetDescription(request.description.c_str())), "fixture description should set");
    ComPointer<IPropertyStore> properties;
    expect(SUCCEEDED(link->QueryInterface(IID_IPropertyStore,
        reinterpret_cast<void**>(properties.put()))), "fixture properties should open");
    PROPVARIANT value{};
    value.vt = VT_LPWSTR;
    value.pwszVal = const_cast<wchar_t*>(request.appUserModelId.c_str());
    expect(SUCCEEDED(properties->SetValue(kActualAppUserModelId, value)), "fixture AUMID should set");
    expect(SUCCEEDED(properties->Commit()), "fixture properties should commit");
    ComPointer<IPersistFile> persist;
    expect(SUCCEEDED(link->QueryInterface(IID_IPersistFile,
        reinterpret_cast<void**>(persist.put()))), "fixture persistence should open");
    expect(SUCCEEDED(persist->Save(path.c_str(), TRUE)), "fixture shortcut should save");
}

void expectShortcut(const std::filesystem::path& path, const ShortcutRequest& expected) {
    ComPointer<IShellLinkW> link;
    ComPointer<IPersistFile> persist;
    expect(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, reinterpret_cast<void**>(link.put()))), "reader ShellLinkW should create");
    expect(SUCCEEDED(link->QueryInterface(IID_IPersistFile,
        reinterpret_cast<void**>(persist.put()))), "reader persistence should open");
    expect(SUCCEEDED(persist->Load(path.c_str(), STGM_READ)), "written shortcut should load");
    std::array<wchar_t, 32768> buffer{};
    expect(SUCCEEDED(link->GetPath(buffer.data(), static_cast<int>(buffer.size()), nullptr, SLGP_RAWPATH)),
        "wide target should read");
    expect(sameWindowsPath(buffer.data(), expected.target), "exact Unicode target should match");
    expect(SUCCEEDED(link->GetArguments(buffer.data(), static_cast<int>(buffer.size()))),
        "arguments should read");
    expect(buffer[0] == L'\0', "installed shortcut should not retain stale launch arguments");
    expect(SUCCEEDED(link->GetWorkingDirectory(buffer.data(), static_cast<int>(buffer.size()))),
        "wide working directory should read");
    expect(sameWindowsPath(buffer.data(), expected.workingDirectory),
        "exact Unicode working directory should match");
    expect(SUCCEEDED(link->GetDescription(buffer.data(), static_cast<int>(buffer.size()))),
        "description should read");
    expect(std::wstring(buffer.data()) == expected.description, "description should match");
    int icon = -1;
    expect(SUCCEEDED(link->GetIconLocation(buffer.data(), static_cast<int>(buffer.size()), &icon)),
        "icon should read");
    expect(icon == 0 && sameWindowsPath(buffer.data(), expected.target),
        "icon should target this installation");
    ComPointer<IPropertyStore> properties;
    expect(SUCCEEDED(link->QueryInterface(IID_IPropertyStore,
        reinterpret_cast<void**>(properties.put()))), "reader properties should open");
    PROPVARIANT value{};
    expect(SUCCEEDED(properties->GetValue(kActualAppUserModelId, &value)), "AUMID should read");
    const bool matching = value.vt == VT_LPWSTR && value.pwszVal
        && expected.appUserModelId == value.pwszVal;
    PropVariantClear(&value);
    expect(matching, "exact AppUserModelID should match");
}

void expectNoStaging(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        expect(!entry.path().filename().native().starts_with(L".yanami-shortcut-"),
            "temporary shortcut staging should always be removed");
    }
}

void testNewAndRepeat(const std::filesystem::path& root) {
    auto request = requestFor(root / L"Unicode 安装 \U0001F9EA [test]");
    request.shortcutPath = request.shortcutPath.parent_path() / L"Yanami [test].lnk";
    const auto result = synchronizeInstallerShortcut(request);
    expect(result.ok && result.changed,
        "a new selected shortcut should be created without a warning");
    expect(result.warning.empty() && sameWindowsPath(result.shortcutPath, request.shortcutPath),
        "the canonical selected path should be returned");
    expectShortcut(result.shortcutPath, request);
    const auto original = readFile(request.shortcutPath);
    const auto repeated = synchronizeInstallerShortcut(request);
    expect(repeated.ok && !repeated.changed,
        "repeated unchanged selection should be idempotent");
    expect(readFile(request.shortcutPath) == original, "repeat should not rewrite matching bytes");
    fixtureShortcut(request.shortcutPath, request, L"--stale-argument from-older-shortcut");
    const auto argumentsRepaired = synchronizeInstallerShortcut(request);
    expect(argumentsRepaired.ok && argumentsRepaired.changed,
        "a correct target with stale launch arguments must not be skipped as idempotent");
    expectShortcut(request.shortcutPath, request);

    request.workingDirectory = root / L"new working directory 安装 \U0001F9EA";
    std::filesystem::create_directories(request.workingDirectory);
    request.appUserModelId = L"io.github.TwooSix.Yanami.Yanami.Changed";
    request.description = L"Yanami updated description";
    const auto updated = synchronizeInstallerShortcut(request);
    if (!updated.ok) { std::wcerr << updated.warning << L'\n'; }
    expect(updated.ok && updated.changed && sameWindowsPath(updated.shortcutPath, request.shortcutPath),
        "an owned link should update atomically at the same path");
    expectShortcut(updated.shortcutPath, request);
    request.selected = false;
    const auto removed = synchronizeInstallerShortcut(request);
    expect(removed.ok && removed.changed && removed.shortcutPath.empty(),
        "deselection should remove an owned canonical link");
    expect(!std::filesystem::exists(request.shortcutPath), "owned link should be absent");
    expect(synchronizeInstallerShortcut(request).ok, "repeated deselection should succeed");
}

void testShortPathAliasOwnership(const std::filesystem::path& root) {
    const auto longRoot = root / L"long directory with spaces for 8.3 alias";
    auto request = requestFor(longRoot / L"Unicode 安装 \U0001F9EA [test]");
    const auto longTarget = request.target;
    std::array<wchar_t, 32768> shortName{};
    const DWORD length = GetShortPathNameW(
        longRoot.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    expect(length != 0 && length < shortName.size(), "isolated short path query should succeed");
    const std::filesystem::path shortRoot(shortName.data());
    if (CompareStringOrdinal(shortRoot.c_str(), -1, longRoot.c_str(), -1, TRUE) == CSTR_EQUAL) {
        std::cout << "SKIP: 8.3 alias ownership fixture; volume does not expose short names.\n";
        return;
    }
    // Keep the CJK/supplementary-plane suffix intact while aliasing the parent,
    // as GetTempPathW can do for the runneradmin account on hosted Windows.
    request.target = shortRoot / request.target.lexically_relative(longRoot);
    request.workingDirectory = request.target.parent_path();
    request.shortcutPath = shortRoot / request.shortcutPath.lexically_relative(longRoot);
    expect(request.target.native() != longTarget.native(), "alias fixture must use a genuinely different spelling");
    const auto created = synchronizeInstallerShortcut(request);
    expect(created.ok && created.changed, "short-alias selection should create its canonical link");
    expectShortcut(created.shortcutPath, request);
    const auto originalBytes = readFile(request.shortcutPath);
    const auto repeated = synchronizeInstallerShortcut(request);
    expect(repeated.ok && !repeated.changed,
        "short-alias selection must recognize its expanded target and avoid rewriting matching bytes");
    expect(readFile(request.shortcutPath) == originalBytes, "short-alias repeat must preserve exact shortcut bytes");
    request.selected = false;
    const auto removed = synchronizeInstallerShortcut(request);
    expect(removed.ok && removed.changed && removed.shortcutPath.empty(),
        "short-alias deselection must recognize and remove its exact expanded target");
    expect(!std::filesystem::exists(request.shortcutPath), "short-alias owned link must be absent after deselection");
    std::cout << "PASS: short-alias Unicode target remains idempotent and removable.\n";
}

void testUnselectedForeignAndInvalid(const std::filesystem::path& root) {
    auto request = requestFor(root);
    request.selected = false;
    const auto missing = synchronizeInstallerShortcut(request);
    expect(missing.ok && !missing.changed, "unselected missing shortcut should be a no-op");
    expect(!std::filesystem::exists(request.shortcutPath.parent_path()),
        "unselected missing shortcut should not create its parent directory");

    auto foreign = requestFor(root / L"older installation");
    fixtureShortcut(request.shortcutPath, foreign);
    const auto foreignBytes = readFile(request.shortcutPath);
    const auto result = synchronizeInstallerShortcut(request);
    expect(result.ok && !result.changed && result.warning.empty(),
        "unselected foreign link should be kept without blocking installation");
    expect(readFile(request.shortcutPath) == foreignBytes, "unselected foreign bytes should remain exact");
    writeFile(request.shortcutPath, "this is not a shell link");
    const auto invalidBytes = readFile(request.shortcutPath);
    const auto invalid = synchronizeInstallerShortcut(request);
    expect(invalid.ok && !invalid.changed, "unselected invalid link should be ignored");
    expect(readFile(request.shortcutPath) == invalidBytes, "invalid foreign bytes should remain exact");
}

void testSelectedOverwrite(const std::filesystem::path& root) {
    auto request = requestFor(root);
    auto older = requestFor(root / L"old installation");
    older.appUserModelId = L"Older.App.Identity";
    fixtureShortcut(request.shortcutPath, older);
    const auto original = readFile(request.shortcutPath);
    const auto second = request.shortcutPath.parent_path() / L"Yanami (2).lnk";
    writeFile(second, "reserved invalid shortcut");
    const auto invalid = readFile(second);
    const auto selected = synchronizeInstallerShortcut(request);
    expect(selected.ok && selected.changed && selected.warning.empty(),
        "selection should overwrite an older installation's canonical shortcut");
    expect(sameWindowsPath(selected.shortcutPath, request.shortcutPath),
        "selection should retain the requested canonical name");
    expectShortcut(request.shortcutPath, request);
    expect(readFile(request.shortcutPath) != original,
        "selection should replace the old target and AppUserModelID");
    expect(readFile(second) == invalid, "selection should not inspect or overwrite numbered names");
    const auto repeated = synchronizeInstallerShortcut(request);
    expect(repeated.ok && !repeated.changed && sameWindowsPath(repeated.shortcutPath, request.shortcutPath),
        "a repeated installation should reuse the canonical shortcut");
    writeFile(request.shortcutPath, "invalid shell link bytes");
    const auto repaired = synchronizeInstallerShortcut(request);
    expect(repaired.ok && repaired.changed && repaired.warning.empty(),
        "selection should atomically replace an invalid canonical link");
    expectShortcut(request.shortcutPath, request);
    fixtureShortcut(second, request);
    const auto numberedOwned = readFile(second);
    request.selected = false;
    const auto deselected = synchronizeInstallerShortcut(request);
    expect(deselected.ok && deselected.changed && !std::filesystem::exists(request.shortcutPath),
        "deselection should remove only the owned canonical link");
    expect(readFile(second) == numberedOwned, "deselection must not scan or remove numbered names");
}

void testExactTargetOwnership(const std::filesystem::path& root) {
    auto request = requestFor(root);
    auto other = request;
    other.target = request.target.parent_path() / L"Other.exe";
    writeFile(other.target, "another executable in the same installation directory");
    fixtureShortcut(request.shortcutPath, other);
    const auto original = readFile(request.shortcutPath);
    request.selected = false;
    expect(synchronizeInstallerShortcut(request).ok, "deselection should accept a foreign same-root link");
    expect(readFile(request.shortcutPath) == original, "parent-directory membership must not establish ownership");
    request.selected = true;
    const auto result = synchronizeInstallerShortcut(request);
    expect(result.ok && sameWindowsPath(result.shortcutPath, request.shortcutPath),
        "selected same-root foreign target should be overwritten at the canonical name");
    expect(readFile(request.shortcutPath) != original, "selected same-root foreign target should be updated");
    expectShortcut(result.shortcutPath, request);
}

void testFailureKeepsOldLink(const std::filesystem::path& root) {
    auto request = requestFor(root);
    fixtureShortcut(request.shortcutPath, request);
    const auto original = readFile(request.shortcutPath);
    expect(SetFileAttributesW(request.shortcutPath.c_str(), FILE_ATTRIBUTE_READONLY),
        "isolated owned link should become read-only");
    request.description = L"new description must not truncate a read-only link";
    const auto update = synchronizeInstallerShortcut(request);
    expect(!update.ok && !update.warning.empty() && sameWindowsPath(update.shortcutPath, request.shortcutPath),
        "read-only update failure should return a nonfatal shortcut warning");
    expect(readFile(request.shortcutPath) == original, "failed replacement must leave original bytes intact");
    request.selected = false;
    const auto remove = synchronizeInstallerShortcut(request);
    expect(!remove.ok && !remove.warning.empty(), "unremovable owned link should report a warning");
    expect(sameWindowsPath(remove.shortcutPath, request.shortcutPath),
        "failed deselection should identify its canonical path");
    expect(readFile(request.shortcutPath) == original, "failed removal must leave original bytes intact");
    expect(SetFileAttributesW(request.shortcutPath.c_str(), FILE_ATTRIBUTE_NORMAL),
        "isolated read-only attribute should be restored");

    auto blocked = requestFor(root / L"blocked-parent");
    writeFile(blocked.shortcutPath.parent_path(), "this is a file, not a writable directory");
    const auto blockingBytes = readFile(blocked.shortcutPath.parent_path());
    const auto failure = synchronizeInstallerShortcut(blocked);
    expect(!failure.ok && !failure.warning.empty(), "unwritable parent should produce a warning, not an exception");
    expect(readFile(blocked.shortcutPath.parent_path()) == blockingBytes,
        "failure must not replace a blocking foreign file with a directory");
    blocked.shortcutPath = L"relative-shortcut.lnk";
    expect(!synchronizeInstallerShortcut(blocked).ok, "relative shortcut paths should be rejected");
}

void testRejectDirectoryAndReparse(const std::filesystem::path& root) {
    auto request = requestFor(root);
    std::filesystem::create_directories(request.shortcutPath);
    writeFile(request.shortcutPath / L"keep.txt", "directory contents must remain unchanged");
    const auto rejected = synchronizeInstallerShortcut(request);
    expect(!rejected.ok && !rejected.warning.empty(), "a directory at the link path must not be overwritten");
    expect(std::filesystem::is_regular_file(request.shortcutPath / L"keep.txt"),
        "the rejected directory's contents should remain intact");
    request.selected = false;
    expect(synchronizeInstallerShortcut(request).ok,
        "unselected directory collision should be ignored");

    auto reparse = requestFor(root / L"reparse");
    std::filesystem::create_directories(reparse.shortcutPath.parent_path());
    const auto foreign = root / L"foreign-link.lnk";
    fixtureShortcut(foreign, request);
    const auto foreignBytes = readFile(foreign);
    if (CreateSymbolicLinkW(reparse.shortcutPath.c_str(), foreign.c_str(),
                           SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        expect(!synchronizeInstallerShortcut(reparse).ok,
            "a reparse point at the canonical link path must not be overwritten");
        reparse.selected = false;
        expect(synchronizeInstallerShortcut(reparse).ok,
            "unselected reparse point should be ignored");
        expect((GetFileAttributesW(reparse.shortcutPath.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
            "the reparse point should remain intact");
        expect(readFile(foreign) == foreignBytes, "a foreign reparse destination must remain unchanged");
    } else {
        // Directory junctions exercise real reparse-point rejection without
        // requiring Developer Mode or SeCreateSymbolicLinkPrivilege.
        const auto foreignDirectory = root / L"foreign reparse directory";
        writeFile(foreignDirectory / L"keep.txt", "reparse destination must remain unchanged");
        const auto untouched = readFile(foreignDirectory / L"keep.txt");
        expect(CreateDirectoryW(reparse.shortcutPath.c_str(), nullptr),
            "isolated junction directory should create");
        const HANDLE junction = CreateFileW(reparse.shortcutPath.c_str(), GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        expect(junction != INVALID_HANDLE_VALUE, "isolated junction handle should open");
        struct MountPointBuffer {
            DWORD tag;
            WORD dataLength;
            WORD reserved;
            WORD substituteOffset;
            WORD substituteLength;
            WORD printOffset;
            WORD printLength;
            wchar_t path[1];
        };
        const auto substitute = std::wstring(L"\\??\\") + foreignDirectory.native();
        const auto printed = foreignDirectory.native();
        const auto characters = substitute.size() + 1 + printed.size() + 1;
        const auto bufferSize = offsetof(MountPointBuffer, path) + characters * sizeof(wchar_t);
        std::vector<std::byte> storage(bufferSize);
        auto* mount = reinterpret_cast<MountPointBuffer*>(storage.data());
        mount->tag = IO_REPARSE_TAG_MOUNT_POINT;
        mount->dataLength = static_cast<WORD>(bufferSize - 8);
        mount->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        mount->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
        mount->printLength = static_cast<WORD>(printed.size() * sizeof(wchar_t));
        std::memcpy(mount->path, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
        std::memcpy(mount->path + substitute.size() + 1, printed.c_str(),
                    (printed.size() + 1) * sizeof(wchar_t));
        DWORD returned = 0;
        const bool created = DeviceIoControl(junction, FSCTL_SET_REPARSE_POINT,
            storage.data(), static_cast<DWORD>(storage.size()), nullptr, 0, &returned, nullptr) != FALSE;
        CloseHandle(junction);
        expect(created, "isolated junction reparse data should be set");
        expect((GetFileAttributesW(reparse.shortcutPath.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
            "the fixture should be a real reparse point");
        expect(!synchronizeInstallerShortcut(reparse).ok,
            "selected junction must not be overwritten");
        reparse.selected = false;
        expect(synchronizeInstallerShortcut(reparse).ok,
            "unselected junction should be ignored");
        expect(readFile(foreignDirectory / L"keep.txt") == untouched,
            "junction destination bytes should remain unchanged");
        expect(RemoveDirectoryW(reparse.shortcutPath.c_str()),
            "the isolated junction should be removed without following it");
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "BEGIN: COM initialization (ACP=" << GetACP() << ")\n";
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::cerr << "COM initialization failed\n"; return 1; }
    std::cout << "PASS: COM initialization\n";
    int result = 0;
    try {
        TemporaryDirectory directory;
        const auto runCase = [&directory](const wchar_t* name, const char* label,
                void (*test)(const std::filesystem::path&)) {
            const auto started = GetTickCount64();
            std::cout << "BEGIN: " << label << '\n';
            test(directory.path() / name);
            std::cout << "PASS: " << label << " (" << GetTickCount64() - started << " ms)\n";
        };
        runCase(L"new", "new and repeated selection", testNewAndRepeat);
        runCase(L"short-alias", "short path alias ownership", testShortPathAliasOwnership);
        runCase(L"unselected", "unselected foreign and invalid links", testUnselectedForeignAndInvalid);
        runCase(L"overwrite", "selected canonical replacement", testSelectedOverwrite);
        runCase(L"exact-target", "exact target ownership", testExactTargetOwnership);
        runCase(L"failure", "failed writes preserve the old link", testFailureKeepsOldLink);
        runCase(L"reparse-policy", "directory and reparse rejection", testRejectDirectoryAndReparse);
        std::cout << "BEGIN: staging inspection\n";
        expectNoStaging(directory.path());
        std::cout << "Installer shortcut tests passed (" << assertions << " assertions).\n";
        std::cout << "BEGIN: fixture cleanup\n";
    } catch (const std::exception& exception) {
        std::cerr << "Installer shortcut tests failed: " << exception.what() << '\n';
        result = 1;
    }
    std::cout << "BEGIN: COM cleanup\n";
    CoUninitialize();
    std::cout << "PASS: COM cleanup\n";
    return result;
}
