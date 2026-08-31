#include <cstdlib>
#include <chrono>
#include <iostream>
#include <thread>
#include <string_view>

namespace {
void delayExitWhenRequested(const char *variable)
{
    if (const char *value = std::getenv(variable);
        value && std::string_view(value) == "1") {
        std::cout << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}
}

int main(int argumentCount, char **argumentValues)
{
    if (argumentCount != 2)
        return 2;
    const std::string_view command(argumentValues[1]);
    if (command == "check") {
        std::cout
            << "{\"event\":\"check\",\"status\":\"available\","
               "\"version\":\"0.2.0-dev.16\",\"delta_count\":1,"
               "\"download_size\":1048576,\"full_size\":8388608}\n";
        delayExitWhenRequested("YANAMI_UPDATE_HELPER_FIXTURE_DELAY_CHECK_EXIT");
        return 0;
    }
    if (command == "download") {
        if (const char *result = std::getenv(
                "YANAMI_UPDATE_HELPER_FIXTURE_DOWNLOAD_RESULT");
            result && std::string_view(result) == "current") {
            std::cout
                << "{\"event\":\"check\",\"status\":\"current\","
                   "\"delta_count\":0,\"download_size\":0,"
                   "\"full_size\":0}\n";
            return 0;
        }
        std::cout << "{\"event\":\"progress\",\"percent\":37}\n";
        std::cout
            << "{\"event\":\"ready\",\"version\":\"0.2.0-dev.16\"}\n";
        delayExitWhenRequested(
            "YANAMI_UPDATE_HELPER_FIXTURE_DELAY_DOWNLOAD_EXIT");
        return 0;
    }
    return 2;
}
