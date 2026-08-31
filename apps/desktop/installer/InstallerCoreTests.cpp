#include "InstallerCore.hpp"
#include "InstallerLayout.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

    expect(pathWithin(L"C:\\Apps\\Yanami\\current", L"c:\\apps\\yanami"),
           "path prefix should be case insensitive");
    expect(!pathWithin(L"C:\\Apps\\Yanami2", L"C:\\Apps\\Yanami"),
           "path prefix should respect directory boundaries");
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
