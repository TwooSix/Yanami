#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace yanami::installer {

inline constexpr std::size_t kPayloadFooterSize = 64;
inline constexpr std::uint32_t kPayloadFormat = 1;
inline constexpr std::array<std::uint8_t, 16> kPayloadMagic = {
    'Y', 'A', 'N', 'A', 'M', 'I', '_', 'S',
    'E', 'T', 'U', 'P', '_', 'V', '1', 0,
};

struct PayloadMetadata {
    std::uint64_t payloadOffset = 0;
    std::uint64_t payloadSize = 0;
    std::array<std::uint8_t, 32> payloadSha256{};
    std::uint32_t format = 0;
};

struct PayloadResult {
    bool ok = false;
    PayloadMetadata metadata;
    std::wstring error;
};

bool parsePayloadFooter(
    std::span<const std::uint8_t> footer,
    std::uint64_t fileSize,
    PayloadMetadata& metadata,
    std::wstring& error);

std::array<std::uint8_t, kPayloadFooterSize> serializePayloadFooter(
    std::uint64_t payloadSize,
    const std::array<std::uint8_t, 32>& payloadSha256);

PayloadResult verifyPayloadFile(const std::wstring& sourcePath);
PayloadResult extractVerifiedPayload(
    const std::wstring& sourcePath,
    const std::wstring& destinationPath);

// Verifies the complete contents of an already-open Win32 file handle against
// the extracted payload metadata. The caller owns the handle and must keep it
// open with write/delete sharing denied until CreateProcessW has opened the
// verified backend. The handle's current file position is preserved.
bool verifyExtractedPayloadHandle(
    void* fileHandle,
    const PayloadMetadata& expected,
    std::wstring& error);

std::optional<std::array<std::uint8_t, 32>> sha256Bytes(
    std::span<const std::uint8_t> bytes,
    std::wstring& error);
std::string hexLower(std::span<const std::uint8_t> bytes);

enum class LaunchMode {
    Gui,
    VerifyPayload,
    SilentInstall,
};

struct CommandLineOptions {
    LaunchMode mode = LaunchMode::Gui;
    std::wstring installDirectory;
    bool startMenu = true;
    bool desktop = false;
    bool launchAfterInstall = true;
};

struct CommandLineResult {
    bool ok = false;
    CommandLineOptions options;
    std::wstring error;
};

// Arguments exclude argv[0]. Invalid or incomplete command lines are reported
// to the caller, which must return process exit code 2.
CommandLineResult parseCommandLine(const std::vector<std::wstring>& arguments);

enum class ExistingPathState {
    Missing,
    EmptyDirectory,
    NonEmptyDirectory,
    NotDirectory,
    Inaccessible,
};

struct PathPolicyRoots {
    std::wstring windowsDirectory;
    std::wstring programFiles;
    std::wstring programFilesX86;
};

struct PathValidationResult {
    bool ok = false;
    bool existingInstall = false;
    std::wstring normalizedPath;
    std::wstring error;
};

std::optional<std::wstring> normalizeAbsolutePath(
    const std::wstring& path,
    std::wstring& error);
bool samePath(const std::wstring& left, const std::wstring& right);
bool pathWithin(
    const std::wstring& child,
    const std::wstring& parent,
    bool allowEqual = true);

PathValidationResult validateInstallPathPolicy(
    const std::wstring& candidate,
    const std::optional<std::wstring>& registeredInstall,
    const PathPolicyRoots& roots,
    ExistingPathState existingState);

ExistingPathState inspectExistingPath(const std::wstring& normalizedPath);

// Read-only resolution of the user's selected directory. Empty/missing paths
// are used directly. A non-empty unregistered selection uses an available
// Yanami or Yanami (N) child, never recursively descending through occupied
// children or changing the selected parent. A registered installation is
// repaired in place. normalizedPath is the final destination to show in the UI;
// the worker must revalidate it with the strict policy, not resolve it again.
PathValidationResult resolveInstallDirectory(
    const std::wstring& selected,
    const std::optional<std::wstring>& registeredInstall,
    const PathPolicyRoots& roots);

std::wstring quoteWindowsArgument(const std::wstring& argument);

} // namespace yanami::installer
