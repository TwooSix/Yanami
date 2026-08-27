#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <sddl.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "BootstrapProtocol.hpp"
#include "BootstrapLocale.hpp"
#include "BootstrapWindowsAnimation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr wchar_t windowClassName[] = L"YanamiBootstrapWindow.v1";
constexpr wchar_t instanceMutexName[] = L"Local\\Yanami.Desktop.Instance.v1";
constexpr wchar_t desktopExecutableName[] = L"yanami-desktop.exe";
constexpr wchar_t readyFileName[] = L"desktop-ready.json";
constexpr UINT_PTR animationTimerId = 1;
constexpr int cancelButtonId = 1001;
constexpr auto cancelRevealDelay = 1800ms;
constexpr auto handoffDuration = 180ms;
constexpr COLORREF spinnerColor = RGB(91, 149, 255);

struct LauncherText {
    std::wstring windowTitle = L"Yanami is starting";
    std::wstring cancel = L"Cancel";
    std::wstring preparing = L"Preparing Yanami\u2026";
    std::wstring starting = L"Starting Yanami\u2026";
    std::wstring cancelling = L"Cancelling safely\u2026";
    std::wstring continuing = L"Continuing to start Yanami\u2026";
    std::wstring takingLong = L"Yanami is taking longer than expected\u2026";
    std::wstring retrying = L"Retrying Yanami\u2026";
    std::wstring missingMessage =
        L"Yanami Desktop is missing from this application folder.\n\n"
        L"Reinstall Yanami and try again.";
    std::wstring missingTitle = L"Yanami installation is incomplete";
    std::wstring instanceError =
        L"Yanami could not establish single-instance ownership.";
    std::wstring startupErrorTitle = L"Yanami startup error";
    std::wstring couldNotStartTitle = L"Yanami could not start";
    std::wstring readyDirectoryError =
        L"Yanami could not create a private startup handoff directory.";
    std::wstring processAttributesError =
        L"Yanami could not prepare the desktop process attributes.";
    std::wstring instanceHandoffError =
        L"Yanami could not prepare its single-instance handoff.";
    std::wstring desktopStartError =
        L"Yanami Desktop could not be started.\n\n";
    std::wstring forceStopMessage =
        L"Yanami is still starting and could not close gracefully.\n\n"
        L"Force stop the startup process? No user library data will be removed.";
    std::wstring forceStopTitle = L"Cancel Yanami startup";
    std::wstring invalidHandoffPrefix =
        L"Yanami received an invalid startup handoff.\n\n";
    std::wstring invalidHandoffSuffix =
        L"\n\nPlease retry or reinstall the application.";
    std::wstring exitedBeforeReady =
        L"Yanami Desktop exited before its first window was ready.\n\nExit code: ";
    std::wstring retryOrReinstall =
        L"\n\nRetry the launch. If this continues, reinstall Yanami.";
    std::wstring timeoutMessage =
        L"Yanami is still starting.\n\n"
        L"Retry keeps waiting. Cancel safely stops the startup process.";
    std::wstring timeoutTitle =
        L"Yanami startup is taking longer than expected";
};

LauncherText localizedText(YanamiBootstrap::UiLanguage language)
{
    LauncherText text;
    if (language != YanamiBootstrap::UiLanguage::SimplifiedChinese)
        return text;
    text.windowTitle = L"Yanami 正在启动";
    text.cancel = L"取消";
    text.preparing = L"正在准备 Yanami\u2026";
    text.starting = L"正在启动 Yanami\u2026";
    text.cancelling = L"正在安全取消\u2026";
    text.continuing = L"继续启动 Yanami\u2026";
    text.takingLong = L"Yanami 启动时间比预期更长\u2026";
    text.retrying = L"正在重试启动 Yanami\u2026";
    text.missingMessage =
        L"此应用目录中缺少 Yanami Desktop。\n\n请重新安装 Yanami 后再试。";
    text.missingTitle = L"Yanami 安装不完整";
    text.instanceError = L"Yanami 无法建立单实例所有权。";
    text.startupErrorTitle = L"Yanami 启动错误";
    text.couldNotStartTitle = L"Yanami 无法启动";
    text.readyDirectoryError = L"Yanami 无法创建私有启动交接目录。";
    text.processAttributesError = L"Yanami 无法准备桌面进程属性。";
    text.instanceHandoffError = L"Yanami 无法准备单实例交接。";
    text.desktopStartError = L"无法启动 Yanami Desktop。\n\n";
    text.forceStopMessage =
        L"Yanami 仍在启动，无法正常关闭。\n\n是否强制停止启动进程？"
        L"不会删除媒体库中的任何数据。";
    text.forceStopTitle = L"取消 Yanami 启动";
    text.invalidHandoffPrefix = L"Yanami 收到了无效的启动交接数据。\n\n";
    text.invalidHandoffSuffix =
        L"\n\n请重试；若问题持续，请重新安装应用。";
    text.exitedBeforeReady =
        L"Yanami Desktop 在首个窗口就绪前已退出。\n\n退出码：";
    text.retryOrReinstall =
        L"\n\n请重试启动；若问题持续，请重新安装 Yanami。";
    text.timeoutMessage =
        L"Yanami 仍在启动。\n\n选择“重试”将继续等待；选择“取消”将安全停止启动进程。";
    text.timeoutTitle = L"Yanami 启动时间比预期更长";
    return text;
}

struct SplashState {
    HWND window = nullptr;
    HWND cancelButton = nullptr;
    HICON icon = nullptr;
    HICON brandIcon = nullptr;
    int brandIconSize = 0;
    HBRUSH backgroundBrush = nullptr;
    HPEN borderPen = nullptr;
    HPEN iconBorderPen = nullptr;
    HPEN ringPen = nullptr;
    HPEN spinnerMaskPen = nullptr;
    HBRUSH spinnerMaskBrush = nullptr;
    HFONT titleFont = nullptr;
    HFONT statusFont = nullptr;
    HFONT buttonFont = nullptr;
    HDC staticBuffer = nullptr;
    HBITMAP staticBufferBitmap = nullptr;
    HGDIOBJ previousStaticBufferBitmap = nullptr;
    std::uint32_t *staticBufferPixels = nullptr;
    HDC frameBuffer = nullptr;
    HBITMAP frameBufferBitmap = nullptr;
    HGDIOBJ previousFrameBufferBitmap = nullptr;
    std::uint32_t *frameBufferPixels = nullptr;
    HDC spinnerMaskBuffer = nullptr;
    HBITMAP spinnerMaskBitmap = nullptr;
    HGDIOBJ previousSpinnerMaskBitmap = nullptr;
    std::uint32_t *spinnerMaskPixels = nullptr;
    YanamiBootstrap::SplashMetrics metrics =
        YanamiBootstrap::splashMetricsForDpi(YanamiBootstrap::baselineDpi);
    LauncherText text;
    std::wstring status;
    std::chrono::steady_clock::time_point animationEpoch =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point fadeEpoch {};
    bool cancelRequested = false;
    bool fading = false;
    bool fadeComplete = false;
    bool cancelButtonShown = false;
    bool staticFrameDirty = true;
    std::uint64_t renderedSpinnerFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t lastPresentedSpinnerFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t spinnerPaintCount = 0;

