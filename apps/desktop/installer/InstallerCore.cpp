#include "InstallerCore.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>

namespace yanami::installer {
namespace {

class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
                CloseHandle(value_);
            }
            value_ = other.value_;
            other.value_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }
    void close() noexcept {
        if (valid()) {
            CloseHandle(value_);
            value_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE value_;
};

class FindHandle final {
public:
    explicit FindHandle(HANDLE value) noexcept : value_(value) {}
    ~FindHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            FindClose(value_);
        }
    }
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_;
};

std::wstring windowsError(const wchar_t* prefix, DWORD code = GetLastError()) {
    wchar_t* message = nullptr;
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    std::wstring result(prefix);
    if (count != 0 && message != nullptr) {
        result.append(L": ");
        result.append(message, count);
        while (!result.empty()
               && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
    }
    if (message != nullptr) {
        LocalFree(message);
    }
    return result;
}

std::uint32_t readLe32(const std::uint8_t* value) {
    return static_cast<std::uint32_t>(value[0])
        | (static_cast<std::uint32_t>(value[1]) << 8U)
        | (static_cast<std::uint32_t>(value[2]) << 16U)
        | (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::uint64_t readLe64(const std::uint8_t* value) {
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index) {
        result |= static_cast<std::uint64_t>(value[index]) << (index * 8U);
    }
    return result;
}

void writeLe32(std::uint8_t* destination, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        destination[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

void writeLe64(std::uint8_t* destination, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        destination[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

class BCryptAlgorithm final {
public:
    ~BCryptAlgorithm() {
        if (value != nullptr) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
    BCRYPT_ALG_HANDLE value = nullptr;
};

class BCryptHash final {
public:
    ~BCryptHash() {
        if (value != nullptr) {
            BCryptDestroyHash(value);
        }
    }
    BCRYPT_HASH_HANDLE value = nullptr;
};

bool beginSha256(
    BCryptAlgorithm& algorithm,
    BCryptHash& hash,
    std::vector<std::uint8_t>& object,
    std::wstring& error) {
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = L"BCryptOpenAlgorithmProvider(SHA-256) failed";
        return false;
    }

    DWORD objectLength = 0;
    DWORD resultLength = 0;
    status = BCryptGetProperty(
        algorithm.value,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength),
        sizeof(objectLength),
        &resultLength,
        0);
    if (!BCRYPT_SUCCESS(status) || objectLength == 0) {
        error = L"BCrypt could not report the SHA-256 object size";
        return false;
    }
    object.resize(objectLength);
    status = BCryptCreateHash(
        algorithm.value,
        &hash.value,
        object.data(),
        static_cast<ULONG>(object.size()),
        nullptr,
        0,
        0);
    if (!BCRYPT_SUCCESS(status)) {
        error = L"BCryptCreateHash(SHA-256) failed";
        return false;
    }
    return true;
}

bool finishSha256(
    BCryptHash& hash,
    std::array<std::uint8_t, 32>& digest,
    std::wstring& error) {
    const NTSTATUS status = BCryptFinishHash(
        hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = L"BCryptFinishHash(SHA-256) failed";
        return false;
    }
    return true;
}

bool readExactly(HANDLE file, void* destination, DWORD size, std::wstring& error) {
    auto* current = static_cast<std::uint8_t*>(destination);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(file, current, remaining, &read, nullptr)) {
            error = windowsError(L"Unable to read installer payload");
            return false;
        }
        if (read == 0) {
            error = L"Installer payload ended unexpectedly";
            return false;
        }
        current += read;
        remaining -= read;
    }
    return true;
}

PayloadResult processPayload(
    const std::wstring& sourcePath,
    const std::optional<std::wstring>& destinationPath) {
    PayloadResult result;
    Handle source(CreateFileW(
        sourcePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!source.valid()) {
        result.error = windowsError(L"Unable to open installer executable");
        return result;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(source.get(), &fileSize) || fileSize.QuadPart < 0) {
        result.error = windowsError(L"Unable to read installer size");
        return result;
    }
    if (static_cast<std::uint64_t>(fileSize.QuadPart) < kPayloadFooterSize) {
        result.error = L"Installer payload footer is missing";
        return result;
    }

    LARGE_INTEGER footerOffset{};
    footerOffset.QuadPart = -static_cast<LONGLONG>(kPayloadFooterSize);
    if (!SetFilePointerEx(source.get(), footerOffset, nullptr, FILE_END)) {
        result.error = windowsError(L"Unable to locate installer payload footer");
        return result;
    }
    std::array<std::uint8_t, kPayloadFooterSize> footer{};
    if (!readExactly(
            source.get(), footer.data(), static_cast<DWORD>(footer.size()),
            result.error)) {
        return result;
    }
    if (!parsePayloadFooter(
            footer,
            static_cast<std::uint64_t>(fileSize.QuadPart),
            result.metadata,
            result.error)) {
        return result;
    }

    LARGE_INTEGER payloadOffset{};
    payloadOffset.QuadPart = static_cast<LONGLONG>(result.metadata.payloadOffset);
    if (!SetFilePointerEx(source.get(), payloadOffset, nullptr, FILE_BEGIN)) {
        result.error = windowsError(L"Unable to locate embedded Velopack backend");
        return result;
    }

    Handle destination;
    const auto deleteDestination = [&]() {
        if (destinationPath.has_value()) {
            destination.close();
            DeleteFileW(destinationPath->c_str());
        }
    };
    if (destinationPath.has_value()) {
        destination = Handle(CreateFileW(
            destinationPath->c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr));
        if (!destination.valid()) {
            result.error = windowsError(L"Unable to create temporary Velopack backend");
            return result;
        }
    }

    BCryptAlgorithm algorithm;
    std::vector<std::uint8_t> hashObject;
    // Destroy the hash before releasing its caller-owned object buffer.
    BCryptHash hash;
    if (!beginSha256(algorithm, hash, hashObject, result.error)) {
        deleteDestination();
        return result;
    }

    std::array<std::uint8_t, 1024 * 1024> buffer{};
    std::uint64_t remaining = result.metadata.payloadSize;
    while (remaining > 0) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        DWORD read = 0;
        if (!ReadFile(source.get(), buffer.data(), requested, &read, nullptr)
            || read != requested) {
            result.error = windowsError(L"Unable to read embedded Velopack backend");
            deleteDestination();
            return result;
        }
        const NTSTATUS hashStatus = BCryptHashData(
            hash.value, buffer.data(), read, 0);
        if (!BCRYPT_SUCCESS(hashStatus)) {
            result.error = L"BCryptHashData(SHA-256) failed";
            deleteDestination();
            return result;
        }
        if (destination.valid()) {
            DWORD written = 0;
            if (!WriteFile(destination.get(), buffer.data(), read, &written, nullptr)
                || written != read) {
                result.error = windowsError(L"Unable to extract Velopack backend");
                deleteDestination();
                return result;
            }
        }
        remaining -= read;
    }

    std::array<std::uint8_t, 32> digest{};
    if (!finishSha256(hash, digest, result.error)) {
        deleteDestination();
        return result;
    }
    if (digest != result.metadata.payloadSha256) {
        result.error = L"Embedded Velopack backend SHA-256 does not match the footer";
        deleteDestination();
        return result;
    }
    if (destination.valid() && !FlushFileBuffers(destination.get())) {
        result.error = windowsError(L"Unable to flush temporary Velopack backend");
        deleteDestination();
        return result;
    }
    result.ok = true;
    return result;
}

bool hasDriveAbsolutePrefix(const std::wstring& path) {
    return path.size() >= 3
        && std::iswalpha(path[0]) != 0
        && path[1] == L':'
        && (path[2] == L'\\' || path[2] == L'/');
}

std::wstring trimTrailingSeparators(std::wstring path) {
    while (path.size() > 3
           && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

bool hasPathPrefix(
    const std::wstring& child,
    const std::wstring& parent,
    bool allowEqual) {
    const std::wstring normalizedChild = trimTrailingSeparators(child);
    const std::wstring normalizedParent = trimTrailingSeparators(parent);
    if (normalizedChild.size() < normalizedParent.size()) {
        return false;
    }
    if (_wcsnicmp(
            normalizedChild.c_str(), normalizedParent.c_str(),
            normalizedParent.size()) != 0) {
        return false;
    }
    if (normalizedChild.size() == normalizedParent.size()) {
        return allowEqual;
    }
    if (normalizedParent.size() == 3 && normalizedParent.back() == L'\\') {
        return true;
    }
    return normalizedChild[normalizedParent.size()] == L'\\';
}

bool hasUnsafeSegmentEnding(const std::wstring& normalized) {
    std::size_t start = 3;
    while (start < normalized.size()) {
        const std::size_t end = normalized.find(L'\\', start);
        const std::size_t length = (end == std::wstring::npos)
            ? normalized.size() - start
            : end - start;
        if (length == 0) {
            return true;
        }
        const wchar_t last = normalized[start + length - 1];
        if (last == L'.' || last == L' ') {
            return true;
        }
        start = end == std::wstring::npos ? normalized.size() : end + 1;
    }
    return false;
}

} // namespace

bool parsePayloadFooter(
    std::span<const std::uint8_t> footer,
    std::uint64_t fileSize,
    PayloadMetadata& metadata,
    std::wstring& error) {
    if (footer.size() != kPayloadFooterSize) {
        error = L"Installer payload footer has an invalid size";
        return false;
    }
    if (!std::equal(kPayloadMagic.begin(), kPayloadMagic.end(), footer.begin())) {
        error = L"Installer payload footer magic is missing";
        return false;
    }
    const std::uint64_t payloadSize = readLe64(footer.data() + 16);
    const std::uint32_t format = readLe32(footer.data() + 56);
    const std::uint32_t footerSize = readLe32(footer.data() + 60);
    if (format != kPayloadFormat) {
        error = L"Installer payload format is unsupported";
        return false;
    }
    if (footerSize != kPayloadFooterSize) {
        error = L"Installer payload footer length is invalid";
        return false;
    }
    if (payloadSize == 0) {
        error = L"Installer payload is empty";
        return false;
    }
    if (fileSize < kPayloadFooterSize
        || payloadSize > fileSize - kPayloadFooterSize) {
        error = L"Installer payload size exceeds the executable bounds";
        return false;
    }

    metadata.payloadSize = payloadSize;
    metadata.payloadOffset = fileSize - kPayloadFooterSize - payloadSize;
    std::copy_n(footer.begin() + 24, metadata.payloadSha256.size(),
                metadata.payloadSha256.begin());
    metadata.format = format;
    return true;
}

std::array<std::uint8_t, kPayloadFooterSize> serializePayloadFooter(
    std::uint64_t payloadSize,
    const std::array<std::uint8_t, 32>& payloadSha256) {
    std::array<std::uint8_t, kPayloadFooterSize> footer{};
    std::copy(kPayloadMagic.begin(), kPayloadMagic.end(), footer.begin());
    writeLe64(footer.data() + 16, payloadSize);
    std::copy(payloadSha256.begin(), payloadSha256.end(), footer.begin() + 24);
    writeLe32(footer.data() + 56, kPayloadFormat);
    writeLe32(footer.data() + 60, kPayloadFooterSize);
    return footer;
}

PayloadResult verifyPayloadFile(const std::wstring& sourcePath) {
    return processPayload(sourcePath, std::nullopt);
}

PayloadResult extractVerifiedPayload(
    const std::wstring& sourcePath,
    const std::wstring& destinationPath) {
    return processPayload(sourcePath, destinationPath);
}

bool verifyExtractedPayloadHandle(
    void* fileHandle,
    const PayloadMetadata& expected,
    std::wstring& error) {
    const HANDLE file = static_cast<HANDLE>(fileHandle);
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        error = L"The extracted Velopack backend handle is invalid";
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0) {
        error = windowsError(L"Unable to read the extracted Velopack backend size");
        return false;
    }
    if (static_cast<std::uint64_t>(fileSize.QuadPart) != expected.payloadSize) {
        error = L"Extracted Velopack backend size does not match the installer payload";
        return false;
    }

    LARGE_INTEGER zero{};
    LARGE_INTEGER originalPosition{};
    if (!SetFilePointerEx(file, zero, &originalPosition, FILE_CURRENT)) {
        error = windowsError(L"Unable to read the extracted Velopack backend position");
        return false;
    }
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
        error = windowsError(L"Unable to seek the extracted Velopack backend");
        return false;
    }

    const auto restorePosition = [&]() {
        if (!SetFilePointerEx(file, originalPosition, nullptr, FILE_BEGIN)) {
            error = windowsError(L"Unable to restore the extracted Velopack backend position");
            return false;
        }
        return true;
    };

    BCryptAlgorithm algorithm;
    std::vector<std::uint8_t> hashObject;
    // Destroy the hash before releasing its caller-owned object buffer.
    BCryptHash hash;
    if (!beginSha256(algorithm, hash, hashObject, error)) {
        restorePosition();
        return false;
    }

    std::array<std::uint8_t, 1024 * 1024> buffer{};
    std::uint64_t remaining = expected.payloadSize;
    while (remaining > 0) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), requested, &read, nullptr)) {
            error = windowsError(L"Unable to read the extracted Velopack backend");
            restorePosition();
            return false;
        }
        if (read == 0) {
            error = L"Extracted Velopack backend ended unexpectedly";
            restorePosition();
            return false;
        }
        const NTSTATUS hashStatus = BCryptHashData(
            hash.value, buffer.data(), read, 0);
        if (!BCRYPT_SUCCESS(hashStatus)) {
            error = L"BCryptHashData(SHA-256) failed";
            restorePosition();
            return false;
        }
        remaining -= read;
    }

    std::array<std::uint8_t, 32> digest{};
    if (!finishSha256(hash, digest, error)) {
        restorePosition();
        return false;
    }
    if (digest != expected.payloadSha256) {
        error = L"Extracted Velopack backend SHA-256 does not match the installer payload";
        restorePosition();
        return false;
    }
    return restorePosition();
}

