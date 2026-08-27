#include "BootstrapProtocol.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    ++failures;
}

} // namespace

int main()
{
    using namespace YanamiBootstrap;
    const LauncherOptions options = parseLauncherOptions({
        "--yanami-bootstrap-timeout-ms=250",
        "--runtime-smoke-test",
        "--performance-trace",
        "trace.jsonl",
    });
    expect(options.readyTimeout.count() == 250,
           "timeout override must be parsed");
    expect(options.forwardedArguments.size() == 3,
           "launcher-only timeout must not be forwarded");
    expect(options.waitForDesktopExit(),
           "forwarded command arguments must preserve child wait semantics");
    expect(options.performanceTracePath
               == std::optional<std::filesystem::path>("trace.jsonl"),
           "performance trace path must be detected without consuming it");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / "yanami-bootstrap-protocol-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    const std::filesystem::path readyFile = root / "desktop-ready.json";
    {
        std::ofstream output(readyFile);
        output << "{\"schemaVersion\":\"1.0\","
                  "\"state\":\"desktop_ready\",\"processId\":42}";
    }
    std::string error;
    expect(validateReadyFile(readyFile, 42, error),
           "matching ready payload must pass");
    expect(!validateReadyFile(readyFile, 41, error),
           "ready payload from another process must fail closed");
    std::filesystem::remove_all(root, ignored);
    return failures == 0 ? 0 : 1;
}
