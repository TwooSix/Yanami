#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Minimal ABI surface from Velopack 1.2.0. Keeping the helper behind the C ABI
// lets Yanami's MinGW build load the official MSVC runtime DLL safely without
// linking incompatible C++ runtimes.
using UpdateSource = void;
using UpdateManager = void;

enum class UpdateCheck : std::int8_t {
    Error = -1,
    Available = 0,
    Current = 1,
    Empty = 2,
};

struct Asset {
    char *packageId;
    char *version;
    char *type;
    char *fileName;
    char *sha1;
    char *sha256;
    std::uint64_t size;
    char *notesMarkdown;
    char *notesHtml;
};

struct UpdateInfo {
    Asset *targetFullRelease;
    Asset *baseRelease;
    Asset **deltasToTarget;
    std::size_t deltasToTargetCount;
    bool isDowngrade;
};

using ProgressCallback = void (*)(void *, std::size_t);
using NewGithubSource = UpdateSource *(*)(const char *, const char *, bool);
using FreeSource = void (*)(UpdateSource *);
using NewManager = bool (*)(UpdateSource *, void *, void *, UpdateManager **);
using FreeManager = void (*)(UpdateManager *);
using CheckForUpdates = UpdateCheck (*)(UpdateManager *, UpdateInfo **);
using FreeUpdateInfo = void (*)(UpdateInfo *);
using DownloadUpdates = bool (*)(
    UpdateManager *, UpdateInfo *, ProgressCallback, void *);
using UpdatePendingRestart = bool (*)(UpdateManager *, Asset **);
using UnsafeApplyUpdates = bool (*)(
    UpdateManager *, Asset *, bool, std::uint32_t, bool, char **, std::size_t);
using FreeAsset = void (*)(Asset *);
using GetVelopackError = std::size_t (*)(char *, std::size_t);
using AppSetAutoApply = void (*)(bool);
using AppSetArgs = void (*)(char **, std::size_t);
using AppRun = void (*)(void *);

constexpr char repositoryUrl[] = "https://github.com/TwooSix/Yanami";
constexpr wchar_t runtimeName[] = L"velopack_libc.dll";

std::mutex outputMutex;

std::string jsonEscape(std::string_view value)
{
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                escaped << "\\u00" << hex[character >> 4]
                        << hex[character & 0x0f];
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    return escaped.str();
}

void emitJson(const std::string &json)
{
    const std::scoped_lock lock(outputMutex);
    std::cout << json << '\n' << std::flush;
}

void emitError(std::string_view kind, std::string_view message)
{
    emitJson(
        "{\"event\":\"error\",\"kind\":\"" + jsonEscape(kind)
        + "\",\"message\":\"" + jsonEscape(message) + "\"}");
}

std::string utf8(const std::wstring &value)
{
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required,
            nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

std::filesystem::path executableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

HANDLE isolateDownloadProcessTree(std::string &error)
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        error = "Could not create the updater process job (Windows error "
            + std::to_string(::GetLastError()) + ").";
        return nullptr;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        error = "Could not configure the updater process job (Windows error "
            + std::to_string(::GetLastError()) + ").";
        CloseHandle(job);
        return nullptr;
    }
    if (!AssignProcessToJobObject(job, GetCurrentProcess())) {
        error = "Could not isolate the updater process tree (Windows error "
            + std::to_string(::GetLastError()) + ").";
        CloseHandle(job);
        return nullptr;
    }

    // Intentionally keep the handle open for the lifetime of this process.
    // If Yanami cancels or exits while Velopack's patch worker is active,
    // Windows closes this handle and terminates the entire inherited tree.
    return job;
}

class Runtime final {
public:
    Runtime()
    {
        const std::filesystem::path runtimePath =
            executableDirectory() / runtimeName;
        m_module = LoadLibraryExW(
            runtimePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module) {
            m_loadError = "Velopack runtime is unavailable (Windows error "
                + std::to_string(::GetLastError()) + ").";
            return;
        }

        newGithubSource = resolve<NewGithubSource>("vpkc_new_source_github");
        freeSource = resolve<FreeSource>("vpkc_free_source");
        newManager = resolve<NewManager>("vpkc_new_update_manager_with_source");
        freeManager = resolve<FreeManager>("vpkc_free_update_manager");
        checkForUpdates = resolve<CheckForUpdates>("vpkc_check_for_updates");
        freeUpdateInfo = resolve<FreeUpdateInfo>("vpkc_free_update_info");
        downloadUpdates = resolve<DownloadUpdates>("vpkc_download_updates");
        updatePendingRestart = resolve<UpdatePendingRestart>(
            "vpkc_update_pending_restart");
        unsafeApplyUpdates = resolve<UnsafeApplyUpdates>(
            "vpkc_unsafe_apply_updates");
        freeAsset = resolve<FreeAsset>("vpkc_free_asset");
        getLastError = resolve<GetVelopackError>("vpkc_get_last_error");
        appSetAutoApply = resolve<AppSetAutoApply>(
            "vpkc_app_set_auto_apply_on_startup");
        appSetArgs = resolve<AppSetArgs>("vpkc_app_set_args");
        appRun = resolve<AppRun>("vpkc_app_run");

        if (!newGithubSource || !freeSource || !newManager || !freeManager
            || !checkForUpdates || !freeUpdateInfo || !downloadUpdates
            || !updatePendingRestart || !unsafeApplyUpdates || !freeAsset
            || !getLastError || !appSetAutoApply || !appSetArgs || !appRun) {
            m_loadError = "Velopack runtime does not expose the required 1.2.0 ABI.";
        }
    }