std::optional<std::array<std::uint8_t, 32>> sha256Bytes(
    std::span<const std::uint8_t> bytes,
    std::wstring& error) {
    BCryptAlgorithm algorithm;
    std::vector<std::uint8_t> object;
    // Destroy the hash before releasing its caller-owned object buffer.
    BCryptHash hash;
    if (!beginSha256(algorithm, hash, object, error)) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ULONG length = static_cast<ULONG>(std::min<std::size_t>(
            bytes.size() - offset,
            std::numeric_limits<ULONG>::max()));
        const NTSTATUS status = BCryptHashData(
            hash.value,
            const_cast<PUCHAR>(bytes.data() + offset),
            length,
            0);
        if (!BCRYPT_SUCCESS(status)) {
            error = L"BCryptHashData(SHA-256) failed";
            return std::nullopt;
        }
        offset += length;
    }
    std::array<std::uint8_t, 32> digest{};
    if (!finishSha256(hash, digest, error)) {
        return std::nullopt;
    }
    return digest;
}

std::string hexLower(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::uint8_t value : bytes) {
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

CommandLineResult parseCommandLine(
    const std::vector<std::wstring>& arguments) {
    CommandLineResult result;
    if (arguments.empty()) {
        result.ok = true;
        result.options.mode = LaunchMode::Gui;
        return result;
    }
    if (arguments.size() == 1 && arguments[0] == L"--verify-payload") {
        result.ok = true;
        result.options.mode = LaunchMode::VerifyPayload;
        return result;
    }

    CommandLineOptions options;
    options.mode = LaunchMode::SilentInstall;
    bool sawSilent = false;
    bool sawInstallDirectory = false;
    bool sawStartMenu = false;
    bool sawDesktop = false;
    bool sawNoLaunch = false;

    auto parseYesNo = [&](const std::wstring& value, bool& destination) {
        if (value == L"yes") {
            destination = true;
            return true;
        }
        if (value == L"no") {
            destination = false;
            return true;
        }
        return false;
    };

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--silent") {
            if (sawSilent) {
                result.error = L"--silent was specified more than once";
                return result;
            }
            sawSilent = true;
        } else if (argument == L"--install-dir") {
            if (sawInstallDirectory || index + 1 >= arguments.size()) {
                result.error = L"--install-dir requires one path";
                return result;
            }
            options.installDirectory = arguments[++index];
            sawInstallDirectory = true;
        } else if (argument == L"--start-menu") {
            if (sawStartMenu || index + 1 >= arguments.size()
                || !parseYesNo(arguments[index + 1], options.startMenu)) {
                result.error = L"--start-menu requires yes or no";
                return result;
            }
            ++index;
            sawStartMenu = true;
        } else if (argument == L"--desktop") {
            if (sawDesktop || index + 1 >= arguments.size()
                || !parseYesNo(arguments[index + 1], options.desktop)) {
                result.error = L"--desktop requires yes or no";
                return result;
            }
            ++index;
            sawDesktop = true;
        } else if (argument == L"--no-launch") {
            if (sawNoLaunch) {
                result.error = L"--no-launch was specified more than once";
                return result;
            }
            options.launchAfterInstall = false;
            sawNoLaunch = true;
        } else {
            result.error = L"Unknown installer argument: " + argument;
            return result;
        }
    }

    if (!sawSilent || !sawInstallDirectory || !sawStartMenu || !sawDesktop) {
        result.error = L"Silent install requires --install-dir, --start-menu and --desktop";
        return result;
    }
    if (options.installDirectory.empty()) {
        result.error = L"--install-dir cannot be empty";
        return result;
    }
    result.ok = true;
    result.options = std::move(options);
    return result;
}

