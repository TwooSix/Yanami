#include "InstallerCore.hpp"
#include "InstallerLayout.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using namespace yanami::installer;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path makeTemporaryDirectory() {
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("GetTempPathW failed");
    }
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto name = std::wstring(L"yanami-installer-core-tests-")
            + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64()) + L"-"
            + std::to_wstring(attempt);
        const std::filesystem::path candidate =
            std::filesystem::path(buffer.data()) / name;
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
    }
    throw std::runtime_error("unable to create temporary test directory");
}

void writeFixture(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& stub,
    const std::vector<std::uint8_t>& payload) {
    std::wstring error;
    const auto digest = sha256Bytes(payload, error);
    expect(digest.has_value(), "fixture sha256 should succeed");
    const auto footer = serializePayloadFooter(payload.size(), *digest);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    expect(stream.good(), "fixture should open");
    stream.write(reinterpret_cast<const char*>(stub.data()),
                 static_cast<std::streamsize>(stub.size()));
    stream.write(reinterpret_cast<const char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
    stream.write(reinterpret_cast<const char*>(footer.data()),
                 static_cast<std::streamsize>(footer.size()));
    expect(stream.good(), "fixture should be written");
}

void testFooterAndPayload() {
    const std::vector<std::uint8_t> payload = {
        'f', 'a', 'k', 'e', '-', 'v', 'e', 'l', 'o', 'p', 'a', 'c', 'k'};
    std::wstring hashError;
    const auto digest = sha256Bytes(payload, hashError);
    expect(digest.has_value(), "sha256 should succeed");
    auto footer = serializePayloadFooter(payload.size(), *digest);
    PayloadMetadata metadata;
    std::wstring error;
    expect(parsePayloadFooter(footer, 512 + payload.size() + footer.size(),
                              metadata, error),
           "valid footer should parse");
    expect(metadata.payloadOffset == 512, "payload offset should be derived");
    expect(metadata.payloadSize == payload.size(), "payload size should match");
    expect(metadata.payloadSha256 == *digest, "footer hash should match");
    expect(metadata.format == 1, "footer format should match");

    footer[0] ^= 0xff;
    expect(!parsePayloadFooter(footer, 1024, metadata, error),
           "bad footer magic should fail");
    footer = serializePayloadFooter(payload.size(), *digest);
    footer[56] = 2;
    expect(!parsePayloadFooter(footer, 1024, metadata, error),
           "unsupported footer format should fail");
    footer = serializePayloadFooter(4096, *digest);
    expect(!parsePayloadFooter(footer, 1024, metadata, error),
           "out-of-bounds payload should fail");
}

void testPayloadFileVerification(const std::filesystem::path& directory) {
    const std::vector<std::uint8_t> stub = {'M', 'Z', 0, 0, 1, 2, 3, 4};
    const std::vector<std::uint8_t> payload = {
        'V', 'e', 'l', 'o', 'p', 'a', 'c', 'k', '-', 'b', 'a', 'c', 'k', 'e', 'n', 'd'};
    const auto fixture = directory / L"fixture.exe";
    const auto extracted = directory / L"backend.exe";
    writeFixture(fixture, stub, payload);

    const PayloadResult verified = verifyPayloadFile(fixture.wstring());
    expect(verified.ok, "valid payload file should verify");
    expect(verified.metadata.payloadOffset == stub.size(),
           "file payload offset should match stub size");
    expect(!std::filesystem::exists(extracted),
           "verify-only must not extract payload");

    const PayloadResult extractedResult = extractVerifiedPayload(
        fixture.wstring(), extracted.wstring());
    expect(extractedResult.ok, "valid payload should extract");
    std::ifstream stream(extracted, std::ios::binary);
    const std::vector<std::uint8_t> extractedBytes(
        std::istreambuf_iterator<char>(stream), {});
    expect(extractedBytes == payload, "extracted payload should be exact");
    stream.close();

    HANDLE backend = CreateFileW(
        extracted.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    expect(backend != INVALID_HANDLE_VALUE,
           "extracted backend handle should open");
    SetLastError(ERROR_SUCCESS);
    HANDLE writer = CreateFileW(
        extracted.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD writerError = GetLastError();
    expect(writer == INVALID_HANDLE_VALUE
               && writerError == ERROR_SHARING_VIOLATION,
           "backend read lock should deny a second write handle");
    if (writer != INVALID_HANDLE_VALUE) {
        CloseHandle(writer);
    }
    SetLastError(ERROR_SUCCESS);
    HANDLE deleter = CreateFileW(
        extracted.c_str(), DELETE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD deleterError = GetLastError();
    expect(deleter == INVALID_HANDLE_VALUE
               && deleterError == ERROR_SHARING_VIOLATION,
           "backend read lock should deny a second delete handle");
    if (deleter != INVALID_HANDLE_VALUE) {
        CloseHandle(deleter);
    }
    LARGE_INTEGER initialPosition{};
    initialPosition.QuadPart = 3;
    expect(SetFilePointerEx(backend, initialPosition, nullptr, FILE_BEGIN) != FALSE,
           "backend handle position should be adjustable");
    std::wstring handleError;
    expect(verifyExtractedPayloadHandle(
               backend, extractedResult.metadata, handleError),
           "already-open extracted backend should verify");
    LARGE_INTEGER zero{};
    LARGE_INTEGER restoredPosition{};
    expect(SetFilePointerEx(
               backend, zero, &restoredPosition, FILE_CURRENT) != FALSE
               && restoredPosition.QuadPart == initialPosition.QuadPart,
           "backend verification should preserve file position");

    PayloadMetadata wrongMetadata = extractedResult.metadata;
    ++wrongMetadata.payloadSize;
    expect(!verifyExtractedPayloadHandle(backend, wrongMetadata, handleError),
           "backend verification should reject a size mismatch");
    wrongMetadata = extractedResult.metadata;
    wrongMetadata.payloadSha256[0] ^= 0xff;
    expect(!verifyExtractedPayloadHandle(backend, wrongMetadata, handleError),
           "backend verification should reject a hash mismatch");
    CloseHandle(backend);

    std::fstream corrupt(fixture, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(static_cast<std::streamoff>(stub.size() + 1));
    corrupt.put('X');
    corrupt.close();
    expect(!verifyPayloadFile(fixture.wstring()).ok,
           "corrupted payload should fail hash verification");
}

void testCommandLine() {
    auto result = parseCommandLine({});
    expect(result.ok && result.options.mode == LaunchMode::Gui,
           "empty command line should select GUI");

    result = parseCommandLine({L"--verify-payload"});
    expect(result.ok && result.options.mode == LaunchMode::VerifyPayload,
           "verify command should parse");

    result = parseCommandLine({
        L"--silent", L"--install-dir", L"D:\\Apps\\Yanami",
        L"--start-menu", L"no", L"--desktop", L"yes", L"--no-launch"});
    expect(result.ok && result.options.mode == LaunchMode::SilentInstall,
           "complete silent command should parse");
    expect(!result.options.startMenu && result.options.desktop
               && !result.options.launchAfterInstall,
           "silent flags should be retained");

    expect(!parseCommandLine({L"--silent"}).ok,
           "incomplete silent command should fail");
    expect(!parseCommandLine({L"--verify-payload", L"--silent"}).ok,
           "mixed verify command should fail");
    expect(!parseCommandLine({
        L"--silent", L"--install-dir", L"D:\\Apps\\Yanami",
        L"--start-menu", L"maybe", L"--desktop", L"no"}).ok,
        "invalid yes/no should fail");
    expect(!parseCommandLine({L"--unknown"}).ok,
           "unknown command should fail");
}

void testPathPolicy() {
    const PathPolicyRoots roots{
        .windowsDirectory = L"C:\\Windows",
        .programFiles = L"C:\\Program Files",
        .programFilesX86 = L"C:\\Program Files (x86)",
    };
    auto result = validateInstallPathPolicy(
        L"D:\\Apps\\Yanami", std::nullopt, roots,
        ExistingPathState::Missing);
    expect(result.ok, "writable-policy candidate outside profile should pass");

    expect(!validateInstallPathPolicy(
                L"relative\\Yanami", std::nullopt, roots,
                ExistingPathState::Missing).ok,
           "relative path should fail");
    expect(!validateInstallPathPolicy(
                L"C:\\", std::nullopt, roots,
                ExistingPathState::EmptyDirectory).ok,
           "drive root should fail");
    expect(!validateInstallPathPolicy(
                L"C:\\Windows\\Yanami", std::nullopt, roots,
                ExistingPathState::Missing).ok,
           "Windows child should fail");
    expect(!validateInstallPathPolicy(
                L"C:\\Program Files\\Yanami", std::nullopt, roots,
                ExistingPathState::Missing).ok,
           "Program Files child should fail");
    expect(!validateInstallPathPolicy(
                L"D:\\Apps\\Yanami", std::nullopt, roots,
                ExistingPathState::NonEmptyDirectory).ok,
           "unmanaged non-empty directory should fail");

    result = validateInstallPathPolicy(
        L"d:\\apps\\YANAMI\\",
        std::optional<std::wstring>(L"D:\\Apps\\Yanami"),
        roots,
        ExistingPathState::NonEmptyDirectory);
    expect(result.ok && result.existingInstall,
           "registered install should allow its non-empty directory");
    expect(!validateInstallPathPolicy(
                L"D:\\Other\\Yanami",
                std::optional<std::wstring>(L"D:\\Apps\\Yanami"),
                roots,
                ExistingPathState::Missing).ok,
           "registered install should reject a second directory");

    for (const auto& protectedPath : {
             L"C:\\", L"C:\\Windows\\Yanami",
             L"C:\\Program Files\\Yanami", L"C:\\Program Files (x86)\\Yanami"}) {
        expect(!validateInstallPathPolicy(
                    protectedPath, std::optional<std::wstring>(protectedPath),
                    roots, ExistingPathState::NonEmptyDirectory).ok,
               "registration must not bypass protected installation roots");
        expect(!resolveInstallDirectory(protectedPath, std::nullopt, roots).ok,
               "protected selections must not produce an alternate installation path");
    }
    for (const auto state : {ExistingPathState::Missing,
                            ExistingPathState::EmptyDirectory}) {
        expect(validateInstallPathPolicy(
                   L"D:\\Apps\\Yanami", std::nullopt, roots, state).ok,
               "new missing and empty destinations must be accepted");
        expect(validateInstallPathPolicy(
                   L"D:\\Apps\\Yanami", std::wstring(L"D:\\Apps\\Yanami"),
                   roots, state).existingInstall,
               "an incomplete registered install may be repaired in place");
    }
    expect(!validateInstallPathPolicy(
                L"D:\\Apps\\Yanami", std::wstring(L"D:\\Apps\\Yanami"),
                roots, ExistingPathState::Inaccessible).ok,
           "registration must not bypass inaccessible or redirected paths");
    expect(!validateInstallPathPolicy(
                L"D:\\Apps\\Yanami", std::wstring(L"D:\\Apps\\Yanami"),
                roots, ExistingPathState::NotDirectory).ok,
           "registration must not authorize replacing an ordinary file");

    expect(pathWithin(L"C:\\Apps\\Yanami\\current", L"c:\\apps\\yanami"),
           "path prefix should be case insensitive");
    expect(!pathWithin(L"C:\\Apps\\Yanami2", L"C:\\Apps\\Yanami"),
           "path prefix should respect directory boundaries");
    expect(pathWithin(L"C:\\Apps\\Yanami", L"C:\\"),
           "a drive root must contain paths on that drive");
    expect(!pathWithin(L"D:\\Apps\\Yanami", L"C:\\"),
           "a drive root must not contain paths on another drive");
    expect(!validateInstallPathPolicy(
                L"D:\\Apps\\Invalid*Name", std::nullopt, roots,
                ExistingPathState::Missing).ok,
           "invalid Win32 filename characters must fail before installation");
}

void createJunction(
    const std::filesystem::path& link,
    const std::filesystem::path& target) {
    expect(std::filesystem::create_directory(link), "junction directory should be created");
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const std::wstring print = target.wstring();
    struct MountPointHeader {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
    };
    static_assert(sizeof(MountPointHeader) == 16);
    const auto substituteBytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    const auto printBytes = static_cast<WORD>(print.size() * sizeof(wchar_t));
    const WORD printOffset = substituteBytes + sizeof(wchar_t);
    std::vector<std::uint8_t> buffer(
        sizeof(MountPointHeader) + printOffset + printBytes + sizeof(wchar_t));
    const MountPointHeader header{
        IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(buffer.size() - 8), 0,
        0, substituteBytes, printOffset, printBytes};
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.data(), substituteBytes);
    std::memcpy(buffer.data() + sizeof(header) + printOffset, print.data(), printBytes);
    HANDLE handle = CreateFileW(
        link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    expect(handle != INVALID_HANDLE_VALUE, "junction handle should open");
    DWORD returned = 0;
    const BOOL configured = DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    expect(configured != FALSE, "junction should be configured without following it");
}

void testDirectoryResolution(const std::filesystem::path& directory) {
    const PathPolicyRoots roots{};
    const auto base = directory / L"directory-resolution";
    std::filesystem::create_directory(base);

    for (const auto& name : {L"Custom Empty Folder", L"Yanami", L"Yanami (2)"}) {
        const auto selected = base / name;
        auto result = resolveInstallDirectory(selected.wstring(), std::nullopt, roots);
        expect(result.ok && samePath(result.normalizedPath, selected.wstring()),
               "a missing selected directory must be used directly regardless of its name");
        expect(!std::filesystem::exists(selected), "resolution must not create missing folders");
        std::filesystem::create_directory(selected);
        result = resolveInstallDirectory(selected.wstring(), std::nullopt, roots);
        expect(result.ok && samePath(result.normalizedPath, selected.wstring()),
               "an empty selected directory must be used directly regardless of its name");
    }

    const auto library = base / L"用户 软件";
    std::filesystem::create_directory(library);
    const auto document = library / L"资料.txt";
    {
        std::ofstream stream(document);
        stream << "must remain untouched";
    }
    const auto firstChild = resolveInstallDirectory(
        library.wstring(), std::nullopt, roots);
    expect(firstChild.ok && samePath(firstChild.normalizedPath, (library / L"Yanami").wstring()),
           "a non-empty selected parent must automatically resolve to its Yanami child");
    expect(!std::filesystem::exists(firstChild.normalizedPath),
           "resolving the final directory must not create it");
    expect(!validateInstallPathPolicy(
                library.wstring(), std::nullopt, roots,
                inspectExistingPath(library.wstring())).ok,
           "the worker's strict policy must still reject the non-empty original parent");
    std::filesystem::create_directory(firstChild.normalizedPath);
    const auto emptyChild = resolveInstallDirectory(library.wstring(), std::nullopt, roots);
    expect(emptyChild.ok && samePath(emptyChild.normalizedPath, firstChild.normalizedPath),
           "an existing empty Yanami child must be reused without adding another level");
    const auto resolvedAgain = resolveInstallDirectory(firstChild.normalizedPath, std::nullopt, roots);
    expect(resolvedAgain.ok && samePath(resolvedAgain.normalizedPath, firstChild.normalizedPath),
           "an empty final directory must not receive a redundant Yanami child");
    {
        std::ofstream marker(std::filesystem::path(firstChild.normalizedPath) / L"sq.version");
        marker << "not proof of installer ownership";
    }
    auto occupiedChild = resolveInstallDirectory(library.wstring(), std::nullopt, roots);
    expect(occupiedChild.ok
               && samePath(occupiedChild.normalizedPath, (library / L"Yanami (2)").wstring()),
           "an unregistered non-empty child must select its sibling, never descend through it");
    expect(!validateInstallPathPolicy(
                firstChild.normalizedPath, std::nullopt, roots,
                inspectExistingPath(firstChild.normalizedPath)).ok,
           "a familiar payload marker must not authorize overwriting an unknown child");
    const auto sibling2 = library / L"Yanami (2)";
    {
        std::ofstream occupied(sibling2);
        occupied << "ordinary user file";
    }
    std::filesystem::create_directories(library / L"Yanami (3)" / L"user-files");
    const auto sibling4 = library / L"Yanami (4)";
    std::filesystem::create_directory(sibling4);
    HANDLE busyFile = CreateFileW(
        sibling2.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(busyFile != INVALID_HANDLE_VALUE, "ordinary user file should be locked for the fixture");
    const auto whileBusy = resolveInstallDirectory(library.wstring(), std::nullopt, roots);
    const auto selectedFile = resolveInstallDirectory(sibling2.wstring(), std::nullopt, roots);
    CloseHandle(busyFile);
    expect(whileBusy.ok && samePath(whileBusy.normalizedPath, sibling4.wstring()),
           "occupied child files must be skipped in favor of a safe child under the same parent");
    expect(!selectedFile.ok, "selecting an ordinary file must fail without choosing an alternative");
    std::ifstream unchanged(sibling2);
    expect(std::string(std::istreambuf_iterator<char>(unchanged), {}) == "ordinary user file",
           "directory resolution must preserve ordinary file content");
    unchanged.close();
    expect(!resolveInstallDirectory((sibling2 / L"child").wstring(), std::nullopt, roots).ok,
           "a file in an ancestor must not be treated as a writable parent");

    const auto namedYanami = base / L"Yanami";
    std::filesystem::create_directory(namedYanami / L"personal");
    const auto namedResult = resolveInstallDirectory(namedYanami.wstring(), std::nullopt, roots);
    expect(namedResult.ok && samePath(namedResult.normalizedPath, (namedYanami / L"Yanami").wstring()),
           "a non-empty selected folder named Yanami must still use its own child, not a sibling");

    for (const auto& registered : {
             std::filesystem::path(firstChild.normalizedPath), library / L"Yanami (3)"}) {
        const auto directRepair = resolveInstallDirectory(registered.wstring(), registered.wstring(), roots);
        expect(directRepair.ok && directRepair.existingInstall
                   && samePath(directRepair.normalizedPath, registered.wstring()),
               "selecting the registered non-empty directory must repair it in place");
        const auto parentRepair = resolveInstallDirectory(library.wstring(), registered.wstring(), roots);
        expect(parentRepair.ok && parentRepair.existingInstall
                   && samePath(parentRepair.normalizedPath, registered.wstring()),
               "selecting the parent of a registered Yanami child must resolve to its existing installation");
    }
    expect(!resolveInstallDirectory(
                library.wstring(), (base / L"different-install").wstring(), roots).ok,
           "a registered installation elsewhere must not be silently duplicated");

    const auto target = base / L"protected-target";
    std::filesystem::create_directory(target);
    const auto junction = base / L"redirect";
    createJunction(junction, target);
    for (const auto& redirected : {junction, junction / L"missing"}) {
        const auto state = inspectExistingPath(redirected.wstring());
        expect(state == ExistingPathState::Inaccessible,
               "direct or ancestor reparse points must not be followed");
        expect(!validateInstallPathPolicy(
                    redirected.wstring(), redirected.wstring(), roots, state).ok,
               "registration must not authorize writes through a junction");
        expect(!resolveInstallDirectory(redirected.wstring(), std::nullopt, roots).ok,
               "a redirected selection must not produce an alternative child");
    }
    expect(RemoveDirectoryW(junction.c_str()) != FALSE, "test junction should be removed without following it");
    expect(std::filesystem::is_empty(target), "junction inspection must leave its target untouched");

    const auto exhausted = base / L"bounded-search";
    std::filesystem::create_directory(exhausted);
    for (unsigned suffix = 1; suffix <= 100; ++suffix) {
        const std::wstring name = suffix == 1 ? L"Yanami"
            : L"Yanami (" + std::to_wstring(suffix) + L")";
        std::ofstream occupied(exhausted / name);
        occupied << "keep this file";
    }
    expect(!resolveInstallDirectory(exhausted.wstring(), std::nullopt, roots).ok,
           "an exhausted child search must stop without touching occupied paths");
}

void testArgumentQuoting() {
    expect(quoteWindowsArgument(L"plain") == L"plain",
           "plain argument should not be quoted");
    expect(quoteWindowsArgument(L"") == L"\"\"",
           "empty argument should be quoted");
    expect(quoteWindowsArgument(L"C:\\Some Path\\")
               == L"\"C:\\Some Path\\\\\"",
           "trailing slash should be escaped in quoted argument");
}

void testPathLineCentering() {
    for (const int dpi : {96, 144, 192}) {
        const RECT field{MulDiv(80, dpi, 96), MulDiv(172, dpi, 96),
                         MulDiv(556, dpi, 96), MulDiv(224, dpi, 96)};
        // Cover differing CJK/Latin font cell metrics and odd-pixel rounding.
        for (const int logicalHeight : {14, 17, 18, 21}) {
            const int lineHeight = MulDiv(logicalHeight, dpi, 96);
            const RECT line = ui::centeredTextLineRect(field, lineHeight);
            const int topPadding = line.top - field.top;
            const int bottomPadding = field.bottom - line.bottom;
            expect(line.left == field.left && line.right == field.right,
                   "centering must preserve the path's available width");
            expect(line.bottom - line.top == lineHeight,
                   "centering must preserve the complete font cell");
            expect(topPadding >= 0 && bottomPadding >= topPadding
                       && bottomPadding - topPadding <= 1,
                   "path font cell must be centered to within one physical pixel");
        }
    }
}

} // namespace

int main() {
    std::filesystem::path temporaryDirectory;
    try {
        temporaryDirectory = makeTemporaryDirectory();
        testFooterAndPayload();
        testPayloadFileVerification(temporaryDirectory);
        testCommandLine();
        testPathPolicy();
        testDirectoryResolution(temporaryDirectory);
        testArgumentQuoting();
        testPathLineCentering();
        std::filesystem::remove_all(temporaryDirectory);
        std::cout << "Yanami installer core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        if (!temporaryDirectory.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(temporaryDirectory, ignored);
        }
        std::cerr << "Yanami installer core tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