    explicit SplashState(LauncherText localized)
        : text(std::move(localized)), status(text.starting)
    {
    }
};

void initializeDrawingResources(SplashState &state)
{
    // Create font and GDI resources before the Qt child starts. Recreating
    // fonts inside each animation paint can serialize with the child's cold
    // DirectWrite/font initialization and stall ready-file polling.
    state.backgroundBrush = CreateSolidBrush(RGB(8, 13, 23));
    state.borderPen = CreatePen(
        PS_SOLID, state.metrics.borderThickness, RGB(27, 36, 50));
    state.iconBorderPen = CreatePen(
        PS_SOLID, state.metrics.borderThickness, RGB(27, 37, 52));
    state.ringPen = CreatePen(
        PS_SOLID, state.metrics.spinnerThickness, spinnerColor);
    state.spinnerMaskPen = CreatePen(
        PS_SOLID,
        state.metrics.spinnerThickness * YanamiBootstrap::spinnerSupersample,
        RGB(255, 255, 255));
    state.spinnerMaskBrush = CreateSolidBrush(RGB(255, 255, 255));
    state.titleFont = CreateFontW(
        -state.metrics.titleFontHeight, 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    state.statusFont = CreateFontW(
        -state.metrics.statusFontHeight, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    state.buttonFont = CreateFontW(
        -state.metrics.buttonFontHeight, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void releaseDrawingBuffer(
    HDC &buffer, HBITMAP &bitmap, HGDIOBJ &previousBitmap,
    std::uint32_t *&pixels)
{
    if (!buffer)
        return;
    if (previousBitmap)
        SelectObject(buffer, previousBitmap);
    if (bitmap)
        DeleteObject(bitmap);
    DeleteDC(buffer);
    buffer = nullptr;
    bitmap = nullptr;
    previousBitmap = nullptr;
    pixels = nullptr;
}

void releaseDrawingResources(SplashState &state)
{
    releaseDrawingBuffer(
        state.frameBuffer, state.frameBufferBitmap,
        state.previousFrameBufferBitmap, state.frameBufferPixels);
    releaseDrawingBuffer(
        state.staticBuffer, state.staticBufferBitmap,
        state.previousStaticBufferBitmap, state.staticBufferPixels);
    releaseDrawingBuffer(
        state.spinnerMaskBuffer, state.spinnerMaskBitmap,
        state.previousSpinnerMaskBitmap, state.spinnerMaskPixels);
    if (state.buttonFont)
        DeleteObject(state.buttonFont);
    if (state.statusFont)
        DeleteObject(state.statusFont);
    if (state.titleFont)
        DeleteObject(state.titleFont);
    if (state.ringPen)
        DeleteObject(state.ringPen);
    if (state.spinnerMaskPen)
        DeleteObject(state.spinnerMaskPen);
    if (state.spinnerMaskBrush)
        DeleteObject(state.spinnerMaskBrush);
    if (state.iconBorderPen)
        DeleteObject(state.iconBorderPen);
    if (state.borderPen)
        DeleteObject(state.borderPen);
    if (state.backgroundBrush)
        DeleteObject(state.backgroundBrush);
}

std::wstring executableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    const std::size_t separator = buffer.find_last_of(L"\\/");
    return separator == std::wstring::npos
        ? std::wstring() : buffer.substr(0, separator);
}

std::string utf8(const std::wstring &value)
{
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value)
{
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring quoteCommandLineArgument(const std::wstring &argument)
{
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring formatWindowsError(DWORD error)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    std::wstring result = length && message
        ? std::wstring(message, length) : L"Unknown Windows error";
    if (message)
        LocalFree(message);
    while (!result.empty()
           && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

std::optional<std::filesystem::path> createPrivateReadyDirectory()
{
    std::wstring temporaryRoot(32768, L'\0');
    const DWORD rootLength = GetTempPathW(
        static_cast<DWORD>(temporaryRoot.size()), temporaryRoot.data());
    if (rootLength == 0 || rootLength >= temporaryRoot.size())
        return std::nullopt;
    temporaryRoot.resize(rootLength);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;OW)(A;;FA;;;SY)",
            SDDL_REVISION_1, &descriptor, nullptr)) {
        return std::nullopt;
    }
    SECURITY_ATTRIBUTES securityAttributes {
        sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};

    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    const DWORD processId = GetCurrentProcessId();
    for (unsigned int attempt = 0; attempt < 16; ++attempt) {
        const std::wstring directoryName =
            L"YanamiBootstrap-" + std::to_wstring(processId) + L"-"
            + std::to_wstring(
                static_cast<unsigned long long>(counter.QuadPart) + attempt);
        const std::filesystem::path directory =
            std::filesystem::path(temporaryRoot) / directoryName;
        if (CreateDirectoryW(directory.c_str(), &securityAttributes)) {
            LocalFree(descriptor);
            return directory;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    LocalFree(descriptor);
    return std::nullopt;
}

void cleanupReadyDirectory(const std::filesystem::path &directory)
{
    std::error_code ignored;
    std::filesystem::remove(directory / readyFileName, ignored);
    std::filesystem::remove(directory, ignored);
}

struct WindowSearch {
    DWORD processId = 0;
    HWND result = nullptr;
};

BOOL CALLBACK findWindowForProcess(HWND window, LPARAM parameter)
{
    auto *search = reinterpret_cast<WindowSearch *>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || !IsWindowVisible(window)
        || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT bounds {};
    if (!GetWindowRect(window, &bounds)
        || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return TRUE;
    }
    search->result = window;
    return FALSE;
}

HWND topLevelWindowForProcess(DWORD processId)
{
    WindowSearch search {processId, nullptr};
    EnumWindows(&findWindowForProcess, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

void activateWindow(HWND window)
{
    if (!window)
        return;
    if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);
    else
        ShowWindow(window, SW_SHOW);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
}

bool imagePathMatches(DWORD processId, const std::filesystem::path &expected)
{
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return false;
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL queried = QueryFullProcessImageNameW(
        process, 0, path.data(), &length);
    CloseHandle(process);
    if (!queried)
        return false;
    path.resize(length);
    return _wcsicmp(path.c_str(), expected.c_str()) == 0;
}

HWND findExistingDesktopWindow(const std::filesystem::path &desktopPath)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;
    PROCESSENTRY32W entry {sizeof(PROCESSENTRY32W)};
    HWND result = nullptr;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, desktopExecutableName) != 0
                || !imagePathMatches(entry.th32ProcessID, desktopPath)) {
                continue;
            }
            result = topLevelWindowForProcess(entry.th32ProcessID);
            if (result)
                break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

RECT spinnerBounds(const SplashState &state)
{
    const auto &metrics = state.metrics;
    const int spinnerX = (metrics.width - metrics.spinnerSize) / 2;
    return {
        spinnerX,
        metrics.spinnerTop,
        spinnerX + metrics.spinnerSize,
        metrics.spinnerTop + metrics.spinnerSize,
    };
}

RECT spinnerPaintBounds(const SplashState &state)
{
    RECT bounds = spinnerBounds(state);
    InflateRect(
        &bounds, state.metrics.spinnerPaintPadding,
        state.metrics.spinnerPaintPadding);
    return bounds;
}

void drawStaticSplash(SplashState &state, HDC device)
{
    const auto &metrics = state.metrics;
    RECT client {0, 0, metrics.width, metrics.height};
    FillRect(device, &client, state.backgroundBrush);

    HGDIOBJ oldPen = SelectObject(device, state.borderPen);
    HGDIOBJ oldBrush = SelectObject(device, GetStockObject(NULL_BRUSH));
    RoundRect(
        device, 0, 0, client.right - 1, client.bottom - 1,
        metrics.cornerRadius, metrics.cornerRadius);

    const HICON brandIcon = state.brandIcon ? state.brandIcon : state.icon;
    if (brandIcon) {
        const bool exactSplashFrame = state.brandIcon
            && state.brandIconSize > 0;
        const int drawSize = exactSplashFrame
            ? state.brandIconSize : metrics.iconSize;
        const int iconX = (client.right - drawSize) / 2;
        const int iconY = metrics.iconY
            + (metrics.iconSize - drawSize) / 2;
        DrawIconEx(
            device, iconX, iconY, brandIcon,
            exactSplashFrame ? 0 : drawSize,
            exactSplashFrame ? 0 : drawSize,
            0, nullptr, DI_NORMAL);

        // The source app icon has an intentionally dark rounded tile. A quiet
        // one-pixel keyline makes that tile read as deliberate structure
        // instead of a vague block against the nearly identical background.
        if (state.iconBorderPen) {
            HGDIOBJ oldIconPen = SelectObject(device, state.iconBorderPen);
            HGDIOBJ oldIconBrush = SelectObject(
                device, GetStockObject(NULL_BRUSH));
            const int cornerDiameter = std::max(
                2, metrics.iconCornerDiameter * drawSize
                    / std::max(1, metrics.iconSize));
            RoundRect(
                device, iconX, iconY, iconX + drawSize, iconY + drawSize,
                cornerDiameter, cornerDiameter);
            SelectObject(device, oldIconBrush);
            SelectObject(device, oldIconPen);
        }
    }

    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, RGB(245, 247, 250));
    HGDIOBJ oldFont = SelectObject(device, state.titleFont);
    RECT titleBounds {
        0, metrics.titleTop, client.right, metrics.titleBottom};
    DrawTextW(device, L"Yanami", -1, &titleBounds,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(device, RGB(154, 163, 178));
    SelectObject(device, state.statusFont);
    RECT statusBounds {
        metrics.horizontalTextMargin, metrics.statusTop,
        client.right - metrics.horizontalTextMargin, metrics.statusBottom};
    DrawTextW(device, state.status.c_str(), -1, &statusBounds,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(device, oldFont);

    SelectObject(device, oldBrush);
    SelectObject(device, oldPen);
}

void drawSpinnerArc(
    SplashState &state, HDC device, int startDegrees)
{
    const RECT ringBounds = spinnerBounds(state);
    const double startRadians = startDegrees * 3.14159265358979323846 / 180.0;
    const double endRadians =
        (startDegrees + YanamiBootstrap::spinnerSweepDegrees)
        * 3.14159265358979323846 / 180.0;
    const int centerX = (ringBounds.left + ringBounds.right) / 2;
    const int centerY = (ringBounds.top + ringBounds.bottom) / 2;
    const int radiusX = (ringBounds.right - ringBounds.left) / 2;
    const int radiusY = (ringBounds.bottom - ringBounds.top) / 2;
    HGDIOBJ oldPen = SelectObject(device, state.ringPen);
    Arc(device, ringBounds.left, ringBounds.top, ringBounds.right,
        ringBounds.bottom,
        centerX + static_cast<int>(std::cos(startRadians) * radiusX),
        centerY - static_cast<int>(std::sin(startRadians) * radiusY),
        centerX + static_cast<int>(std::cos(endRadians) * radiusX),
        centerY - static_cast<int>(std::sin(endRadians) * radiusY));
    SelectObject(device, oldPen);
}

bool createDrawingBuffer(
    HDC device, int width, int height,
    HDC &buffer, HBITMAP &bitmap, HGDIOBJ &previousBitmap,
    std::uint32_t *&pixels)
{
    buffer = CreateCompatibleDC(device);
    if (!buffer)
        return false;

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void *bitmapPixels = nullptr;
    bitmap = CreateDIBSection(
        device, &bitmapInfo, DIB_RGB_COLORS, &bitmapPixels, nullptr, 0);
    pixels = static_cast<std::uint32_t *>(bitmapPixels);
    if (!bitmap || !pixels) {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(buffer);
        buffer = nullptr;
        bitmap = nullptr;
        pixels = nullptr;
        return false;
    }
    previousBitmap = SelectObject(buffer, bitmap);
    if (!previousBitmap || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(buffer);
        buffer = nullptr;
        bitmap = nullptr;
        previousBitmap = nullptr;
        pixels = nullptr;
        return false;
    }
    return true;
}

bool ensureDrawingBuffers(SplashState &state, HDC device)
{
    if (state.staticBuffer && state.frameBuffer && state.spinnerMaskBuffer)
        return true;

    const RECT paintBounds = spinnerPaintBounds(state);
    const int maskWidth =
        (paintBounds.right - paintBounds.left)
        * YanamiBootstrap::spinnerSupersample;
    const int maskHeight =
        (paintBounds.bottom - paintBounds.top)
        * YanamiBootstrap::spinnerSupersample;
    const auto &metrics = state.metrics;
    if (createDrawingBuffer(
            device, metrics.width, metrics.height,
            state.staticBuffer, state.staticBufferBitmap,
            state.previousStaticBufferBitmap, state.staticBufferPixels)
        && createDrawingBuffer(
            device, metrics.width, metrics.height,
            state.frameBuffer, state.frameBufferBitmap,
            state.previousFrameBufferBitmap, state.frameBufferPixels)
        && createDrawingBuffer(
            device, maskWidth, maskHeight,
            state.spinnerMaskBuffer, state.spinnerMaskBitmap,
            state.previousSpinnerMaskBitmap, state.spinnerMaskPixels)) {
        return true;
    }
    releaseDrawingBuffer(
        state.spinnerMaskBuffer, state.spinnerMaskBitmap,
        state.previousSpinnerMaskBitmap, state.spinnerMaskPixels);
    releaseDrawingBuffer(
        state.frameBuffer, state.frameBufferBitmap,
        state.previousFrameBufferBitmap, state.frameBufferPixels);
    releaseDrawingBuffer(
        state.staticBuffer, state.staticBufferBitmap,
        state.previousStaticBufferBitmap, state.staticBufferPixels);
    return false;
}

void drawAntiAliasedSpinner(
    SplashState &state, int startDegrees)
{
    if (!state.spinnerMaskBuffer || !state.spinnerMaskPixels
        || !state.frameBufferPixels) {
        drawSpinnerArc(state, state.frameBuffer, startDegrees);
        return;
    }

    // Keep the quality work confined to the small spinner rectangle. GDI's
    // Arc primitive has hard pixel edges, so render a monochrome mask at 2x,
    // box-filter it to physical pixels, and blend it over the cached frame.
    // This avoids loading Direct2D/GDI+ during the latency-sensitive bootstrap.
    GdiFlush();
    const RECT paintBounds = spinnerPaintBounds(state);
    const RECT ringBounds = spinnerBounds(state);
    constexpr int sample = YanamiBootstrap::spinnerSupersample;
    const int targetWidth = paintBounds.right - paintBounds.left;
    const int targetHeight = paintBounds.bottom - paintBounds.top;
    const int maskWidth = targetWidth * sample;
    const int maskHeight = targetHeight * sample;
    std::fill_n(
        state.spinnerMaskPixels,
        static_cast<std::size_t>(maskWidth) * maskHeight, 0u);

    const RECT localRing {
        (ringBounds.left - paintBounds.left) * sample,
        (ringBounds.top - paintBounds.top) * sample,
        (ringBounds.right - paintBounds.left) * sample,
        (ringBounds.bottom - paintBounds.top) * sample,
    };
    const double startRadians = startDegrees * 3.14159265358979323846 / 180.0;
    const double endRadians =
        (startDegrees + YanamiBootstrap::spinnerSweepDegrees)
        * 3.14159265358979323846 / 180.0;
    const int centerX = (localRing.left + localRing.right) / 2;
    const int centerY = (localRing.top + localRing.bottom) / 2;
    const int radiusX = (localRing.right - localRing.left) / 2;
    const int radiusY = (localRing.bottom - localRing.top) / 2;
    const POINT startPoint {
        centerX + static_cast<int>(std::cos(startRadians) * radiusX),
        centerY - static_cast<int>(std::sin(startRadians) * radiusY)};
    const POINT endPoint {
        centerX + static_cast<int>(std::cos(endRadians) * radiusX),
        centerY - static_cast<int>(std::sin(endRadians) * radiusY)};

    HGDIOBJ oldPen = SelectObject(
        state.spinnerMaskBuffer, state.spinnerMaskPen);
    HGDIOBJ oldBrush = SelectObject(
        state.spinnerMaskBuffer, GetStockObject(NULL_BRUSH));
    Arc(
        state.spinnerMaskBuffer,
        localRing.left, localRing.top, localRing.right, localRing.bottom,
        startPoint.x, startPoint.y, endPoint.x, endPoint.y);

    SelectObject(state.spinnerMaskBuffer, GetStockObject(NULL_PEN));
    SelectObject(state.spinnerMaskBuffer, state.spinnerMaskBrush);
    const int capRadius = std::max(
        1, state.metrics.spinnerThickness * sample / 2);
    for (const POINT point : {startPoint, endPoint}) {
        Ellipse(
            state.spinnerMaskBuffer,
            point.x - capRadius, point.y - capRadius,
            point.x + capRadius + 1, point.y + capRadius + 1);
    }
    SelectObject(state.spinnerMaskBuffer, oldBrush);
    SelectObject(state.spinnerMaskBuffer, oldPen);
    GdiFlush();

    constexpr unsigned int sampleCount = sample * sample;
    const unsigned int sourceRed = GetRValue(spinnerColor);
    const unsigned int sourceGreen = GetGValue(spinnerColor);
    const unsigned int sourceBlue = GetBValue(spinnerColor);
    for (int targetY = 0; targetY < targetHeight; ++targetY) {
        for (int targetX = 0; targetX < targetWidth; ++targetX) {
            unsigned int coverageSum = 0;
            for (int sampleY = 0; sampleY < sample; ++sampleY) {
                const std::size_t row = static_cast<std::size_t>(
                    targetY * sample + sampleY) * maskWidth;
                for (int sampleX = 0; sampleX < sample; ++sampleX) {
                    coverageSum += state.spinnerMaskPixels[
                        row + targetX * sample + sampleX] & 0xffu;
                }
            }
            const unsigned int alpha =
                (coverageSum + sampleCount / 2) / sampleCount;
            if (alpha == 0)
                continue;

            const std::size_t destination = static_cast<std::size_t>(
                paintBounds.top + targetY) * state.metrics.width
                + paintBounds.left + targetX;
            const std::uint32_t original = state.frameBufferPixels[destination];
            const unsigned int inverseAlpha = 255u - alpha;
            const unsigned int red =
                (((original >> 16) & 0xffu) * inverseAlpha
                 + sourceRed * alpha + 127u) / 255u;
            const unsigned int green =
                (((original >> 8) & 0xffu) * inverseAlpha
                 + sourceGreen * alpha + 127u) / 255u;
            const unsigned int blue =
                ((original & 0xffu) * inverseAlpha
                 + sourceBlue * alpha + 127u) / 255u;
            state.frameBufferPixels[destination] =
                (red << 16) | (green << 8) | blue;
        }
    }
}

YanamiBootstrap::SpinnerAnimationFrame currentSpinnerFrame(
    const SplashState &state,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    return YanamiBootstrap::spinnerAnimationFrameAt(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - state.animationEpoch));
}

void rebuildBufferedFrame(SplashState &state)
{
    drawStaticSplash(state, state.staticBuffer);
    BitBlt(
        state.frameBuffer, 0, 0, state.metrics.width, state.metrics.height,
        state.staticBuffer, 0, 0, SRCCOPY);
    const auto frame = currentSpinnerFrame(state);
    drawAntiAliasedSpinner(state, frame.startDegrees);
    state.renderedSpinnerFrame = frame.ordinal;
    state.staticFrameDirty = false;
}

void advanceBufferedSpinner(
    SplashState &state, std::chrono::steady_clock::time_point now)
{
    const auto frame = currentSpinnerFrame(state, now);
    if (frame.ordinal == state.renderedSpinnerFrame)
        return;
    if (!state.staticBuffer || !state.frameBuffer || state.staticFrameDirty) {
        InvalidateRect(state.window, nullptr, FALSE);
        return;
    }

    const RECT paintBounds = spinnerPaintBounds(state);
    BitBlt(
        state.frameBuffer, paintBounds.left, paintBounds.top,
        paintBounds.right - paintBounds.left,
        paintBounds.bottom - paintBounds.top,
        state.staticBuffer, paintBounds.left, paintBounds.top, SRCCOPY);
    drawAntiAliasedSpinner(state, frame.startDegrees);
    state.renderedSpinnerFrame = frame.ordinal;
    InvalidateRect(state.window, &paintBounds, FALSE);
}

void setSplashStatus(SplashState &state, const std::wstring &status)
{
    if (state.status == status)
        return;
    state.status = status;
    state.staticFrameDirty = true;
    if (state.window)
        InvalidateRect(state.window, nullptr, FALSE);
}

void drawCancelButton(
    const DRAWITEMSTRUCT &drawItem, const SplashState &state)
{
    const bool disabled = (drawItem.itemState & ODS_DISABLED) != 0;
    const bool pressed = (drawItem.itemState & ODS_SELECTED) != 0;
    const COLORREF background = disabled ? RGB(18, 24, 37)
        : pressed ? RGB(34, 45, 68) : RGB(24, 33, 51);
    HBRUSH brush = CreateSolidBrush(background);
    HPEN border = CreatePen(
        PS_SOLID, state.metrics.borderThickness,
        disabled ? RGB(42, 51, 70) : RGB(62, 78, 108));
    HGDIOBJ oldBrush = SelectObject(drawItem.hDC, brush);
    HGDIOBJ oldPen = SelectObject(drawItem.hDC, border);
    RoundRect(
        drawItem.hDC, drawItem.rcItem.left, drawItem.rcItem.top,
        drawItem.rcItem.right, drawItem.rcItem.bottom,
        state.metrics.buttonCornerRadius,
        state.metrics.buttonCornerRadius);
    SelectObject(drawItem.hDC, oldPen);
    SelectObject(drawItem.hDC, oldBrush);
    DeleteObject(border);
    DeleteObject(brush);

    SetBkMode(drawItem.hDC, TRANSPARENT);
    SetTextColor(
        drawItem.hDC, disabled ? RGB(91, 103, 126) : RGB(183, 195, 216));
    HGDIOBJ oldFont = SelectObject(drawItem.hDC, state.buttonFont);
    RECT textBounds = drawItem.rcItem;
    DrawTextW(
        drawItem.hDC, state.text.cancel.c_str(), -1, &textBounds,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(drawItem.hDC, oldFont);

    if ((drawItem.itemState & ODS_FOCUS) != 0 && !disabled) {
        RECT focusBounds = drawItem.rcItem;
        InflateRect(
            &focusBounds,
            -state.metrics.buttonFocusInset,
            -state.metrics.buttonFocusInset);
        DrawFocusRect(drawItem.hDC, &focusBounds);
    }
}

unsigned int dpiForWindow(HWND window)
{
    using GetDpiForWindowFunction = UINT(WINAPI *)(HWND);
    if (auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFunction>(
            GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))) {
        const UINT dpi = getDpiForWindow(window);
        if (dpi != 0)
            return dpi;
    }

    HDC device = GetDC(window);
    const int dpi = device ? GetDeviceCaps(device, LOGPIXELSX) : 0;
    if (device)
        ReleaseDC(window, device);
    return dpi > 0
        ? static_cast<unsigned int>(dpi)
        : YanamiBootstrap::baselineDpi;
}

std::optional<SIZE> iconPixelSize(HICON icon)
{
    if (!icon)
        return std::nullopt;

    ICONINFO information {};
    if (!GetIconInfo(icon, &information))
        return std::nullopt;

    SIZE size {};
    BITMAP bitmap {};
    if (information.hbmColor
        && GetObjectW(
            information.hbmColor, sizeof(bitmap), &bitmap)
            == sizeof(bitmap)) {
        size = {bitmap.bmWidth, bitmap.bmHeight};
    } else if (information.hbmMask
        && GetObjectW(
            information.hbmMask, sizeof(bitmap), &bitmap)
            == sizeof(bitmap)) {
        size = {bitmap.bmWidth, bitmap.bmHeight / 2};
    }

    if (information.hbmColor)
        DeleteObject(information.hbmColor);
    if (information.hbmMask)
        DeleteObject(information.hbmMask);
    if (size.cx <= 0 || size.cy <= 0)
        return std::nullopt;
    return size;
}

LRESULT CALLBACK splashWindowProcedure(
    HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter)
{
    auto *state = reinterpret_cast<SplashState *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(longParameter);
        state = reinterpret_cast<SplashState *>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE:
        if (state) {
            state->window = window;
            state->metrics = YanamiBootstrap::splashMetricsForDpi(
                dpiForWindow(window));
            const int brandSourceSize =
                YanamiBootstrap::nearestSplashIconPhysicalSize(
                    state->metrics.iconSize);
            state->brandIcon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), IMAGE_ICON,
                brandSourceSize, brandSourceSize, LR_DEFAULTCOLOR));
            if (const auto loadedSize = iconPixelSize(state->brandIcon);
                loadedSize && loadedSize->cx == loadedSize->cy) {
                state->brandIconSize = loadedSize->cx;
            }
            initializeDrawingResources(*state);
            state->cancelButton = CreateWindowExW(
                0, L"BUTTON", state->text.cancel.c_str(),
                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                state->metrics.width - state->metrics.buttonRightMargin
                    - state->metrics.buttonWidth,
                state->metrics.height - state->metrics.buttonBottomMargin
                    - state->metrics.buttonHeight,
                state->metrics.buttonWidth, state->metrics.buttonHeight,
                window, reinterpret_cast<HMENU>(cancelButtonId),
                GetModuleHandleW(nullptr), nullptr);
            SetTimer(
                window, animationTimerId,
                static_cast<UINT>(
                    YanamiBootstrap::spinnerFrameInterval.count()),
                nullptr);
        }
        return 0;
    case WM_COMMAND:
        if (state && LOWORD(wordParameter) == cancelButtonId) {
            state->cancelRequested = true;
            setSplashStatus(*state, state->text.cancelling);
            EnableWindow(state->cancelButton, FALSE);
        }
        return 0;
    case WM_CLOSE:
        if (state) {
            state->cancelRequested = true;
            setSplashStatus(*state, state->text.cancelling);
            EnableWindow(state->cancelButton, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (!state)
            return 0;
        {
            const auto now = std::chrono::steady_clock::now();
            if (!state->cancelButtonShown && !state->fading
                && now - state->animationEpoch
                >= cancelRevealDelay) {
                ShowWindow(state->cancelButton, SW_SHOW);
                state->cancelButtonShown = true;
            }
            if (state->fading) {
                const auto elapsed = now - state->fadeEpoch;
                if (elapsed >= handoffDuration) {
                    state->fadeComplete = true;
                    ShowWindow(window, SW_HIDE);
                    KillTimer(window, animationTimerId);
                } else {
                    const auto remaining = handoffDuration - elapsed;
                    const auto alpha = static_cast<BYTE>(
                        255 * std::chrono::duration<double>(remaining).count()
                        / std::chrono::duration<double>(handoffDuration).count());
                    SetLayeredWindowAttributes(window, 0, alpha, LWA_ALPHA);
                }
            } else {
                // The logo, text and background live in an immutable memory
                // surface. A tick restores only the spinner rectangle from
                // that surface, draws the new arc, and invalidates that small
                // region for presentation.
                advanceBufferedSpinner(*state, now);
            }
        }
        return 0;
    case WM_DRAWITEM:
        if (state && wordParameter == cancelButtonId) {
            drawCancelButton(
                *reinterpret_cast<DRAWITEMSTRUCT *>(longParameter), *state);
            return TRUE;
        }
        return DefWindowProcW(window, message, wordParameter, longParameter);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT paint {};
            HDC device = BeginPaint(window, &paint);
            if (ensureDrawingBuffers(*state, device)) {
                if (state->staticFrameDirty)
                    rebuildBufferedFrame(*state);
                const RECT &paintBounds = paint.rcPaint;
                BitBlt(
                    device, paintBounds.left, paintBounds.top,
                    paintBounds.right - paintBounds.left,
                    paintBounds.bottom - paintBounds.top,
                    state->frameBuffer, paintBounds.left, paintBounds.top,
                    SRCCOPY);
                RECT spinnerIntersection {};
                const RECT spinnerBounds = spinnerPaintBounds(*state);
                if (IntersectRect(
                        &spinnerIntersection, &paintBounds, &spinnerBounds)
                    && state->renderedSpinnerFrame
                        != state->lastPresentedSpinnerFrame) {
                    state->lastPresentedSpinnerFrame =
                        state->renderedSpinnerFrame;
                    ++state->spinnerPaintCount;
                }
            } else {
                drawStaticSplash(*state, device);
                drawSpinnerArc(
                    *state, device, currentSpinnerFrame(*state).startDegrees);
            }
            EndPaint(window, &paint);
        }
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(window, message, wordParameter, longParameter);
    }
}

HWND createSplashWindow(SplashState &state)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass {
        sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW,
        &splashWindowProcedure, 0, 0, instance,
        state.icon, LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), nullptr, nullptr,
        windowClassName, state.icon};
    if (!RegisterClassExW(&windowClass)
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    POINT cursor {};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo {sizeof(MONITORINFO)};
    GetMonitorInfoW(monitor, &monitorInfo);
    const auto initialMetrics = YanamiBootstrap::splashMetricsForDpi(
        YanamiBootstrap::baselineDpi);
    const int x = monitorInfo.rcWork.left
        + (monitorInfo.rcWork.right - monitorInfo.rcWork.left
           - initialMetrics.width) / 2;
    const int y = monitorInfo.rcWork.top
        + (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top
           - initialMetrics.height) / 2;
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_LAYERED,
        windowClassName, state.text.windowTitle.c_str(),
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, initialMetrics.width, initialMetrics.height,
        nullptr, nullptr, instance, &state);
    if (!window)
        return nullptr;
    const int scaledX = monitorInfo.rcWork.left
        + (monitorInfo.rcWork.right - monitorInfo.rcWork.left
           - state.metrics.width) / 2;
    const int scaledY = monitorInfo.rcWork.top
        + (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top
           - state.metrics.height) / 2;
    SetWindowPos(
        window, nullptr, scaledX, scaledY,
        state.metrics.width, state.metrics.height,
        SWP_NOACTIVATE | SWP_NOZORDER);
    HRGN region = CreateRoundRectRgn(
        0, 0, state.metrics.width + 1, state.metrics.height + 1,
        state.metrics.cornerRadius, state.metrics.cornerRadius);
    SetWindowRgn(window, region, TRUE);
    SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return window;
}