std::optional<std::wstring> normalizeAbsolutePath(
    const std::wstring& path,
    std::wstring& error) {
    if (path.empty()) {
        error = L"The installation directory cannot be empty.";
        return std::nullopt;
    }
    if (std::any_of(path.begin(), path.end(), [](wchar_t value) {
            return value < L' ' || value == L'"' || value == L'<'
                || value == L'>' || value == L'|' || value == L'?'
                || value == L'*';
        })) {
        error = L"The installation directory contains an invalid character.";
        return std::nullopt;
    }
    if (!hasDriveAbsolutePrefix(path)) {
        error = L"The installation directory must be an absolute path on a local drive.";
        return std::nullopt;
    }
    if (path.find(L':', 2) != std::wstring::npos) {
        error = L"The installation directory cannot contain an alternate data stream.";
        return std::nullopt;
    }
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        error = windowsError(L"Unable to normalize the installation directory");
        return std::nullopt;
    }
    std::wstring normalized(required, L'\0');
    const DWORD written = GetFullPathNameW(
        path.c_str(), required, normalized.data(), nullptr);
    if (written == 0 || written >= required) {
        error = windowsError(L"Unable to normalize the installation directory");
        return std::nullopt;
    }
    normalized.resize(written);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    normalized = trimTrailingSeparators(std::move(normalized));
    if (hasUnsafeSegmentEnding(normalized)) {
        error = L"Installation directory names cannot end with a space or period.";
        return std::nullopt;
    }
    return normalized;
}