    ~Runtime()
    {
        if (m_module)
            FreeLibrary(m_module);
    }

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    bool valid() const { return m_loadError.empty(); }
    const std::string &loadError() const { return m_loadError; }

    std::string lastError() const
    {
        if (!getLastError)
            return "Velopack operation failed.";
        const std::size_t required = getLastError(nullptr, 0);
        if (required <= 1)
            return "Velopack operation failed.";
        std::vector<char> buffer(required, '\0');
        getLastError(buffer.data(), buffer.size());
        return std::string(buffer.data());
    }

    NewGithubSource newGithubSource = nullptr;
    FreeSource freeSource = nullptr;
    NewManager newManager = nullptr;
    FreeManager freeManager = nullptr;
    CheckForUpdates checkForUpdates = nullptr;
    FreeUpdateInfo freeUpdateInfo = nullptr;
    DownloadUpdates downloadUpdates = nullptr;
    UpdatePendingRestart updatePendingRestart = nullptr;
    UnsafeApplyUpdates unsafeApplyUpdates = nullptr;
    FreeAsset freeAsset = nullptr;
    GetVelopackError getLastError = nullptr;
    AppSetAutoApply appSetAutoApply = nullptr;
    AppSetArgs appSetArgs = nullptr;
    AppRun appRun = nullptr;

private:
    template<typename Function>
    Function resolve(const char *name)
    {
        return reinterpret_cast<Function>(GetProcAddress(m_module, name));
    }

    HMODULE m_module = nullptr;
    std::string m_loadError;
};

struct ManagerSession {
    Runtime &runtime;
    UpdateSource *source = nullptr;
    UpdateManager *manager = nullptr;

    explicit ManagerSession(Runtime &runtimeValue)
        : runtime(runtimeValue)
    {
    }

    ManagerSession(const ManagerSession &) = delete;
    ManagerSession &operator=(const ManagerSession &) = delete;

    ManagerSession(ManagerSession &&other) noexcept
        : runtime(other.runtime)
        , source(std::exchange(other.source, nullptr))
        , manager(std::exchange(other.manager, nullptr))
    {
    }

    ManagerSession &operator=(ManagerSession &&) = delete;

    ~ManagerSession()
    {
        if (manager)
            runtime.freeManager(manager);
        if (source)
            runtime.freeSource(source);
    }
};

std::optional<ManagerSession> createManager(Runtime &runtime)
{
    ManagerSession session(runtime);
    session.source = runtime.newGithubSource(repositoryUrl, nullptr, true);
    if (!session.source)
        return std::nullopt;
    if (!runtime.newManager(session.source, nullptr, nullptr, &session.manager))
        return std::nullopt;
    return session;
}

std::uint64_t selectedDownloadSize(const UpdateInfo &update)
{
    if (update.deltasToTargetCount == 0 || !update.deltasToTarget)
        return update.targetFullRelease ? update.targetFullRelease->size : 0;
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < update.deltasToTargetCount; ++index) {
        if (update.deltasToTarget[index])
            total += update.deltasToTarget[index]->size;
    }
    return total;
}

void emitCheck(UpdateCheck result, const UpdateInfo *update)
{
    if (result == UpdateCheck::Available && update
        && update->targetFullRelease) {
        const Asset &target = *update->targetFullRelease;
        emitJson(
            "{\"event\":\"check\",\"status\":\"available\","
            "\"version\":\"" + jsonEscape(target.version ? target.version : "")
            + "\",\"delta_count\":"
            + std::to_string(update->deltasToTargetCount)
            + ",\"download_size\":"
            + std::to_string(selectedDownloadSize(*update))
            + ",\"full_size\":" + std::to_string(target.size) + "}");
        return;
    }
    const char *status = result == UpdateCheck::Empty ? "empty" : "current";
    emitJson(
        std::string("{\"event\":\"check\",\"status\":\"") + status
        + "\",\"delta_count\":0,\"download_size\":0,\"full_size\":0}");
}

int runStartup(Runtime &runtime, const std::vector<std::string> &arguments)
{
    std::vector<std::string> mutableArguments = arguments;
    std::vector<char *> argumentPointers;
    argumentPointers.reserve(mutableArguments.size());
    for (std::string &argument : mutableArguments)
        argumentPointers.push_back(argument.data());
    runtime.appSetAutoApply(false);
    runtime.appSetArgs(argumentPointers.data(), argumentPointers.size());
    runtime.appRun(nullptr);
    emitJson("{\"event\":\"startup_ready\"}");
    return 0;
}