std::chrono::steady_clock::duration pumpMessages()
{
    // The animation timer can continuously replenish the queue while a
    // cold GDI/icon paint is still expensive. Bound message draining so ready
    // file and child-process polling cannot be starved by cosmetic frames.
    constexpr int maximumMessagesPerPump = 24;
    const auto deadline = std::chrono::steady_clock::now() + 4ms;
    MSG message {};
    int processed = 0;
    std::chrono::steady_clock::duration maximumDispatchGap {};
    while (processed < maximumMessagesPerPump
           && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        const auto dispatchStarted = std::chrono::steady_clock::now();
        TranslateMessage(&message);
        DispatchMessageW(&message);
        maximumDispatchGap = std::max(
            maximumDispatchGap,
            std::chrono::steady_clock::now() - dispatchStarted);
        ++processed;
        if (std::chrono::steady_clock::now() >= deadline)
            break;
    }
    return maximumDispatchGap;
}

std::uint64_t durationCeilMilliseconds(
    std::chrono::steady_clock::duration duration)
{
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    return static_cast<std::uint64_t>(
        (std::max<std::int64_t>(0, nanoseconds) + 999'999) / 1'000'000);
}

struct ChildProcess {
    PROCESS_INFORMATION information {};
    std::filesystem::path readyDirectory;
    std::filesystem::path readyFile;
    HANDLE readyEvent = nullptr;
};

std::optional<ChildProcess> launchDesktop(
    const std::filesystem::path &desktopPath,
    const std::vector<std::wstring> &forwardedArguments,
    HANDLE instanceMutex,
    const LauncherText &text,
    std::wstring &error)
{
    const auto readyDirectory = createPrivateReadyDirectory();
    if (!readyDirectory) {
        error = text.readyDirectoryError;
        return std::nullopt;
    }
    const std::filesystem::path readyFile = *readyDirectory / readyFileName;
    SECURITY_ATTRIBUTES eventSecurity {
        sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readyEvent = CreateEventW(
        &eventSecurity, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        cleanupReadyDirectory(*readyDirectory);
        error = text.instanceHandoffError;
        return std::nullopt;
    }

    std::wstring commandLine = quoteCommandLineArgument(desktopPath.wstring());
    for (const std::wstring &argument : forwardedArguments) {
        commandLine.push_back(L' ');
        commandLine += quoteCommandLineArgument(argument);
    }
    commandLine.push_back(L' ');
    commandLine += quoteCommandLineArgument(
        wide(YanamiBootstrap::readyFileArgument(readyFile)));
    commandLine.push_back(L' ');
    commandLine += quoteCommandLineArgument(wide(
        YanamiBootstrap::readyHandleArgument(
            reinterpret_cast<std::uintptr_t>(readyEvent))));

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::byte> attributeStorage(attributeSize);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(
            attributes, 1, 0, &attributeSize)) {
        CloseHandle(readyEvent);
        cleanupReadyDirectory(*readyDirectory);
        error = text.processAttributesError;
        return std::nullopt;
    }
    HANDLE inheritedHandles[] = {instanceMutex, readyEvent};
    if (!UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(readyEvent);
        cleanupReadyDirectory(*readyDirectory);
        error = text.instanceHandoffError;
        return std::nullopt;
    }

    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process {};
    const std::wstring workingDirectory = desktopPath.parent_path().wstring();
    const BOOL created = CreateProcessW(
        desktopPath.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, workingDirectory.c_str(), &startup.StartupInfo, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        CloseHandle(readyEvent);
        cleanupReadyDirectory(*readyDirectory);
        error = text.desktopStartError + formatWindowsError(createError);
        return std::nullopt;
    }
    return ChildProcess {process, *readyDirectory, readyFile, readyEvent};
}

bool requestSafeCancellation(ChildProcess &child, SplashState &splash)
{
    if (HWND desktopWindow = topLevelWindowForProcess(
            child.information.dwProcessId)) {
        PostMessageW(desktopWindow, WM_CLOSE, 0, 0);
        const DWORD result = WaitForSingleObject(
            child.information.hProcess, 2500);
        if (result == WAIT_OBJECT_0)
            return true;
    }
    const int decision = MessageBoxW(
        splash.window,
        splash.text.forceStopMessage.c_str(),
        splash.text.forceStopTitle.c_str(),
        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    if (decision != IDYES) {
        splash.cancelRequested = false;
        setSplashStatus(splash, splash.text.continuing);
        EnableWindow(splash.cancelButton, TRUE);
        return false;
    }
    TerminateProcess(
        child.information.hProcess,
        static_cast<UINT>(YanamiBootstrap::ExitCode::Cancelled));
    WaitForSingleObject(child.information.hProcess, 5000);
    return true;
}

int monitorDesktop(
    ChildProcess &child,
    const YanamiBootstrap::LauncherOptions &options,
    SplashState &splash,
    YanamiBootstrap::TraceWriter &trace)
{
    const auto started = std::chrono::steady_clock::now();
    auto deadline = started + options.readyTimeout;
    bool desktopReady = false;
    bool handoffMarked = false;
    bool animationMarked = false;
    std::optional<DWORD> observedChildExitCode;
    std::chrono::steady_clock::duration maximumDispatchGap {};
    std::chrono::steady_clock::duration maximumLoopGap {};
    std::chrono::steady_clock::duration maximumExistsCall {};
    auto previousLoopStarted = started;
    while (true) {
        const auto loopStarted = std::chrono::steady_clock::now();
        maximumLoopGap = std::max(
            maximumLoopGap, loopStarted - previousLoopStarted);
        previousLoopStarted = loopStarted;
        maximumDispatchGap = std::max(
            maximumDispatchGap, pumpMessages());
        if (!animationMarked && splash.spinnerPaintCount >= 2) {
            animationMarked = true;
            trace.mark(
                "bootstrap_animation_active", false,
                child.information.dwProcessId, "indeterminate");
        }

        if (splash.cancelRequested && !requestSafeCancellation(child, splash)) {
            deadline = std::chrono::steady_clock::now() + options.readyTimeout;
        } else if (splash.cancelRequested) {
            return static_cast<int>(YanamiBootstrap::ExitCode::Cancelled);
        }

        const bool readyEventSignaled = !desktopReady
            && WaitForSingleObject(child.readyEvent, 0) == WAIT_OBJECT_0;
        if (readyEventSignaled) {
            YanamiBootstrap::TraceWriter::MonitorDiagnostics diagnostics;
            diagnostics.maxDispatchGapMs =
                durationCeilMilliseconds(maximumDispatchGap);
            diagnostics.maxLoopGapMs =
                durationCeilMilliseconds(maximumLoopGap);
            const auto probeStarted = std::chrono::steady_clock::now();
            const DWORD readyAttributes = GetFileAttributesW(
                child.readyFile.c_str());
            const bool diagnosticReadyFilePresent =
                readyAttributes != INVALID_FILE_ATTRIBUTES
                && (readyAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
            maximumExistsCall = std::max(
                maximumExistsCall,
                std::chrono::steady_clock::now() - probeStarted);
            diagnostics.maxExistsCallMs =
                durationCeilMilliseconds(maximumExistsCall);
            diagnostics.readyFirstSeenMs = durationCeilMilliseconds(
                std::chrono::steady_clock::now() - started);
            diagnostics.validateMs = 0;
            diagnostics.validationAttributesMs = 0;
            diagnostics.validationOpenMs = 0;
            diagnostics.validationHandleInformationMs = 0;
            diagnostics.validationSizeMs = 0;
            diagnostics.validationReadMs = 0;
            diagnostics.validationCloseMs = 0;
            trace.mark(
                diagnosticReadyFilePresent
                    ? "bootstrap_ready_event_with_file"
                    : "bootstrap_ready_event_without_file",
                false,
                child.information.dwProcessId, "indeterminate", diagnostics);
            desktopReady = true;
            trace.mark(
                "desktop_ready", true, child.information.dwProcessId,
                {}, diagnostics);
            activateWindow(topLevelWindowForProcess(
                child.information.dwProcessId));
            if (splash.window) {
                // Keep the brand surface visually above the already-active Qt
                // window during the short fade without stealing focus back.
                SetWindowPos(
                    splash.window, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                splash.fading = true;
                splash.fadeEpoch = std::chrono::steady_clock::now();
                EnableWindow(splash.cancelButton, FALSE);
                ShowWindow(splash.cancelButton, SW_HIDE);
            } else {
                splash.fadeComplete = true;
            }
        }

        if (desktopReady && splash.fadeComplete && !handoffMarked) {
            handoffMarked = true;
            trace.mark(
                "handoff_complete", true, child.information.dwProcessId);
            if (observedChildExitCode)
                return static_cast<int>(*observedChildExitCode);
            if (!options.waitForDesktopExit())
                return static_cast<int>(YanamiBootstrap::ExitCode::Success);
        }

        const DWORD processState = observedChildExitCode
            ? WAIT_TIMEOUT
            : WaitForSingleObject(child.information.hProcess, 0);
        if (processState == WAIT_OBJECT_0) {
            DWORD childExitCode = 0;
            GetExitCodeProcess(child.information.hProcess, &childExitCode);
            if (desktopReady) {
                if (handoffMarked)
                    return static_cast<int>(childExitCode);
                // The desktop may intentionally auto-exit immediately after
                // writing ready (runtime/perf smoke). Preserve its exit code,
                // but finish the already-started visual handoff and emit the
                // complete protocol boundary before returning.
                observedChildExitCode = childExitCode;
                continue;
            }
            if (options.waitForDesktopExit())
                return static_cast<int>(childExitCode);
            if (!desktopReady) {
                const std::wstring message =
                    splash.text.exitedBeforeReady
                    + std::to_wstring(childExitCode)
                    + splash.text.retryOrReinstall;
                const int choice = MessageBoxW(
                    splash.window, message.c_str(),
                    splash.text.couldNotStartTitle.c_str(),
                    MB_ICONERROR | MB_RETRYCANCEL);
                return choice == IDRETRY ? -1
                    : static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
            }
            return static_cast<int>(childExitCode);
        }

        if (!desktopReady && std::chrono::steady_clock::now() >= deadline) {
            if (options.waitForDesktopExit()) {
                TerminateProcess(
                    child.information.hProcess,
                    static_cast<UINT>(YanamiBootstrap::ExitCode::ReadyTimeout));
                WaitForSingleObject(child.information.hProcess, 5000);
                return static_cast<int>(YanamiBootstrap::ExitCode::ReadyTimeout);
            }
            setSplashStatus(splash, splash.text.takingLong);
            const int choice = MessageBoxW(
                splash.window,
                splash.text.timeoutMessage.c_str(),
                splash.text.timeoutTitle.c_str(),
                MB_ICONWARNING | MB_RETRYCANCEL);
            if (choice == IDRETRY) {
                deadline = std::chrono::steady_clock::now()
                    + options.readyTimeout;
                setSplashStatus(splash, splash.text.continuing);
            } else {
                splash.cancelRequested = true;
            }
        }

        HANDLE waitHandles[] = {
            child.information.hProcess,
            child.readyEvent,
        };
        MsgWaitForMultipleObjects(
            desktopReady ? 1 : 2, waitHandles,
            FALSE, 16, QS_ALLINPUT);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    if (auto setDpiAwareness = reinterpret_cast<BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                           "SetProcessDpiAwarenessContext"))) {
        setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    int argumentCount = 0;
    wchar_t **argumentValues = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    std::vector<std::wstring> wideArguments;
    std::vector<std::string> encodedArguments;
    for (int index = 1; argumentValues && index < argumentCount; ++index) {
        const std::wstring argument(argumentValues[index]);
        if (utf8(argument).starts_with(
                YanamiBootstrap::timeoutArgumentPrefix)) {
            encodedArguments.push_back(utf8(argument));
            continue;
        }
        wideArguments.push_back(argument);
        encodedArguments.push_back(utf8(argument));
    }
    if (argumentValues)
        LocalFree(argumentValues);
    const YanamiBootstrap::LauncherOptions options =
        YanamiBootstrap::parseLauncherOptions(encodedArguments);
    const LauncherText text = localizedText(
        YanamiBootstrap::persistedUiLanguage());

    const std::filesystem::path desktopPath =
        std::filesystem::path(executableDirectory()) / desktopExecutableName;
    if (!std::filesystem::is_regular_file(desktopPath)) {
        MessageBoxW(
            nullptr,
            text.missingMessage.c_str(), text.missingTitle.c_str(),
            MB_ICONERROR | MB_OK);
        return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
    }

    SECURITY_ATTRIBUTES mutexSecurity {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE instanceMutex = CreateMutexW(
        &mutexSecurity, FALSE, instanceMutexName);
    if (!instanceMutex) {
        MessageBoxW(
            nullptr, text.instanceError.c_str(),
            text.startupErrorTitle.c_str(), MB_ICONERROR | MB_OK);
        return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
    }
    const bool existingInstance = GetLastError() == ERROR_ALREADY_EXISTS;
    if (existingInstance) {
        if (HWND splash = FindWindowW(windowClassName, nullptr))
            activateWindow(splash);
        else
            activateWindow(findExistingDesktopWindow(desktopPath));
        CloseHandle(instanceMutex);
        return static_cast<int>(YanamiBootstrap::ExitCode::Success);
    }

    YanamiBootstrap::TraceWriter trace(
        options.performanceTracePath, GetCurrentProcessId());
    trace.mark("bootstrap_entered", false, std::nullopt, "indeterminate");

    SplashState splash(text);
    splash.icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
        256, 256, LR_DEFAULTCOLOR));
    splash.window = createSplashWindow(splash);
    if (splash.window) {
        trace.mark(
            "bootstrap_first_visible", false, std::nullopt, "indeterminate");
    }

    int result = static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
    while (true) {
        std::wstring launchError;
        auto child = launchDesktop(
            desktopPath, wideArguments, instanceMutex, text, launchError);
        if (!child) {
            const int choice = MessageBoxW(
                splash.window, launchError.c_str(),
                text.couldNotStartTitle.c_str(),
                MB_ICONERROR | MB_RETRYCANCEL);
            if (choice == IDRETRY)
                continue;
            result = static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
            break;
        }
        trace.mark(
            "bootstrap_desktop_spawned", false,
            child->information.dwProcessId, "indeterminate");
        setSplashStatus(splash, text.starting);
        result = monitorDesktop(*child, options, splash, trace);
        cleanupReadyDirectory(child->readyDirectory);
        CloseHandle(child->readyEvent);
        CloseHandle(child->information.hThread);
        CloseHandle(child->information.hProcess);
        if (result == -1) {
            setSplashStatus(splash, text.retrying);
            splash.cancelRequested = false;
            EnableWindow(splash.cancelButton, TRUE);
            continue;
        }
        break;
    }

    if (splash.window)
        DestroyWindow(splash.window);
    if (splash.icon)
        DestroyIcon(splash.icon);
    if (splash.brandIcon)
        DestroyIcon(splash.brandIcon);
    releaseDrawingResources(splash);
    CloseHandle(instanceMutex);
    return result;
}