bool samePath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(
        trimTrailingSeparators(left).c_str(),
        trimTrailingSeparators(right).c_str()) == 0;
}

bool pathWithin(
    const std::wstring& child,
    const std::wstring& parent,
    bool allowEqual) {
    return hasPathPrefix(child, parent, allowEqual);
}

PathValidationResult validateInstallPathPolicy(
    const std::wstring& candidate,
    const std::optional<std::wstring>& registeredInstall,
    const PathPolicyRoots& roots,
    ExistingPathState existingState) {
    PathValidationResult result;
    std::wstring error;
    const auto normalized = normalizeAbsolutePath(candidate, error);
    if (!normalized.has_value()) {
        result.error = std::move(error);
        return result;
    }
    result.normalizedPath = *normalized;
    if (result.normalizedPath.size() == 3
        && result.normalizedPath[1] == L':') {
        result.error = L"A drive root cannot be used as the installation directory.";
        return result;
    }

    // Registration authorizes repairing an existing installation, not writing
    // into protected system roots. Apply these boundaries before the repair path.
    for (const std::wstring* protectedRoot : {
             &roots.windowsDirectory,
             &roots.programFiles,
             &roots.programFilesX86}) {
        if (protectedRoot->empty()) {
            continue;
        }
        std::wstring protectedError;
        const auto normalizedProtected = normalizeAbsolutePath(
            *protectedRoot, protectedError);
        if (normalizedProtected.has_value()
            && pathWithin(result.normalizedPath, *normalizedProtected, true)) {
            result.error = L"Yanami cannot be installed inside Windows or Program Files.";
            return result;
        }
    }

    if (registeredInstall.has_value()) {
        std::wstring installedError;
        const auto installed = normalizeAbsolutePath(
            *registeredInstall, installedError);
        if (!installed.has_value()) {
            result.error = L"The registered installation directory is invalid: " + installedError;
            return result;
        }
        if (!samePath(result.normalizedPath, *installed)) {
            result.error = L"An existing Yanami installation must be upgraded or repaired in its registered directory.";
            return result;
        }
        if (existingState == ExistingPathState::NotDirectory
            || existingState == ExistingPathState::Inaccessible) {
            result.error = L"The existing installation directory is not accessible.";
            return result;
        }
        result.ok = true;
        result.existingInstall = true;
        return result;
    }

    if (existingState == ExistingPathState::NotDirectory) {
        result.error = L"The installation directory is occupied by a file.";
        return result;
    }
    if (existingState == ExistingPathState::NonEmptyDirectory) {
        result.error = L"A new installation directory must be empty.";
        return result;
    }
    if (existingState == ExistingPathState::Inaccessible) {
        result.error = L"The installation directory cannot be inspected.";
        return result;
    }

    result.ok = true;
    return result;
}

