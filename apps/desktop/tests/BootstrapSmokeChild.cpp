#include "BootstrapProtocol.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::uint64_t processId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::filesystem::path readyPath(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (!argument.starts_with(YanamiBootstrap::readyArgumentPrefix))
            continue;
        return std::filesystem::path(argument.substr(
            YanamiBootstrap::readyArgumentPrefix.size()));
    }
    return {};
}

std::string mode(int argc, char *argv[])
{
    constexpr std::string_view prefix = "--bootstrap-child-mode=";
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument.starts_with(prefix))
            return argument.substr(prefix.size());
    }
    return "ready";
}

bool signalReadyEvent(int argc, char *argv[])
{
#ifdef _WIN32
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (!argument.starts_with(
                YanamiBootstrap::readyHandleArgumentPrefix)) {
            continue;
        }
        const std::string_view encoded(
            argument.data() + YanamiBootstrap::readyHandleArgumentPrefix.size(),
            argument.size() - YanamiBootstrap::readyHandleArgumentPrefix.size());
        std::uintptr_t value = 0;
        const auto [end, error] = std::from_chars(
            encoded.data(), encoded.data() + encoded.size(), value);
        if (error != std::errc() || end != encoded.data() + encoded.size()
            || value == 0) {
            return false;
        }
        return SetEvent(reinterpret_cast<HANDLE>(value));
    }
    return false;
#else
    (void)argc;
    (void)argv;
    return true;
#endif
}

bool writeReady(const std::filesystem::path &path, std::uint64_t id)
{
    if (path.empty())
        return false;
    const std::filesystem::path temporary = path.string() + ".tmp";
    const auto writtenUnixMs = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "{\"schemaVersion\":\"1.0\","
                  "\"state\":\"desktop_ready\",\"processId\":"
               << id << ",\"writtenUnixMs\":" << writtenUnixMs << '}';
        if (!output)
            return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

} // namespace

int main(int argc, char *argv[])
{
    const std::string selectedMode = mode(argc, argv);
    if (selectedMode == "timeout") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (selectedMode == "exit19")
        return 19;
    if (selectedMode == "delayed-ready-exit")
        std::this_thread::sleep_for(std::chrono::milliseconds(750));

    const std::uint64_t id = processId();
    const bool fileSpoof = selectedMode == "file-spoof";
    if (!writeReady(
            readyPath(argc, argv), fileSpoof ? id + 1 : id)) {
        return 20;
    }
    if (selectedMode == "missing-event" || fileSpoof) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (!signalReadyEvent(argc, argv))
        return 21;
    if (selectedMode == "ready-exit"
        || selectedMode == "delayed-ready-exit") {
        return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    return 0;
}
