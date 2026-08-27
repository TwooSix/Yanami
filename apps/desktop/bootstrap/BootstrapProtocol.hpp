#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace YanamiBootstrap {

inline constexpr std::string_view readyArgumentPrefix =
    "--yanami-bootstrap-ready-file=";
inline constexpr std::string_view timeoutArgumentPrefix =
    "--yanami-bootstrap-timeout-ms=";
inline constexpr std::string_view readyHandleArgumentPrefix =
    "--yanami-bootstrap-ready-handle=";
inline constexpr std::chrono::milliseconds defaultReadyTimeout {45'000};

enum class ExitCode : int {
    Success = 0,
    ReadyTimeout = 70,
    DesktopFailed = 71,
    InvalidReadyPayload = 72,
    Cancelled = 73,
};

struct LauncherOptions {
    std::chrono::milliseconds readyTimeout = defaultReadyTimeout;
    std::vector<std::string> forwardedArguments;
    std::optional<std::filesystem::path> performanceTracePath;

    [[nodiscard]] bool waitForDesktopExit() const noexcept
    {
        return !forwardedArguments.empty();
    }
};

struct ReadyValidationDiagnostics {
    std::uint64_t attributesMs = 0;
    std::uint64_t openMs = 0;
    std::uint64_t handleInformationMs = 0;
    std::uint64_t sizeMs = 0;
    std::uint64_t readMs = 0;
    std::uint64_t closeMs = 0;
};

LauncherOptions parseLauncherOptions(const std::vector<std::string> &arguments);

[[nodiscard]] std::string readyFileArgument(
    const std::filesystem::path &readyFile);

[[nodiscard]] std::string readyHandleArgument(std::uintptr_t nativeHandle);

[[nodiscard]] bool validateReadyFile(
    const std::filesystem::path &readyFile,
    std::uint64_t expectedProcessId,
    std::string &error,
    ReadyValidationDiagnostics *diagnostics = nullptr);

class TraceWriter final
{
public:
    struct MonitorDiagnostics {
        std::optional<std::uint64_t> maxDispatchGapMs;
        std::optional<std::uint64_t> maxLoopGapMs;
        std::optional<std::uint64_t> maxExistsCallMs;
        std::optional<std::uint64_t> readyFirstSeenMs;
        std::optional<std::uint64_t> validateMs;
        std::optional<std::uint64_t> validationAttributesMs;
        std::optional<std::uint64_t> validationOpenMs;
        std::optional<std::uint64_t> validationHandleInformationMs;
        std::optional<std::uint64_t> validationSizeMs;
        std::optional<std::uint64_t> validationReadMs;
        std::optional<std::uint64_t> validationCloseMs;
    };

    TraceWriter(
        const std::optional<std::filesystem::path> &desktopTracePath,
        std::uint64_t processId);

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] const std::filesystem::path &path() const noexcept;

    void mark(
        std::string_view milestone,
        bool readiness,
        std::optional<std::uint64_t> childProcessId = std::nullopt,
        std::string_view progressSemantic = {},
        const MonitorDiagnostics &monitorDiagnostics = {});

private:
    std::filesystem::path m_path;
    std::uint64_t m_processId = 0;
    std::string m_runId;
    std::chrono::steady_clock::time_point m_epoch;
    bool m_enabled = false;
};

} // namespace YanamiBootstrap
