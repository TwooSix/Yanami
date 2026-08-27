#include "BootstrapProtocol.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

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

constexpr std::uint64_t minimumTimeoutMs = 100;
constexpr std::uint64_t maximumTimeoutMs = 120'000;
constexpr std::uintmax_t maximumReadyPayloadBytes = 4096;

std::filesystem::path pathFromUtf8(std::string_view value)
{
    const auto *begin = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path(
        std::u8string(begin, begin + value.size()));
}

std::optional<std::string> jsonString(
    std::string_view payload,
    std::string_view key)
{
    const std::string quotedKey = '"' + std::string(key) + '"';
    const std::size_t keyPosition = payload.find(quotedKey);
    if (keyPosition == std::string_view::npos)
        return std::nullopt;
    std::size_t cursor = payload.find(':', keyPosition + quotedKey.size());
    if (cursor == std::string_view::npos)
        return std::nullopt;
    cursor = payload.find('"', cursor + 1);
    if (cursor == std::string_view::npos)
        return std::nullopt;
    const std::size_t end = payload.find('"', cursor + 1);
    if (end == std::string_view::npos)
        return std::nullopt;
    return std::string(payload.substr(cursor + 1, end - cursor - 1));
}

std::optional<std::uint64_t> jsonUnsigned(
    std::string_view payload,
    std::string_view key)
{
    const std::string quotedKey = '"' + std::string(key) + '"';
    const std::size_t keyPosition = payload.find(quotedKey);
    if (keyPosition == std::string_view::npos)
        return std::nullopt;
    std::size_t cursor = payload.find(':', keyPosition + quotedKey.size());
    if (cursor == std::string_view::npos)
        return std::nullopt;
    ++cursor;
    while (cursor < payload.size()
           && (payload[cursor] == ' ' || payload[cursor] == '\t'
               || payload[cursor] == '\r' || payload[cursor] == '\n')) {
        ++cursor;
    }
    const char *begin = payload.data() + cursor;
    const char *end = payload.data() + payload.size();
    std::uint64_t value = 0;
    const auto [parsedEnd, error] = std::from_chars(begin, end, value);
    if (error != std::errc() || parsedEnd == begin)
        return std::nullopt;
    return value;
}