ExistingPathState inspectExistingPath(const std::wstring& normalizedPath) {
    // Never follow an existing junction/symlink, including an ancestor of a
    // missing target: a harmless-looking path could otherwise enter Windows,
    // another installation, or unrelated user data through a reparse point.
    std::size_t componentEnd = 3;
    while (componentEnd <= normalizedPath.size()) {
        const DWORD componentAttributes = GetFileAttributesW(
            normalizedPath.substr(0, componentEnd).c_str());
        if (componentAttributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? ExistingPathState::Missing : ExistingPathState::Inaccessible;
        }
        if ((componentAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return ExistingPathState::Inaccessible;
        }
        if ((componentAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return ExistingPathState::NotDirectory;
        }
        if (componentEnd == normalizedPath.size()) {
            break;
        }
        componentEnd = normalizedPath.find(L'\\', componentEnd + 1);
        if (componentEnd == std::wstring::npos) {
            componentEnd = normalizedPath.size();
        }
    }
    const DWORD attributes = GetFileAttributesW(normalizedPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND
                || GetLastError() == ERROR_PATH_NOT_FOUND
            ? ExistingPathState::Missing
            : ExistingPathState::Inaccessible;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return ExistingPathState::NotDirectory;
    }

    std::wstring pattern = normalizedPath;
    if (!pattern.empty() && pattern.back() != L'\\') {
        pattern.push_back(L'\\');
    }
    pattern.append(L"*");
    WIN32_FIND_DATAW data{};
    FindHandle find(FindFirstFileW(pattern.c_str(), &data));
    if (!find.valid()) {
        return GetLastError() == ERROR_FILE_NOT_FOUND
            ? ExistingPathState::EmptyDirectory
            : ExistingPathState::Inaccessible;
    }
    do {
        if (wcscmp(data.cFileName, L".") != 0
            && wcscmp(data.cFileName, L"..") != 0) {
            return ExistingPathState::NonEmptyDirectory;
        }
    } while (FindNextFileW(find.get(), &data));
    return GetLastError() == ERROR_NO_MORE_FILES
        ? ExistingPathState::EmptyDirectory
        : ExistingPathState::Inaccessible;
}

PathValidationResult resolveInstallDirectory(
    const std::wstring& selected,
    const std::optional<std::wstring>& registeredInstall,
    const PathPolicyRoots& roots) {
    // Validate the selection's syntax and protected boundaries before deriving a
    // child. Registration is checked against each actual installation target.
    const auto policy = validateInstallPathPolicy(
        selected, std::nullopt, roots, ExistingPathState::Missing);
    if (!policy.ok) {
        return policy;
    }
    const auto state = inspectExistingPath(policy.normalizedPath);
    auto result = validateInstallPathPolicy(
        policy.normalizedPath, registeredInstall, roots, state);
    if (result.ok || state != ExistingPathState::NonEmptyDirectory) {
        return result;
    }

    const std::filesystem::path parent(policy.normalizedPath);
    std::optional<std::wstring> normalizedRegistered;
    if (registeredInstall.has_value()) {
        std::wstring registeredError;
        normalizedRegistered = normalizeAbsolutePath(*registeredInstall, registeredError);
        if (!normalizedRegistered.has_value()
            || !samePath(std::filesystem::path(*normalizedRegistered).parent_path().wstring(),
                         parent.wstring())) {
            return result;
        }
    }
    for (unsigned suffix = 1; suffix <= 100; ++suffix) {
        const std::wstring name = suffix == 1 ? L"Yanami"
            : L"Yanami (" + std::to_wstring(suffix) + L")";
        const auto proposed = (parent / name).wstring();
        if (normalizedRegistered.has_value()
            && !samePath(proposed, *normalizedRegistered)) {
            continue;
        }
        const auto proposedState = inspectExistingPath(proposed);
        const auto validation = validateInstallPathPolicy(
            proposed, registeredInstall, roots, proposedState);
        if (validation.ok || normalizedRegistered.has_value()) {
            return validation;
        }
    }
    if (!registeredInstall.has_value()) {
        result.error = L"No available installation directory was found inside the selected folder.";
    }
    return result;
}

std::wstring quoteWindowsArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted;
    quoted.push_back(L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
            continue;
        }
        if (value == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(value);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

} // namespace yanami::installer