int runCheck(Runtime &runtime)
{
    auto session = createManager(runtime);
    if (!session) {
        emitError("not_installed", runtime.lastError());
        return 3;
    }
    Asset *pending = nullptr;
    if (runtime.updatePendingRestart(session->manager, &pending) && pending) {
        const std::string version = pending->version ? pending->version : "";
        emitJson(
            "{\"event\":\"check\",\"status\":\"ready\",\"version\":\""
            + jsonEscape(version)
            + "\",\"delta_count\":0,\"download_size\":0,\"full_size\":0}");
        runtime.freeAsset(pending);
        return 0;
    }
    UpdateInfo *update = nullptr;
    const UpdateCheck result = runtime.checkForUpdates(session->manager, &update);
    if (result == UpdateCheck::Error) {
        emitError("check_failed", runtime.lastError());
        return 1;
    }
    emitCheck(result, update);
    if (update)
        runtime.freeUpdateInfo(update);
    return 0;
}

void reportProgress(void *, std::size_t progress)
{
    emitJson(
        "{\"event\":\"progress\",\"percent\":"
        + std::to_string(std::min<std::size_t>(100, progress)) + "}");
}

int runDownload(Runtime &runtime)
{
    std::string isolationError;
    const HANDLE processTreeJob = isolateDownloadProcessTree(isolationError);
    if (!processTreeJob) {
        emitError("process_isolation_failed", isolationError);
        return 6;
    }
    (void)processTreeJob;

    auto session = createManager(runtime);
    if (!session) {
        emitError("not_installed", runtime.lastError());
        return 3;
    }
    UpdateInfo *update = nullptr;
    const UpdateCheck result = runtime.checkForUpdates(session->manager, &update);
    if (result == UpdateCheck::Error) {
        emitError("check_failed", runtime.lastError());
        return 1;
    }
    if (result != UpdateCheck::Available || !update
        || !update->targetFullRelease) {
        emitCheck(result, update);
        if (update)
            runtime.freeUpdateInfo(update);
        return 0;
    }

    const std::string version = update->targetFullRelease->version
        ? update->targetFullRelease->version : "";
    const bool downloaded = runtime.downloadUpdates(
        session->manager, update, &reportProgress, nullptr);
    runtime.freeUpdateInfo(update);
    if (!downloaded) {
        emitError("download_failed", runtime.lastError());
        return 1;
    }
    emitJson(
        "{\"event\":\"ready\",\"version\":\""
        + jsonEscape(version) + "\"}");
    return 0;
}

int runApply(Runtime &runtime, std::uint32_t waitPid)
{
    auto session = createManager(runtime);
    if (!session) {
        emitError("not_installed", runtime.lastError());
        return 3;
    }
    Asset *pending = nullptr;
    if (!runtime.updatePendingRestart(session->manager, &pending) || !pending) {
        emitError("no_pending_update", "No downloaded update is ready to apply.");
        return 4;
    }
    const std::string version = pending->version ? pending->version : "";
    const bool handedOff = runtime.unsafeApplyUpdates(
        session->manager, pending, true, waitPid, true, nullptr, 0);
    runtime.freeAsset(pending);
    if (!handedOff) {
        emitError("apply_failed", runtime.lastError());
        return 1;
    }
    emitJson(
        "{\"event\":\"handed_off\",\"version\":\""
        + jsonEscape(version) + "\"}");
    return 0;
}

std::optional<std::uint32_t> parsePositiveProcessId(const std::wstring &value)
{
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed, 10);
        if (consumed != value.size() || parsed == 0
            || parsed > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

int wmain(int argumentCount, wchar_t **argumentValues)
{
    if (argumentCount < 2) {
        emitError("invalid_arguments", "Missing updater command.");
        return 2;
    }

    Runtime runtime;
    if (!runtime.valid()) {
        emitError("runtime_unavailable", runtime.loadError());
        return 5;
    }

    const std::wstring command(argumentValues[1]);
    if (command == L"startup") {
        std::vector<std::string> forwarded;
        for (int index = 2; index < argumentCount; ++index)
            forwarded.push_back(utf8(argumentValues[index]));
        return runStartup(runtime, forwarded);
    }
    if (command == L"check" && argumentCount == 2)
        return runCheck(runtime);
    if (command == L"download" && argumentCount == 2)
        return runDownload(runtime);
    if (command == L"apply" && argumentCount == 4
        && std::wstring_view(argumentValues[2]) == L"--wait-pid") {
        const auto processId = parsePositiveProcessId(argumentValues[3]);
        if (processId)
            return runApply(runtime, *processId);
    }

    emitError("invalid_arguments", "Invalid updater command or arguments.");
    return 2;
}