std::string jsonEscape(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string wallClockUtc()
{
    using namespace std::chrono;
    const system_clock::time_point now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    std::tm utc {};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    const auto milliseconds =
        duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
        % 1000;
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
    return result.str();
}

std::uint64_t elapsedCeilMilliseconds(
    std::chrono::steady_clock::time_point started)
{
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
    return static_cast<std::uint64_t>(
        (std::max<std::int64_t>(0, nanoseconds) + 999'999) / 1'000'000);
}

} // namespace

LauncherOptions parseLauncherOptions(const std::vector<std::string> &arguments)
{
    LauncherOptions options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string &argument = arguments[index];
        if (argument.starts_with(timeoutArgumentPrefix)) {
            const std::string_view encoded(argument.data() + timeoutArgumentPrefix.size(),
                                           argument.size() - timeoutArgumentPrefix.size());
            std::uint64_t milliseconds = 0;
            const auto [end, error] = std::from_chars(
                encoded.data(), encoded.data() + encoded.size(), milliseconds);
            if (error == std::errc() && end == encoded.data() + encoded.size()) {
                milliseconds = std::clamp(
                    milliseconds, minimumTimeoutMs, maximumTimeoutMs);
                options.readyTimeout = std::chrono::milliseconds(milliseconds);
            }
            continue;
        }

        options.forwardedArguments.push_back(argument);
        if (argument.starts_with("--performance-trace=")) {
            options.performanceTracePath = pathFromUtf8(
                argument.substr(std::string("--performance-trace=").size()));
        } else if (argument == "--performance-trace"
                   && index + 1 < arguments.size()) {
            options.performanceTracePath = pathFromUtf8(arguments[index + 1]);
        }
    }
    if (!options.performanceTracePath) {
        if (const char *environmentTrace = std::getenv("YANAMI_PERF_TRACE");
            environmentTrace && *environmentTrace) {
            options.performanceTracePath = pathFromUtf8(environmentTrace);
        }
    }
    return options;
}

std::string readyFileArgument(const std::filesystem::path &readyFile)
{
    return std::string(readyArgumentPrefix)
        + readyFile.generic_string();
}

std::string readyHandleArgument(std::uintptr_t nativeHandle)
{
    return std::string(readyHandleArgumentPrefix)
        + std::to_string(nativeHandle);
}

bool validateReadyFile(
    const std::filesystem::path &readyFile,
    std::uint64_t expectedProcessId,
    std::string &error,
    ReadyValidationDiagnostics *diagnostics)
{
#ifdef _WIN32
    ReadyValidationDiagnostics localDiagnostics;
    ReadyValidationDiagnostics &timing = diagnostics
        ? *diagnostics : localDiagnostics;
    WIN32_FILE_ATTRIBUTE_DATA attributes {};
    auto operationStarted = std::chrono::steady_clock::now();
    const BOOL attributesRead = GetFileAttributesExW(
        readyFile.c_str(), GetFileExInfoStandard, &attributes);
    timing.attributesMs = elapsedCeilMilliseconds(operationStarted);
    if (!attributesRead
        || (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "ready path is not a regular file";
        return false;
    }

    operationStarted = std::chrono::steady_clock::now();
    HANDLE file = CreateFileW(
        readyFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    timing.openMs = elapsedCeilMilliseconds(operationStarted);
    if (file == INVALID_HANDLE_VALUE) {
        error = "ready payload could not be opened";
        return false;
    }

    BY_HANDLE_FILE_INFORMATION fileInformation {};
    operationStarted = std::chrono::steady_clock::now();
    const BOOL informationRead = GetFileInformationByHandle(
        file, &fileInformation);
    timing.handleInformationMs = elapsedCeilMilliseconds(operationStarted);
    LARGE_INTEGER fileSize {};
    operationStarted = std::chrono::steady_clock::now();
    const BOOL sizeRead = GetFileSizeEx(file, &fileSize);
    timing.sizeMs = elapsedCeilMilliseconds(operationStarted);
    if (!informationRead
        || (fileInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (fileInformation.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || !sizeRead
        || fileSize.QuadPart <= 0
        || static_cast<std::uint64_t>(fileSize.QuadPart)
            > maximumReadyPayloadBytes) {
        operationStarted = std::chrono::steady_clock::now();
        CloseHandle(file);
        timing.closeMs = elapsedCeilMilliseconds(operationStarted);
        error = "ready payload has an invalid size or file type";
        return false;
    }

    std::string payload(static_cast<std::size_t>(fileSize.QuadPart), '\0');
    DWORD bytesRead = 0;
    operationStarted = std::chrono::steady_clock::now();
    const BOOL read = ReadFile(
        file, payload.data(), static_cast<DWORD>(payload.size()),
        &bytesRead, nullptr);
    timing.readMs = elapsedCeilMilliseconds(operationStarted);
    operationStarted = std::chrono::steady_clock::now();
    CloseHandle(file);
    timing.closeMs = elapsedCeilMilliseconds(operationStarted);
    if (!read || bytesRead != payload.size()) {
        error = "ready payload could not be read";
        return false;
    }
#else
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(readyFile, filesystemError)) {
        error = "ready path is not a regular file";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(
        readyFile, filesystemError);
    if (filesystemError || size == 0 || size > maximumReadyPayloadBytes) {
        error = "ready payload has an invalid size";
        return false;
    }

    std::ifstream input(readyFile, std::ios::binary);
    const std::string payload {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (!input.good() && !input.eof()) {
        error = "ready payload could not be read";
        return false;
    }
#endif
    if (jsonString(payload, "schemaVersion") != std::optional<std::string>("1.0")
        || jsonString(payload, "state")
            != std::optional<std::string>("desktop_ready")) {
        error = "ready payload schema or state is invalid";
        return false;
    }
    const std::optional<std::uint64_t> processId =
        jsonUnsigned(payload, "processId");
    if (!processId || *processId != expectedProcessId) {
        error = "ready payload processId does not match the launched desktop";
        return false;
    }
    return true;
}

TraceWriter::TraceWriter(
    const std::optional<std::filesystem::path> &desktopTracePath,
    std::uint64_t processId)
    : m_processId(processId)
    , m_epoch(std::chrono::steady_clock::now())
{
    if (!desktopTracePath || desktopTracePath->empty())
        return;
    m_path = *desktopTracePath;
    m_path += ".bootstrap.jsonl";
    std::error_code error;
    if (std::filesystem::exists(m_path, error))
        return;
    std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
    if (!output)
        return;
    if (const char *runId = std::getenv("YANAMI_PERF_RUN_ID"); runId && *runId)
        m_runId = runId;
    else
        m_runId = "bootstrap-local";
    m_enabled = true;
}

bool TraceWriter::enabled() const noexcept
{
    return m_enabled;
}

const std::filesystem::path &TraceWriter::path() const noexcept
{
    return m_path;
}

void TraceWriter::mark(
    std::string_view milestone,
    bool readiness,
    std::optional<std::uint64_t> childProcessId,
    std::string_view progressSemantic,
    const MonitorDiagnostics &monitorDiagnostics)
{
    if (!m_enabled || milestone.empty())
        return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - m_epoch).count();
    std::ofstream output(m_path, std::ios::binary | std::ios::app);
    if (!output) {
        m_enabled = false;
        return;
    }
    output << "{\"schemaVersion\":\"1.0\",\"runId\":\""
           << jsonEscape(m_runId)
           << "\",\"suite\":\"startup\",\"scenarioId\":\"desktop.bootstrap\""
           << ",\"milestone\":\"" << jsonEscape(milestone)
           << "\",\"monotonicNs\":" << elapsed
           << ",\"generation\":0,\"processId\":" << m_processId
           << ",\"wallClockUtc\":\"" << wallClockUtc()
           << "\",\"attributes\":{\"readiness\":"
           << (readiness ? "true" : "false");
    if (childProcessId)
        output << ",\"childProcessId\":" << *childProcessId;
    if (!progressSemantic.empty())
        output << ",\"progressSemantic\":\""
               << jsonEscape(progressSemantic) << '"';
    if (monitorDiagnostics.maxDispatchGapMs)
        output << ",\"monitorMaxDispatchGapMs\":"
               << *monitorDiagnostics.maxDispatchGapMs;
    if (monitorDiagnostics.maxLoopGapMs)
        output << ",\"monitorMaxLoopGapMs\":"
               << *monitorDiagnostics.maxLoopGapMs
               << ",\"maxFullLoopGapMs\":"
               << *monitorDiagnostics.maxLoopGapMs;
    if (monitorDiagnostics.maxExistsCallMs)
        output << ",\"maxExistsCallMs\":"
               << *monitorDiagnostics.maxExistsCallMs
               << ",\"maxReadyProbeMs\":"
               << *monitorDiagnostics.maxExistsCallMs;
    if (monitorDiagnostics.readyFirstSeenMs)
        output << ",\"readyFirstSeenMs\":"
               << *monitorDiagnostics.readyFirstSeenMs;
    if (monitorDiagnostics.validateMs)
        output << ",\"validateMs\":"
               << *monitorDiagnostics.validateMs;
    if (monitorDiagnostics.validationAttributesMs)
        output << ",\"validationAttributesMs\":"
               << *monitorDiagnostics.validationAttributesMs;
    if (monitorDiagnostics.validationOpenMs)
        output << ",\"validationOpenMs\":"
               << *monitorDiagnostics.validationOpenMs;
    if (monitorDiagnostics.validationHandleInformationMs)
        output << ",\"validationHandleInformationMs\":"
               << *monitorDiagnostics.validationHandleInformationMs;
    if (monitorDiagnostics.validationSizeMs)
        output << ",\"validationSizeMs\":"
               << *monitorDiagnostics.validationSizeMs;
    if (monitorDiagnostics.validationReadMs)
        output << ",\"validationReadMs\":"
               << *monitorDiagnostics.validationReadMs;
    if (monitorDiagnostics.validationCloseMs)
        output << ",\"validationCloseMs\":"
               << *monitorDiagnostics.validationCloseMs;
    output << "}}\n";
    output.flush();
}

} // namespace YanamiBootstrap
