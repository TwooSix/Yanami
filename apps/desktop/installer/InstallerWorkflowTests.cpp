// Exercise the real native window without installing, launching, or changing
// the user's registry/shortcuts. Include Windows headers before the narrow
// call substitutions so imported API declarations remain untouched.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <uxtheme.h>

#include "InstallerCore.hpp"
#include "InstallerLayout.hpp"
#include "InstallerPainter.hpp"
#include "InstallerShortcuts.hpp"

namespace WorkflowHarness {
HINSTANCE WINAPI shellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
int WINAPI messageBox(HWND, LPCWSTR, LPCWSTR, UINT);
BOOL WINAPI showWindow(HWND, int);
HDC WINAPI beginPaint(HWND, LPPAINTSTRUCT);
BOOL WINAPI endPaint(HWND, const PAINTSTRUCT*);
int WINAPI fillRect(HDC, const RECT*, HBRUSH);
BOOL WINAPI invalidateRect(HWND, const RECT*, BOOL);
BOOL WINAPI redrawWindow(HWND, const RECT*, HRGN, UINT);
BOOL WINAPI getCursorPos(LPPOINT);
}

#define YANAMI_INSTALLER_TESTING
#define ShellExecuteW WorkflowHarness::shellExecute
#define MessageBoxW WorkflowHarness::messageBox
#define ShowWindow WorkflowHarness::showWindow
#define BeginPaint WorkflowHarness::beginPaint
#define EndPaint WorkflowHarness::endPaint
#define FillRect WorkflowHarness::fillRect
#define InvalidateRect WorkflowHarness::invalidateRect
#define RedrawWindow WorkflowHarness::redrawWindow
#define GetCursorPos WorkflowHarness::getCursorPos
#include "InstallerMain.cpp"
#undef GetCursorPos
#undef RedrawWindow
#undef InvalidateRect
#undef FillRect
#undef EndPaint
#undef BeginPaint
#undef ShowWindow
#undef MessageBoxW
#undef ShellExecuteW

#include <functional>
#include <iostream>
#include <stdexcept>

namespace WorkflowHarness {
struct ShellAction {
    std::wstring verb;
    std::wstring target;
    std::wstring workingDirectory;
};

struct FirstShowPlacement {
    HWND window = nullptr;
    RECT outer{};
    RECT client{};
    RECT workArea{};
    UINT dpi = 0;
    bool captured = false;
    bool alreadyVisible = false;
    HMONITOR monitor = nullptr;
};

struct StartupCursorProbe {
    bool succeeded = false;
    POINT position{};
    HMONITOR monitor = nullptr;
};

bool preview = false;
std::vector<ShellAction> shellActions;
std::vector<std::wstring> modalMessages;
std::optional<FirstShowPlacement> firstShowPlacement;
std::optional<StartupCursorProbe> startupCursorProbe;
HWND observedPaintWindow = nullptr;
HDC observedPaintDc = nullptr;
unsigned rootPaintCalls = 0;
unsigned directRootFillCalls = 0;
bool observingProgressTimer = false;
unsigned progressInvalidationCalls = 0;
bool progressInvalidationErases = false;
bool progressInvalidationCoversWholeWindow = false;
RECT lastProgressInvalidation{};
unsigned rootFullRedrawRequests = 0;

HINSTANCE WINAPI shellExecute(
    HWND, LPCWSTR verb, LPCWSTR target, LPCWSTR, LPCWSTR directory, INT) {
    shellActions.push_back({verb ? verb : L"", target ? target : L"",
                            directory ? directory : L""});
    if (preview) {
        std::cout << "Preview suppressed external action: "
                  << wideToUtf8(shellActions.back().target) << '\n';
    }
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(33));
}

int WINAPI messageBox(HWND, LPCWSTR message, LPCWSTR, UINT) {
    modalMessages.emplace_back(message ? message : L"");
    if (preview) {
        std::cout << "Preview suppressed modal: "
                  << wideToUtf8(modalMessages.back()) << std::endl;
    }
    return IDOK;
}

BOOL WINAPI getCursorPos(LPPOINT point) {
    const BOOL result = ::GetCursorPos(point);
    if (!startupCursorProbe) {
        StartupCursorProbe probe;
        probe.succeeded = result != FALSE;
        if (probe.succeeded) {
            probe.position = *point;
            probe.monitor = MonitorFromPoint(*point, MONITOR_DEFAULTTONEAREST);
        }
        startupCursorProbe = probe;
    }
    return result;
}

BOOL WINAPI showWindow(HWND window, int command) {
    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    const bool installerRoot = std::wstring(className) == kWindowClass;
    if (installerRoot && command != SW_HIDE && !firstShowPlacement) {
        FirstShowPlacement placement;
        placement.window = window;
        placement.alreadyVisible = IsWindowVisible(window) != FALSE;
        placement.dpi = GetDpiForWindow(window);
        placement.monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor{sizeof(monitor)};
        placement.captured = GetWindowRect(window, &placement.outer)
            && GetClientRect(window, &placement.client)
            && GetMonitorInfoW(placement.monitor, &monitor);
        placement.workArea = monitor.rcWork;
        firstShowPlacement = placement;
        if (preview) {
            const LONG expectedLeft = placement.workArea.left + std::max<LONG>(0,
                (placement.workArea.right - placement.workArea.left
                    - (placement.outer.right - placement.outer.left)) / 2);
            const LONG expectedTop = placement.workArea.top + std::max<LONG>(0,
                (placement.workArea.bottom - placement.workArea.top
                    - (placement.outer.bottom - placement.outer.top)) / 2);
            std::cout << "Preview first-show: captured=" << placement.captured
                      << " dpi=" << placement.dpi
                      << " outer=[" << placement.outer.left << ',' << placement.outer.top
                      << ',' << placement.outer.right << ',' << placement.outer.bottom << ']'
                      << " workarea=[" << placement.workArea.left << ',' << placement.workArea.top
                      << ',' << placement.workArea.right << ',' << placement.workArea.bottom << ']'
                      << " centerDelta=" << placement.outer.left - expectedLeft
                      << ',' << placement.outer.top - expectedTop
                      << " actualMonitor=" << placement.monitor
                      << " cursorMonitor=" << (startupCursorProbe ? startupCursorProbe->monitor : nullptr)
                      << " alreadyVisible=" << placement.alreadyVisible << std::endl;
        }
    }
    if (!preview && installerRoot) {
        return ::ShowWindow(window, SW_HIDE);
    }
    return ::ShowWindow(window, command);
}

HDC WINAPI beginPaint(HWND window, LPPAINTSTRUCT paint) {
    const HDC device = ::BeginPaint(window, paint);
    if (window == observedPaintWindow) {
        ++rootPaintCalls;
        observedPaintDc = device;
    }
    return device;
}

BOOL WINAPI endPaint(HWND window, const PAINTSTRUCT* paint) {
    const BOOL result = ::EndPaint(window, paint);
    if (window == observedPaintWindow) {
        observedPaintDc = nullptr;
    }
    return result;
}

int WINAPI fillRect(HDC device, const RECT* rectangle, HBRUSH brush) {
    // Count only calls made directly by InstallerMain during the observed
    // root HWND's real WM_PAINT. Child controls, memory surfaces and other
    // windows are deliberately outside this regression's scope.
    if (observedPaintWindow != nullptr && observedPaintDc != nullptr
        && device == observedPaintDc && WindowFromDC(device) == observedPaintWindow) {
        ++directRootFillCalls;
    }
    return ::FillRect(device, rectangle, brush);
}

BOOL WINAPI invalidateRect(HWND window, const RECT* rectangle, BOOL erase) {
    if (window == observedPaintWindow && observingProgressTimer) {
        ++progressInvalidationCalls;
        progressInvalidationErases = progressInvalidationErases || erase != FALSE;
        progressInvalidationCoversWholeWindow = progressInvalidationCoversWholeWindow
            || rectangle == nullptr;
        if (rectangle != nullptr) {
            lastProgressInvalidation = *rectangle;
        }
    }
    return ::InvalidateRect(window, rectangle, erase);
}

BOOL WINAPI redrawWindow(HWND window, const RECT* rectangle, HRGN region, UINT flags) {
    if (window == observedPaintWindow && rectangle == nullptr && region == nullptr
        && (flags & (RDW_INVALIDATE | RDW_ALLCHILDREN)) == (RDW_INVALIDATE | RDW_ALLCHILDREN)) {
        ++rootFullRedrawRequests;
    }
    return ::RedrawWindow(window, rectangle, region, flags);
}
} // namespace WorkflowHarness

namespace {
namespace fs = std::filesystem;

std::optional<std::wstring> testRegisteredInstallLocation;
std::vector<InstallRequest> dispatchedRequests;
std::optional<InstallOutcome> nextOutcome;
bool holdInstallOutcomes = false;
std::wstring fixtureLog;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testDispatchInstall(const InstallRequest& request, HWND destination) {
    // There is deliberately no production-worker fallback in this TU.
    dispatchedRequests.push_back(request);
    if (holdInstallOutcomes) {
        return;
    }
    auto result = std::make_unique<InstallOutcome>(
        nextOutcome.value_or(InstallOutcome{}));
    nextOutcome.reset();
    if (result->installDirectory.empty()) {
        result->installDirectory = request.installDirectory;
    }
    if (result->launchTarget.empty()) {
        result->launchTarget = (fs::path(request.installDirectory)
                                / L"current" / L"Yanami-never-exists.exe").wstring();
    }
    if (!result->success && result->error.empty()) {
        result->error = L"Preview installation could not finish. Please retry.";
    }
    if (result->preservedLog.empty()) {
        result->preservedLog = fixtureLog;
    }
    require(PostMessageW(destination, kInstallFinishedMessage, 0,
                         reinterpret_cast<LPARAM>(result.get())) != FALSE,
            "Could not queue fake installation outcome");
    result.release(); // The real window's completion handler owns this message.
}

class InstallerWindowTestAccess {
public:
    static SIZE baselineSize(UINT dpi) { return InstallerWindow::baselineWindowSize(dpi); }
    static UINT dpi(const InstallerWindow& window) { return window.dpi_; }

    static void prepare(InstallerWindow& window, const fs::path& directory) {
        window.installDirectory_ = directory.wstring();
        window.selectedDirectory_.clear();
        window.page_ = UiPage::Options;
    }

    static UiPage page(const InstallerWindow& window) { return window.page_; }
    static bool installing(const InstallerWindow& window) { return window.installing_; }
    static bool valid(const InstallerWindow& window) { return window.pathValid_; }
    static const std::wstring& directory(const InstallerWindow& window) {
        return window.installDirectory_;
    }
    static const std::wstring& helper(const InstallerWindow& window) {
        return window.pathValidationMessage_;
    }
    static bool hasEditablePath(const InstallerWindow& window) {
        return window.pathEdit_ != nullptr;
    }
    static void deliver(InstallerWindow& window, InstallOutcome outcome) {
        window.handleInstallFinished(std::make_unique<InstallOutcome>(std::move(outcome)));
    }
    static void setPage(InstallerWindow& window, UiPage page) {
        window.page_ = page;
        window.renderPage(false);
    }
    static auto progressStartedAt(const InstallerWindow& window) {
        return window.progressStartedAt_;
    }
    static bool renderHealthy(const InstallerWindow& window) {
        return !window.renderHadFailure_ && !window.renderUnavailable_;
    }
    static bool recoveryQueued(const InstallerWindow& window) {
        return window.renderRecoveryQueued_;
    }
    static int recoveryAttempts(const InstallerWindow& window) {
        return window.renderRecoveryAttempts_;
    }
    static bool renderUnavailable(const InstallerWindow& window) {
        return window.renderUnavailable_;
    }
    static HRESULT paintError(const InstallerWindow& window) {
        return window.painter_.lastError();
    }
    static void queueRecovery(InstallerWindow& window) { window.requestRenderRecovery(); }
    static bool beginDrawing(InstallerWindow& window, HDC device, const RECT& bounds) {
        return window.beginDrawing(device, bounds);
    }
    static RECT progressUpdateBounds(const InstallerWindow& window) {
        return window.logicalRect(196, 312, 288, 14);
    }
};

void pumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        // Each fixture owns a window; WM_QUIT from the previous one must not
        // prematurely terminate the next fixture's bounded event pump.
        if (message.message != WM_QUIT) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void pumpUntil(const std::function<bool()>& predicate) {
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (!predicate() && GetTickCount64() < deadline) {
        pumpMessages();
        if (!predicate()) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    }
    require(predicate(), "Timed out delivering a fake installer outcome");
}

void pumpFor(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    while (GetTickCount64() < deadline) {
        pumpMessages();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    pumpMessages();
}

class RootPaintAudit final {
public:
    explicit RootPaintAudit(HWND window) {
        require(WorkflowHarness::observedPaintWindow == nullptr,
                "Root paint audits must not overlap");
        WorkflowHarness::observedPaintWindow = window;
        WorkflowHarness::observedPaintDc = nullptr;
        WorkflowHarness::rootPaintCalls = 0;
        WorkflowHarness::directRootFillCalls = 0;
        WorkflowHarness::observingProgressTimer = false;
        WorkflowHarness::progressInvalidationCalls = 0;
        WorkflowHarness::progressInvalidationErases = false;
        WorkflowHarness::progressInvalidationCoversWholeWindow = false;
        WorkflowHarness::lastProgressInvalidation = {};
        WorkflowHarness::rootFullRedrawRequests = 0;
    }

    ~RootPaintAudit() {
        WorkflowHarness::observedPaintWindow = nullptr;
        WorkflowHarness::observedPaintDc = nullptr;
        WorkflowHarness::observingProgressTimer = false;
    }

    RootPaintAudit(const RootPaintAudit&) = delete;
    RootPaintAudit& operator=(const RootPaintAudit&) = delete;
};

class ProgressTimerProbe final {
public:
    explicit ProgressTimerProbe(HWND window) : window_(window) {
        require(SetWindowSubclass(window_, procedure, kSubclassId,
                                  reinterpret_cast<DWORD_PTR>(this)) != FALSE,
                "Could not observe isolated installer timer messages");
    }

    ~ProgressTimerProbe() {
        if (IsWindow(window_)) {
            RemoveWindowSubclass(window_, procedure, kSubclassId);
        }
    }

    ProgressTimerProbe(const ProgressTimerProbe&) = delete;
    ProgressTimerProbe& operator=(const ProgressTimerProbe&) = delete;

    unsigned timerMessages = 0;
    unsigned invalidatingTimerMessages = 0;

private:
    static constexpr UINT_PTR kSubclassId = 0x59414e41;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR, DWORD_PTR reference) {
        auto& self = *reinterpret_cast<ProgressTimerProbe*>(reference);
        const bool progressTimer = message == WM_TIMER && wParam == kProgressTimer;
        const bool previousObservation = WorkflowHarness::observingProgressTimer;
        const unsigned previousInvalidations = WorkflowHarness::progressInvalidationCalls;
        if (WorkflowHarness::preview && message == WM_SIZE) {
            RECT client{};
            GetClientRect(window, &client);
            std::cout << "Preview WM_SIZE: type=" << wParam
                      << " iconic=" << IsIconic(window)
                      << " client=" << client.right - client.left
                      << 'x' << client.bottom - client.top << std::endl;
        }
        if (progressTimer) {
            WorkflowHarness::observingProgressTimer = true;
        }
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        WorkflowHarness::observingProgressTimer = previousObservation;
        if (progressTimer) {
            ++self.timerMessages;
            if (WorkflowHarness::progressInvalidationCalls > previousInvalidations) {
                ++self.invalidatingTimerMessages;
                // Windows may discard the dirty region of a hidden HWND.
                // Exercise its real WM_PAINT explicitly for each real timer
                // refresh, without showing a test window on the user's desktop.
                SendMessageW(window, WM_PAINT, 0, 0);
            }
        }
        return result;
    }

    HWND window_;
};

fs::path makeSuiteRoot() {
    wchar_t temporary[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporary);
    require(length > 0 && length < MAX_PATH, "GetTempPathW failed");
    for (int suffix = 0; suffix < 100; ++suffix) {
        const fs::path candidate = fs::path(temporary)
            / (L"Yanami-workflow-" + std::to_wstring(GetCurrentProcessId())
               + L"-" + std::to_wstring(GetTickCount64()) + L"-"
               + std::to_wstring(suffix));
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            return candidate;
        }
        require(!error, "Could not create isolated workflow fixture directory");
    }
    throw std::runtime_error("Could not reserve a unique workflow fixture directory");
}

void writeMarker(const fs::path& path) {
    fs::create_directories(path.parent_path());
    require(!fs::exists(path), "Fixture marker must never overwrite an existing file");
    std::ofstream stream(path, std::ios::binary);
    stream << "isolated fixture; preserve this file\n";
    require(stream.good(), "Could not write isolated fixture marker");
}

bool markerPreserved(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::string value;
    std::getline(stream, value);
    return value == "isolated fixture; preserve this file";
}

class WindowFixture final {
public:
    WindowFixture(const fs::path& root, const std::wstring& name,
                  bool registered = false,
                  UiLanguage language = UiLanguage::SimplifiedChinese)
        : base(root / name), directory(base / L"安装位置"),
          window(language, (base / L"never-a-setup.exe").wstring(),
                 (base / L"LocalAppData").wstring(),
                 {(base / L"Windows").wstring(), (base / L"ProgramFiles").wstring(),
                  (base / L"ProgramFilesX86").wstring()},
                 registered ? std::optional<std::wstring>(directory.wstring())
                            : std::nullopt) {
        require(!fs::exists(base), "Fixture directory must be unique");
        fs::create_directories(base);
        fixtureLog = (base / L"installer-preview.log").wstring();
        writeMarker(fixtureLog);
        testRegisteredInstallLocation = registered
            ? std::optional<std::wstring>(directory.wstring()) : std::nullopt;
        dispatchedRequests.clear();
        nextOutcome.reset();
        holdInstallOutcomes = false;
        WorkflowHarness::shellActions.clear();
        WorkflowHarness::modalMessages.clear();
        WorkflowHarness::firstShowPlacement.reset();
        WorkflowHarness::startupCursorProbe.reset();
        InstallerWindowTestAccess::prepare(window, directory);
        require(window.create(GetModuleHandleW(nullptr)), "Could not create installer window");
    }

    ~WindowFixture() {
        holdInstallOutcomes = false;
        // A failed assertion may leave a queued fake result. Reclaim it before
        // destroying the HWND, then clear WM_QUIT while the C++ owner is alive.
        MSG message{};
        while (PeekMessageW(&message, window.handle(), kInstallFinishedMessage,
                            kInstallFinishedMessage, PM_REMOVE)) {
            delete reinterpret_cast<InstallOutcome*>(message.lParam);
        }
        if (IsWindow(window.handle())) {
            DestroyWindow(window.handle());
        }
        pumpMessages();
    }

    void click(int identifier) {
        const HWND control = GetDlgItem(window.handle(), identifier);
        require(control != nullptr && IsWindowEnabled(control), "Expected enabled action control");
        SendMessageW(control, BM_CLICK, 0, 0);
    }

    void chooseShortcuts(bool startMenu, bool desktop) {
        SendDlgItemMessageW(window.handle(), IdStartMenu, BM_SETCHECK,
                            startMenu ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(window.handle(), IdDesktop, BM_SETCHECK,
                            desktop ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    void expectShortcuts(bool startMenu, bool desktop) const {
        require((SendDlgItemMessageW(window.handle(), IdStartMenu, BM_GETCHECK, 0, 0)
                 == BST_CHECKED) == startMenu, "Start menu selection was not preserved");
        require((SendDlgItemMessageW(window.handle(), IdDesktop, BM_GETCHECK, 0, 0)
                 == BST_CHECKED) == desktop, "Desktop selection was not preserved");
    }

    void waitFor(UiPage page) {
        pumpUntil([&] { return InstallerWindowTestAccess::page(window) == page; });
        require(!InstallerWindowTestAccess::installing(window), "Completion must release installation lock");
    }

    fs::path base;
    fs::path directory;
    InstallerWindow window;
};

void expectCenteredAxis(LONG position, LONG origin, LONG end, LONG windowSize) {
    const auto available = static_cast<std::int64_t>(end) - origin;
    if (windowSize > available) {
        require(position == origin,
                "An oversized window axis must stay at that work area's origin");
    } else {
        const auto leading = static_cast<std::int64_t>(position) - origin;
        const auto trailing = static_cast<std::int64_t>(end) - position - windowSize;
        require(leading >= 0 && trailing >= 0 && trailing - leading >= 0 && trailing - leading <= 1,
                "A fitting window axis must be centered with at most one trailing remainder pixel");
    }
}

void centeredPlacementGeometry(const fs::path&) {
    struct GoldenCase { RECT work; SIZE size; POINT expected; };
    const GoldenCase golden[]{
        {{0, 0, 1920, 1080}, {680, 560}, {620, 260}},
        {{40, 0, 1920, 1080}, {680, 560}, {640, 260}},
        {{0, 40, 1920, 1080}, {680, 560}, {620, 280}},
        {{0, 0, 1880, 1080}, {680, 560}, {600, 260}},
        {{0, 0, 1920, 1040}, {680, 560}, {620, 240}},
        {{1920, 0, 3840, 1080}, {680, 560}, {2540, 260}},
        {{-1920, 0, 0, 1080}, {680, 560}, {-1300, 260}},
        {{0, -1200, 1920, -120}, {680, 560}, {620, -940}},
        {{-1920, -1080, 0, 0}, {680, 560}, {-1300, -820}},
        {{17, -31, 1018, 770}, {680, 560}, {177, 89}},
        {{-700, 20, -200, 1220}, {680, 560}, {-700, 340}},
        {{20, -700, 1220, -200}, {680, 560}, {280, -700}},
        {{-1000, -500, -900, -400}, {680, 560}, {-1000, -500}},
        {{-680, -560, 0, 0}, {680, 560}, {-680, -560}},
    };
    for (const auto& item : golden) {
        const POINT actual = ui::centeredWindowPosition(item.work, item.size);
        require(actual.x == item.expected.x && actual.y == item.expected.y,
                "Golden startup placement disagrees with its work-area coordinates");
    }

    unsigned matrixCases = 0;
    const POINT origins[]{{0, 0}, {-2560, 0}, {0, -1440}, {1920, 240}, {-1920, -1200}};
    for (const UINT dpi : {96U, 120U, 144U, 192U}) {
        const SIZE outer = InstallerWindowTestAccess::baselineSize(dpi);
        require(outer.cx >= MulDiv(kBaselineClientWidth, dpi, 96)
                    && outer.cy >= MulDiv(kBaselineClientHeight, dpi, 96),
                "DPI matrix must include the native frame outside the fixed client");
        for (const POINT origin : origins) {
            // Includes taskbars on each edge, odd free space, exactly fitting,
            // and a work area too small in one or both independent axes.
            const RECT areas[]{
                {origin.x, origin.y, origin.x + 2560, origin.y + 1440},
                {origin.x + 48, origin.y, origin.x + 2560, origin.y + 1440},
                {origin.x, origin.y + 48, origin.x + 2560, origin.y + 1440},
                {origin.x, origin.y, origin.x + 2512, origin.y + 1440},
                {origin.x, origin.y, origin.x + 2560, origin.y + 1392},
                {origin.x, origin.y, origin.x + outer.cx + 101, origin.y + outer.cy + 203},
                {origin.x, origin.y, origin.x + outer.cx, origin.y + outer.cy},
                {origin.x, origin.y, origin.x + outer.cx - 1, origin.y + outer.cy + 101},
                {origin.x, origin.y, origin.x + outer.cx + 101, origin.y + outer.cy - 1},
                {origin.x, origin.y, origin.x + outer.cx - 1, origin.y + outer.cy - 1},
            };
            for (const RECT work : areas) {
                const POINT position = ui::centeredWindowPosition(work, outer);
                expectCenteredAxis(position.x, work.left, work.right, outer.cx);
                expectCenteredAxis(position.y, work.top, work.bottom, outer.cy);
                ++matrixCases;
            }
        }
    }
    std::cout << "Centered placement geometry: " << std::size(golden)
              << " golden + " << matrixCases << " monitor/work-area/DPI cases\n";
}

void assertRequestPath(const fs::path& path) {
    require(!dispatchedRequests.empty(), "Expected a dispatched fake install request");
    require(samePath(dispatchedRequests.back().installDirectory, path.wstring()),
            "Installer dispatched a different directory than the confirmed destination");
    require(!fs::exists(dispatchedRequests.back().selfPath), "Fake setup path must never have a payload");
    require(!dispatchedRequests.back().launchAfterInstall, "UI must defer launching until explicit Finish");
}

void successWithShortcutWarning(const fs::path& root) {
    WindowFixture fixture(root, L"complete-warning");
    InstallOutcome outcome;
    outcome.success = true;
    outcome.shortcutsIncomplete = true;
    outcome.error = L"A shortcut could not be written.";
    nextOutcome = outcome;
    fixture.click(IdInstall);
    require(InstallerWindowTestAccess::page(fixture.window) == UiPage::Installing,
            "Install should transition to its progress page before completion");
    fixture.waitFor(UiPage::Complete);
    require(WorkflowHarness::modalMessages.empty(), "Shortcut warning must not block completion with a modal");
    require(GetDlgItem(fixture.window.handle(), IdOpenLog) != nullptr,
            "Warning completion must offer its preserved log");
    fixture.click(IdOpenFolder);
    require(WorkflowHarness::shellActions.back().verb == L"explore"
                && samePath(WorkflowHarness::shellActions.back().target, fixture.directory.wstring()),
            "Open folder must target the completed installation");
    fixture.click(IdFinish);
    require(!IsWindow(fixture.window.handle()), "Finish should close the completed window");
    const auto& launch = WorkflowHarness::shellActions.back();
    require(launch.verb == L"open" && samePath(launch.target,
                (fixture.directory / L"current" / L"Yanami-never-exists.exe").wstring()),
            "Shortcut failure must not remove the launch action");
    require(samePath(launch.workingDirectory, (fixture.directory / L"current").wstring()),
            "Launch working directory should be the completed current directory");
}

void failureOffersRecoveryAndPreservesChoices(const fs::path& root) {
    WindowFixture fixture(root, L"recovery-choices");
    fixture.chooseShortcuts(false, true);
    fixture.click(IdInstall);
    assertRequestPath(fixture.directory);
    require(!dispatchedRequests.back().startMenu && dispatchedRequests.back().desktop,
            "Fake worker must receive the user's shortcut choices");
    fixture.waitFor(UiPage::Recovery);
    require(WorkflowHarness::modalMessages.empty(), "Install failure must use a nonblocking recovery page");
    require(GetDlgItem(fixture.window.handle(), IdRetry) != nullptr
                && GetDlgItem(fixture.window.handle(), IdReviewOptions) != nullptr,
            "Recovery must offer retry and return to options");
    fixture.click(IdOpenLog);
    require(WorkflowHarness::shellActions.size() == 1
                && WorkflowHarness::shellActions.back().target == fixtureLog,
            "Recovery log action must use the preserved failure log");
    fixture.click(IdReviewOptions);
    require(InstallerWindowTestAccess::page(fixture.window) == UiPage::Options,
            "Review options must return from recovery");
    fixture.expectShortcuts(false, true);
    fixture.click(IdInstall);
    fixture.waitFor(UiPage::Recovery);
    fixture.click(IdRetry);
    assertRequestPath(fixture.directory);
    require(!dispatchedRequests.back().startMenu && dispatchedRequests.back().desktop,
            "Retry must preserve shortcut choices");
    fixture.waitFor(UiPage::Recovery);
}

void newlyRegisteredInstallRepairsInPlace(const fs::path& root) {
    WindowFixture fixture(root, L"fresh-registration");
    fixture.chooseShortcuts(false, true);
    fixture.click(IdInstall);
    const fs::path marker = fixture.directory / L"current" / L"existing-data.txt";
    writeMarker(marker);
    testRegisteredInstallLocation = fixture.directory.wstring();
    fixture.waitFor(UiPage::Recovery);
    fixture.click(IdRetry);
    require(dispatchedRequests.size() == 2, "A partially registered install should retry directly");
    assertRequestPath(fixture.directory);
    require(dispatchedRequests.back().registeredInstall.has_value(),
            "Repair request must include freshly observed registration");
    require(markerPreserved(marker), "Repair must never require emptying the registered directory");
    fixture.waitFor(UiPage::Recovery);
    fixture.click(IdReviewOptions);
    require(!InstallerWindowTestAccess::hasEditablePath(fixture.window),
            "Registered repair should show its locked existing directory");
    fixture.expectShortcuts(false, true);
}

void changedRegistrationRequiresConfirmation(const fs::path& root) {
    for (const bool afterFailure : {false, true}) {
        WindowFixture fixture(root, afterFailure ? L"changed-after-failure" : L"changed-before-install");
        fixture.chooseShortcuts(false, true);
        if (afterFailure) {
            fixture.click(IdInstall);
            fixture.waitFor(UiPage::Recovery);
        }
        const fs::path other = fixture.base / L"registered-elsewhere";
        writeMarker(other / L"current" / L"keep.txt");
        testRegisteredInstallLocation = other.wstring();
        const size_t previousRequests = dispatchedRequests.size();
        fixture.click(afterFailure ? IdRetry : IdInstall);
        require(dispatchedRequests.size() == previousRequests,
                "Changed registered location must not be installed without confirmation");
        require(InstallerWindowTestAccess::page(fixture.window) == UiPage::Options
                    && samePath(InstallerWindowTestAccess::directory(fixture.window), other.wstring()),
                "Confirmation must show new registration, not stale edit-control contents");
        require(!InstallerWindowTestAccess::hasEditablePath(fixture.window),
                "Changed registered location must be displayed read-only");
        fixture.expectShortcuts(false, true);
        fixture.click(IdInstall);
        assertRequestPath(other);
        fixture.waitFor(UiPage::Recovery);
    }
}

void nonemptySelectionResolvesWithoutDestructiveChanges(const fs::path& root) {
    WindowFixture fixture(root, L"automatic-child");
    const fs::path original = fixture.directory / L"keep.txt";
    writeMarker(original);
    InstallerWindowTestAccess::setPage(fixture.window, UiPage::Options);
    const fs::path first = fixture.directory / L"Yanami";
    require(InstallerWindowTestAccess::valid(fixture.window), "Nonempty parent should offer an automatic child");
    require(InstallerWindowTestAccess::helper(fixture.window).find(first.wstring()) != std::wstring::npos,
            "Options must show the resolved installation directory before confirmation");
    require(!fs::exists(first), "Directory preview must not create the destination");
    fixture.click(IdInstall);
    assertRequestPath(first);
    fixture.waitFor(UiPage::Recovery);
    fixture.click(IdRetry);
    assertRequestPath(first);
    fixture.waitFor(UiPage::Recovery);
    const fs::path occupied = first / L"unrelated.txt";
    writeMarker(occupied);
    fixture.click(IdRetry);
    assertRequestPath(fixture.directory / L"Yanami (2)");
    fixture.waitFor(UiPage::Recovery);
    fixture.click(IdReviewOptions);
    require(samePath(InstallerWindowTestAccess::directory(fixture.window), fixture.directory.wstring()),
            "Review/retry should retain the selected parent, not recursively descend into Yanami");
    require(markerPreserved(original) && markerPreserved(occupied),
            "Automatic resolution must preserve every pre-existing file");
    require(!fs::exists(fixture.directory / L"Yanami (2)"),
            "Fake workflow must never perform the actual installation");
}

void emptySelectionIsUsedDirectly(const fs::path& root) {
    WindowFixture fixture(root, L"empty-selection");
    fs::create_directory(fixture.directory);
    fixture.click(IdInstall);
    assertRequestPath(fixture.directory);
    fixture.waitFor(UiPage::Recovery);
    require(fs::is_empty(fixture.directory), "Fake workflow must leave an empty destination empty");
}

void assertControlBounds(const InstallerWindow& window, const std::string& scenario) {
    RECT client{};
    require(GetClientRect(window.handle(), &client) != FALSE, "Could not read client bounds");
    struct ControlBounds { RECT bounds; int identifier; };
    std::vector<ControlBounds> controls;
    for (HWND child = GetWindow(window.handle(), GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if ((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        RECT bounds{};
        require(GetWindowRect(child, &bounds) != FALSE, "Could not read child bounds");
        MapWindowPoints(nullptr, window.handle(), reinterpret_cast<POINT*>(&bounds), 2);
        const int identifier = GetDlgCtrlID(child);
        const std::string context = scenario + " control " + std::to_string(identifier);
        require(bounds.right > bounds.left && bounds.bottom > bounds.top,
                context + " has empty bounds");
        require(bounds.left >= 0 && bounds.top >= 0
                    && bounds.right <= client.right && bounds.bottom <= client.bottom,
                context + " extends outside the client area");
        for (const auto& previous : controls) {
            RECT overlap{};
            require(IntersectRect(&overlap, &bounds, &previous.bounds) == FALSE,
                    context + " overlaps control " + std::to_string(previous.identifier));
        }
        controls.push_back({bounds, identifier});
    }
    require(controls.size() >= 4, scenario + " unexpectedly has too few controls");
}

void prepareScenario(WindowFixture& fixture, const std::wstring& scenario) {
    if (scenario == L"installing") {
        // This is the real progress page and its real timer, but deliberately
        // no beginInstall/click/worker dispatch. The destination stays empty.
        InstallerWindowTestAccess::setPage(fixture.window, UiPage::Installing);
    } else if (scenario == L"conflict") {
        writeMarker(fixture.directory / L"已有文件.txt");
        writeMarker(fixture.directory / L"Yanami" / L"另一个文件.txt");
        InstallerWindowTestAccess::setPage(fixture.window, UiPage::Options);
    } else if (scenario == L"repair") {
        writeMarker(fixture.directory / L"current" / L"existing-data.txt");
        InstallerWindowTestAccess::setPage(fixture.window, UiPage::Options);
    } else if (scenario == L"recovery" || scenario == L"complete-warning") {
        InstallOutcome outcome;
        outcome.success = scenario == L"complete-warning";
        outcome.shortcutsIncomplete = outcome.success;
        outcome.installDirectory = fixture.directory.wstring();
        outcome.launchTarget = (fixture.directory / L"current" / L"Yanami-never-exists.exe").wstring();
        outcome.error = outcome.success ? L"A shortcut could not be written."
                                        : L"Installation did not complete. The existing files were preserved.";
        outcome.preservedLog = fixtureLog;
        InstallerWindowTestAccess::deliver(fixture.window, std::move(outcome));
    }
}

void paintPendingRoot(HWND window) {
    if (GetUpdateRect(window, nullptr, FALSE)) {
        // Hidden fixtures cannot rely on a visible-window paint schedule.
        // Deliver the real WM_PAINT with its real BeginPaint update region.
        SendMessageW(window, WM_PAINT, 0, 0);
    }
}

void rootPaintDoesNotPreclearWithGdi(const fs::path& root) {
    WindowFixture fixture(root, L"single-root-paint");
    RootPaintAudit audit(fixture.window.handle());
    for (const UiPage page : {UiPage::Welcome, UiPage::Options, UiPage::Installing,
                              UiPage::Complete, UiPage::Recovery}) {
        InstallerWindowTestAccess::setPage(fixture.window, page);
        const unsigned previousPaints = WorkflowHarness::rootPaintCalls;
        // Send rather than mock WM_PAINT: Direct2D and all production paint
        // logic execute against a real, non-visible native fixture window.
        SendMessageW(fixture.window.handle(), WM_PAINT, 0, 0);
        require(WorkflowHarness::rootPaintCalls > previousPaints,
                "Paint regression must exercise the root's real BeginPaint");
        require(WorkflowHarness::directRootFillCalls == 0,
                "Normal WM_PAINT must not erase the front buffer with GDI FillRect before Direct2D");
        require(InstallerWindowTestAccess::renderHealthy(fixture.window),
                "A failed Direct2D paint must not satisfy the no-preclear regression");
    }
    require(dispatchedRequests.empty(), "Paint checks must never dispatch installation");
    require(WorkflowHarness::modalMessages.empty(), "Normal paint must not need a recovery modal");
}

void installingAnimationLifecycle(const fs::path& root) {
    WindowFixture fixture(root, L"animation-lifecycle");
    RootPaintAudit audit(fixture.window.handle());
    ProgressTimerProbe probe(fixture.window.handle());
    require(!InstallerWindowTestAccess::progressStartedAt(fixture.window),
            "Options must not initialize the progress animation clock");
    prepareScenario(fixture, L"installing");
    const auto firstStart = InstallerWindowTestAccess::progressStartedAt(fixture.window);
    require(firstStart.has_value(), "Entering Installing must initialize its animation clock");
    require(dispatchedRequests.empty() && !InstallerWindowTestAccess::installing(fixture.window),
            "Controlled Installing preview must not start an installation worker");
    paintPendingRoot(fixture.window.handle());
    const unsigned initialPaints = WorkflowHarness::rootPaintCalls;
    pumpUntil([&] {
        paintPendingRoot(fixture.window.handle());
        return probe.timerMessages >= 4;
    });
    require(probe.invalidatingTimerMessages >= 4,
            "Successive real progress timer messages must request fresh frames");
    const RECT expectedProgressBounds = InstallerWindowTestAccess::progressUpdateBounds(fixture.window);
    require(!WorkflowHarness::progressInvalidationErases
                && !WorkflowHarness::progressInvalidationCoversWholeWindow
                && EqualRect(&WorkflowHarness::lastProgressInvalidation, &expectedProgressBounds),
            "Progress timer must invalidate only its own bounds without requesting background erasure");
    require(WorkflowHarness::rootPaintCalls >= initialPaints + 2,
            "The Installing timer must drive repeated real root paints");
    require(WorkflowHarness::directRootFillCalls == 0,
            "Animated paints must not expose a GDI-cleared intermediate frame");
    require(InstallerWindowTestAccess::progressStartedAt(fixture.window) == firstStart,
            "Timer ticks must advance elapsed time, not replace its starting point");

    InstallerWindowTestAccess::setPage(fixture.window, UiPage::Installing);
    require(InstallerWindowTestAccess::progressStartedAt(fixture.window) == firstStart,
            "Rebuilding the Installing page must preserve its animation start");
    for (const UINT dpi : {96U, 144U, 192U}) {
        RECT suggested{};
        require(GetWindowRect(fixture.window.handle(), &suggested) != FALSE,
                "Could not read hidden animation window bounds");
        SendMessageW(fixture.window.handle(), WM_DPICHANGED,
                     MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        require(InstallerWindowTestAccess::progressStartedAt(fixture.window) == firstStart,
                "DPI redraw must not restart the progress animation");
        paintPendingRoot(fixture.window.handle());
    }
    SendMessageW(fixture.window.handle(), WM_SETTINGCHANGE, 0, 0);
    require(InstallerWindowTestAccess::progressStartedAt(fixture.window) == firstStart,
            "System-style redraw must not restart the progress animation");

    InstallerWindowTestAccess::setPage(fixture.window, UiPage::Options);
    require(!InstallerWindowTestAccess::progressStartedAt(fixture.window),
            "Leaving Installing must release its animation clock");
    paintPendingRoot(fixture.window.handle());
    // KillTimer does not remove an already-queued WM_TIMER. Drain that tail
    // first, then prove that the timer produces no further messages.
    pumpMessages();
    paintPendingRoot(fixture.window.handle());
    const unsigned stoppedMessages = probe.timerMessages;
    pumpFor(120);
    require(probe.timerMessages == stoppedMessages,
            "Progress timer must stop after leaving Installing");
    require(!GetUpdateRect(fixture.window.handle(), nullptr, FALSE),
            "Options must not retain an animation update region");
    const unsigned stoppedInvalidations = probe.invalidatingTimerMessages;
    SendMessageW(fixture.window.handle(), WM_TIMER, kProgressTimer, 0);
    require(probe.invalidatingTimerMessages == stoppedInvalidations
                && !InstallerWindowTestAccess::progressStartedAt(fixture.window),
            "A stale progress timer message must not restart or invalidate another page");

    prepareScenario(fixture, L"installing");
    const auto secondStart = InstallerWindowTestAccess::progressStartedAt(fixture.window);
    require(secondStart && *secondStart > *firstStart,
            "Re-entering Installing must initialize a new animation lifetime");
    paintPendingRoot(fixture.window.handle());
    const unsigned restartMessages = probe.timerMessages;
    pumpUntil([&] {
        paintPendingRoot(fixture.window.handle());
        return probe.timerMessages >= restartMessages + 3;
    });
    require(InstallerWindowTestAccess::progressStartedAt(fixture.window) == secondStart,
            "Restarted timer must preserve the new lifetime's start");
    require(WorkflowHarness::directRootFillCalls == 0
                && InstallerWindowTestAccess::renderHealthy(fixture.window)
                && WorkflowHarness::modalMessages.empty(),
            "Continuous and restarted animation must paint without preclear or render failures");
    require(dispatchedRequests.empty() && !fs::exists(fixture.directory),
            "Animation regression must leave the install destination untouched");
    DestroyWindow(fixture.window.handle());
}

class SuspendedFixtureSurface final {
public:
    SuspendedFixtureSurface(HWND window, bool minimize) : window_(window), minimized_(minimize) {
        require(GetWindowRect(window_, &original_) != FALSE, "Could not save fixture window size");
        originalStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
        if (minimized_) {
            // Only this newly-created fixture is touched. Do not activate it,
            // and immediately hide its minimized representation as well.
            ::ShowWindow(window_, SW_SHOWMINNOACTIVE);
            ::ShowWindow(window_, SW_HIDE);
        } else {
            // Captioned top-level windows enforce a minimum client size even
            // for SetWindowPos. Temporarily make only our hidden fixture a
            // borderless popup so the real Win32 client can reach zero size.
            SetWindowLongPtrW(window_, GWL_STYLE,
                (originalStyle_ & ~(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
                                    | WS_MAXIMIZEBOX | WS_THICKFRAME)) | WS_POPUP);
            require(SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE,
                    "Could not give the hidden fixture a zero-sized client");
        }
        RECT client{};
        require(GetClientRect(window_, &client) != FALSE, "Could not inspect suspended fixture client");
        std::cout << "Suspended fixture: iconic=" << IsIconic(window_)
                  << " client=" << client.right - client.left << 'x' << client.bottom - client.top
                  << " (the regression explicitly sends WM_PAINT)\n";
        require((IsIconic(window_) != FALSE) == minimized_, "Fixture must have the requested native minimized state");
        require(IsRectEmpty(&client) != FALSE, "Suspended fixture must have a genuinely empty native client area");
    }

    void restore() {
        if (minimized_) {
            ::ShowWindow(window_, SW_SHOWNOACTIVATE);
            ::ShowWindow(window_, SW_HIDE);
        } else {
            SetWindowLongPtrW(window_, GWL_STYLE, originalStyle_);
        }
        require(SetWindowPos(window_, nullptr, 0, 0,
                             original_.right - original_.left, original_.bottom - original_.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE,
                "Could not restore fixture window size");
        RECT client{};
        require(!IsIconic(window_) && GetClientRect(window_, &client) && !IsRectEmpty(&client),
                "Restored fixture must have a non-minimized drawable client");
    }

private:
    HWND window_;
    bool minimized_;
    LONG_PTR originalStyle_ = 0;
    RECT original_{};
};

void firstShowIsCenteredAndLaterMovesArePreserved(const fs::path& root) {
    WindowFixture fixture(root, L"initial-centered-placement");
    require(WorkflowHarness::firstShowPlacement.has_value(),
            "The root must request its first ShowWindow through the observed boundary");
    const auto first = *WorkflowHarness::firstShowPlacement;
    require(first.window == fixture.window.handle() && first.captured && !first.alreadyVisible,
            "The complete native root placement must be inspectable before it becomes visible");
    const LONG width = first.outer.right - first.outer.left;
    const LONG height = first.outer.bottom - first.outer.top;
    const LONG expectedLeft = first.workArea.left + std::max<LONG>(0,
        (first.workArea.right - first.workArea.left - width) / 2);
    const LONG expectedTop = first.workArea.top + std::max<LONG>(0,
        (first.workArea.bottom - first.workArea.top - height) / 2);
    std::cout << "Native first-show: dpi=" << first.dpi
              << " outer=[" << first.outer.left << ',' << first.outer.top << ','
              << first.outer.right << ',' << first.outer.bottom << ']'
              << " workarea=[" << first.workArea.left << ',' << first.workArea.top << ','
              << first.workArea.right << ',' << first.workArea.bottom << ']'
              << " centerDelta=" << first.outer.left - expectedLeft << ','
              << first.outer.top - expectedTop << '\n';
    require(first.outer.left == expectedLeft && first.outer.top == expectedTop,
            "The installer must already be centered in its monitor work area before its first ShowWindow");
    require(WorkflowHarness::startupCursorProbe.has_value(),
            "Startup placement must sample the pointer monitor before choosing its hidden window location");
    const auto cursor = *WorkflowHarness::startupCursorProbe;
    if (cursor.succeeded) {
        require(first.monitor == cursor.monitor,
                "The startup window must be centered on the monitor selected by the actual startup cursor sample");
    }
    const SIZE baseline = InstallerWindowTestAccess::baselineSize(first.dpi);
    require(first.dpi != 0 && InstallerWindowTestAccess::dpi(fixture.window) == first.dpi
                && first.dpi == GetDpiForWindow(fixture.window.handle())
                && width == baseline.cx && height == baseline.cy
                && first.client.right - first.client.left == MulDiv(kBaselineClientWidth, first.dpi, 96)
                && first.client.bottom - first.client.top == MulDiv(kBaselineClientHeight, first.dpi, 96),
            "First-show outer frame, client, and renderer must all use the selected monitor's actual DPI");
    require(InstallerWindowTestAccess::renderHealthy(fixture.window)
                && !InstallerWindowTestAccess::recoveryQueued(fixture.window),
            "The hidden monitor-probe size must not queue a rendering failure");

    // Move only this hidden fixture. Keep the new position in the same work
    // area so Windows' own cross-monitor DPI or off-screen recovery is not
    // mistaken for the installer forcing the user back to the center.
    const LONG freeX = first.workArea.right - first.workArea.left - width;
    const LONG freeY = first.workArea.bottom - first.workArea.top - height;
    const LONG left = first.outer.left + (freeX >= 2 ? std::min<LONG>(17, (freeX + 1) / 2) : 0);
    const LONG top = first.outer.top + (freeY >= 2 ? std::min<LONG>(23, (freeY + 1) / 2) : 0);
    require(SetWindowPos(fixture.window.handle(), nullptr, left, top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
            "Could not move the isolated hidden installer fixture");
    RECT moved{};
    require(GetWindowRect(fixture.window.handle(), &moved) != FALSE,
            "Could not inspect the moved fixture");
    if (freeX >= 2 || freeY >= 2) {
        require(moved.left != first.outer.left || moved.top != first.outer.top,
                "The user-movement regression must actually move away from the center");
    } else {
        std::cout << "Native work area has no spare axis; off-center movement is covered by geometry cases\n";
    }
    const auto expectPlacementPreserved = [&] {
        RECT actual{};
        require(GetWindowRect(fixture.window.handle(), &actual) && EqualRect(&actual, &moved),
                "Page changes, settings refresh, and minimize/restore must preserve the user's moved position");
    };
    for (const UiPage page : {UiPage::Welcome, UiPage::Options, UiPage::Installing, UiPage::Recovery}) {
        InstallerWindowTestAccess::setPage(fixture.window, page);
        pumpMessages();
        expectPlacementPreserved();
    }
    InstallerWindowTestAccess::setPage(fixture.window, UiPage::Options);
    SendMessageW(fixture.window.handle(), WM_SETTINGCHANGE, 0, 0);
    pumpMessages();
    expectPlacementPreserved();
    SuspendedFixtureSurface suspended(fixture.window.handle(), true);
    suspended.restore();
    pumpMessages();
    expectPlacementPreserved();
    require(InstallerWindowTestAccess::renderHealthy(fixture.window)
                && WorkflowHarness::modalMessages.empty() && WorkflowHarness::shellActions.empty()
                && dispatchedRequests.empty() && !fs::exists(fixture.directory),
            "Startup placement checks must not install, launch, or produce rendering errors");
}

void expectSuspendedPaintIsHarmless(WindowFixture& fixture) {
    SendMessageW(fixture.window.handle(), WM_PAINT, 0, 0);
    const bool healthyImmediatelyAfterPaint = InstallerWindowTestAccess::renderHealthy(fixture.window);
    if (!healthyImmediatelyAfterPaint) {
        std::cout << "Forced suspended WM_PAINT: HRESULT=0x" << std::hex
                  << static_cast<unsigned long>(InstallerWindowTestAccess::paintError(fixture.window))
                  << std::dec << " recoveryQueued=" << InstallerWindowTestAccess::recoveryQueued(fixture.window)
                  << " unavailable=" << InstallerWindowTestAccess::renderUnavailable(fixture.window)
                  << " modals=" << WorkflowHarness::modalMessages.size() << '\n';
        pumpMessages();
        std::cout << "After pumping actual queued recovery: windowAlive=" << IsWindow(fixture.window.handle())
                  << " unavailable=" << InstallerWindowTestAccess::renderUnavailable(fixture.window)
                  << " attempts=" << InstallerWindowTestAccess::recoveryAttempts(fixture.window)
                  << " modals=" << WorkflowHarness::modalMessages.size() << '\n';
        for (const auto& message : WorkflowHarness::modalMessages) {
            std::cout << "Suppressed modal text: " << wideToUtf8(message) << '\n';
        }
    }
    require(healthyImmediatelyAfterPaint,
            "A real minimized or zero-sized WM_PAINT must not be classified as a renderer failure");
    pumpMessages();
    require(IsWindow(fixture.window.handle()) && InstallerWindowTestAccess::renderHealthy(fixture.window)
                && !InstallerWindowTestAccess::recoveryQueued(fixture.window)
                && InstallerWindowTestAccess::recoveryAttempts(fixture.window) == 0,
            "Suspended recovery must consume its queued message without spending retries or closing setup");
    require(WorkflowHarness::modalMessages.empty() && WorkflowHarness::shellActions.empty(),
            "A suspended installer must not show a failure modal or launch an external action");
}

void minimizedAndZeroSizedOptionsRecover(const fs::path& root) {
    for (const bool minimize : {true, false}) {
        WindowFixture fixture(root, minimize ? L"minimized-options" : L"zero-sized-options");
        fixture.chooseShortcuts(false, true);
        RootPaintAudit audit(fixture.window.handle());
        {
            SuspendedFixtureSurface suspended(fixture.window.handle(), minimize);
            expectSuspendedPaintIsHarmless(fixture);
            require(InstallerWindowTestAccess::page(fixture.window) == UiPage::Options,
                    "Suspending Options must not change its page");
            const auto previousRedraws = WorkflowHarness::rootFullRedrawRequests;
            suspended.restore();
            require(WorkflowHarness::rootFullRedrawRequests > previousRedraws,
                    "Restoring Options must request a full repaint including its child controls");
        }
        paintPendingRoot(fixture.window.handle());
        fixture.expectShortcuts(false, true);
        require(samePath(InstallerWindowTestAccess::directory(fixture.window), fixture.directory.wstring()),
                "Minimize and restore must preserve the selected installation path");
        // A recovery queued before suspension must be consumed only once. It
        // must not poison either the minimized window or the restored page.
        InstallerWindowTestAccess::queueRecovery(fixture.window);
        require(InstallerWindowTestAccess::recoveryQueued(fixture.window), "Fixture recovery must actually be queued");
        SuspendedFixtureSurface suspended(fixture.window.handle(), minimize);
        expectSuspendedPaintIsHarmless(fixture);
        suspended.restore();
        SendMessageW(fixture.window.handle(), kRenderRecoveryMessage, 0, 0);
        paintPendingRoot(fixture.window.handle());
        require(InstallerWindowTestAccess::renderHealthy(fixture.window)
                    && InstallerWindowTestAccess::recoveryAttempts(fixture.window) == 0,
                "A stale consumed recovery must not fail the restored window");
        InstallerWindowTestAccess::queueRecovery(fixture.window);
        SuspendedFixtureSurface pendingAcrossRestore(fixture.window.handle(), minimize);
        require(InstallerWindowTestAccess::recoveryQueued(fixture.window),
                "Suspension must leave an already-posted recovery available for one consumption");
        pendingAcrossRestore.restore();
        pumpMessages();
        require(!InstallerWindowTestAccess::recoveryQueued(fixture.window)
                    && InstallerWindowTestAccess::renderHealthy(fixture.window)
                    && InstallerWindowTestAccess::recoveryAttempts(fixture.window) == 0,
                "A pre-minimize recovery delivered only after restore must not consume retries");
        require(dispatchedRequests.empty() && !fs::exists(fixture.directory),
                "Options lifecycle tests must never install or create the destination");
    }
}

void suspendedWorkerCompletionPreservesOutcome(const fs::path& root) {
    for (const bool minimize : {true, false}) {
        for (const bool success : {true, false}) {
            WindowFixture fixture(root, std::wstring(minimize ? L"minimized-worker-" : L"zero-sized-worker-")
                + (success ? L"success" : L"failure"));
            fixture.chooseShortcuts(false, true);
            holdInstallOutcomes = true;
            fixture.click(IdInstall);
            require(dispatchedRequests.size() == 1 && InstallerWindowTestAccess::installing(fixture.window),
                    "Fixture must hold one fake in-flight installation, never run its real backend");
            const auto epoch = InstallerWindowTestAccess::progressStartedAt(fixture.window);
            require(epoch.has_value(), "In-flight fixture should have an animation epoch");
            RootPaintAudit audit(fixture.window.handle());
            ProgressTimerProbe probe(fixture.window.handle());
            InstallerWindowTestAccess::queueRecovery(fixture.window);
            SuspendedFixtureSurface suspended(fixture.window.handle(), minimize);
            expectSuspendedPaintIsHarmless(fixture);
            const auto previousInvalidations = probe.invalidatingTimerMessages;
            SendMessageW(fixture.window.handle(), WM_TIMER, kProgressTimer, 0);
            pumpFor(60);
            require(probe.invalidatingTimerMessages == previousInvalidations,
                    "Suspended installation timers must not invalidate an absent surface");
            require(InstallerWindowTestAccess::installing(fixture.window)
                        && InstallerWindowTestAccess::page(fixture.window) == UiPage::Installing
                        && InstallerWindowTestAccess::progressStartedAt(fixture.window) == epoch,
                    "Suspension must preserve the in-flight lock, page, and elapsed-time epoch");
            suspended.restore();
            const auto restoredInvalidations = probe.invalidatingTimerMessages;
            pumpUntil([&] { return probe.invalidatingTimerMessages > restoredInvalidations; });
            require(InstallerWindowTestAccess::progressStartedAt(fixture.window) == epoch,
                    "Restored animation must resume with the same epoch");

            SuspendedFixtureSurface completing(fixture.window.handle(), minimize);
            InstallOutcome outcome;
            outcome.success = success;
            outcome.installDirectory = fixture.directory.wstring();
            outcome.launchTarget = (fixture.directory / L"current" / L"Yanami-never-exists.exe").wstring();
            outcome.preservedLog = fixtureLog;
            if (!success) outcome.error = L"Isolated simulated backend failure.";
            InstallerWindowTestAccess::deliver(fixture.window, std::move(outcome));
            expectSuspendedPaintIsHarmless(fixture);
            const UiPage expectedPage = success ? UiPage::Complete : UiPage::Recovery;
            require(!InstallerWindowTestAccess::installing(fixture.window)
                        && InstallerWindowTestAccess::page(fixture.window) == expectedPage,
                    "A suspended worker result must reach the normal Complete or Recovery page");
            RECT completedClient{};
            require(GetClientRect(fixture.window.handle(), &completedClient)
                        && IsRectEmpty(&completedClient)
                        && (IsIconic(fixture.window.handle()) != FALSE) == minimize
                        && !InstallerWindowTestAccess::progressStartedAt(fixture.window),
                    "Worker completion must retain suspension without restarting the animation clock");
            const auto completionInvalidations = probe.invalidatingTimerMessages;
            SendMessageW(fixture.window.handle(), WM_TIMER, kProgressTimer, 0);
            require(probe.invalidatingTimerMessages == completionInvalidations,
                    "A completed suspended installation must not restart its timer redraws");
            completing.restore();
            paintPendingRoot(fixture.window.handle());
            require(IsWindow(fixture.window.handle()) && InstallerWindowTestAccess::renderHealthy(fixture.window),
                    "Worker completion while suspended must not close or poison the restored window");
            if (success) {
                require(IsWindowEnabled(GetDlgItem(fixture.window.handle(), IdFinish)),
                        "A successful suspended installation must retain its explicit launch action");
            } else {
                fixture.click(IdReviewOptions);
                fixture.expectShortcuts(false, true);
            }
            require(dispatchedRequests.size() == 1 && !fs::exists(fixture.directory)
                        && WorkflowHarness::modalMessages.empty() && WorkflowHarness::shellActions.empty(),
                    "Suspension must not restart installation, touch the destination, or open an external action");
        }
    }
}

void emptyBoundsDifferFromInvalidDrawing(const fs::path& root) {
    WindowFixture fixture(root, L"empty-bounds-versus-invalid-dc");
    for (const RECT bounds : {RECT{0, 0, 0, 40}, RECT{0, 0, 40, 0}, RECT{20, 20, 10, 10}}) {
        require(!InstallerWindowTestAccess::beginDrawing(fixture.window, nullptr, bounds),
                "Empty UI bounds should skip a drawing attempt");
        require(InstallerWindowTestAccess::renderHealthy(fixture.window)
                    && !InstallerWindowTestAccess::recoveryQueued(fixture.window),
                "Temporarily empty UI bounds must not enqueue renderer recovery");
    }
    require(!InstallerWindowTestAccess::beginDrawing(fixture.window, nullptr, {0, 0, 40, 40}),
            "A null DC for a real nonempty drawing attempt must still fail");
    require(!InstallerWindowTestAccess::renderHealthy(fixture.window)
                && InstallerWindowTestAccess::recoveryQueued(fixture.window),
            "The minimized-window guard must not suppress genuine rendering errors");
    pumpMessages();
    paintPendingRoot(fixture.window.handle());
    require(IsWindow(fixture.window.handle()) && InstallerWindowTestAccess::renderHealthy(fixture.window),
            "A subsequent valid drawing attempt should recover normally");
}

void layoutsAtMultipleDpis(const fs::path& root) {
    for (const UiLanguage language : {UiLanguage::English, UiLanguage::SimplifiedChinese}) {
        for (const std::wstring scenario : {L"options", L"conflict", L"recovery", L"complete-warning", L"repair"}) {
            const std::wstring name = L"layout-" + scenario
                + (language == UiLanguage::English ? L"-en" : L"-zh");
            WindowFixture fixture(root, name, scenario == L"repair", language);
            prepareScenario(fixture, scenario);
            for (const UINT dpi : {96U, 144U, 192U}) {
                RECT suggested{};
                GetWindowRect(fixture.window.handle(), &suggested);
                SendMessageW(fixture.window.handle(), WM_DPICHANGED,
                             MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
                assertControlBounds(fixture.window, wideToUtf8(name) + " @" + std::to_string(dpi));
            }
            require(WorkflowHarness::modalMessages.empty(), "Layout should not need modal rendering fallback");
        }
    }
}

int runPreview(const fs::path& root, const std::wstring& scenario) {
    require(scenario == L"options" || scenario == L"conflict" || scenario == L"recovery"
                || scenario == L"complete-warning" || scenario == L"repair" || scenario == L"installing",
            "Usage: yanami-installer-workflow-tests --preview options|conflict|recovery|complete-warning|repair|installing");
    WorkflowHarness::preview = true;
    WindowFixture fixture(root, L"preview-" + scenario, scenario == L"repair");
    ProgressTimerProbe previewTrace(fixture.window.handle());
    prepareScenario(fixture, scenario);
    SetWindowTextW(fixture.window.handle(), L"Yanami 安装程序 · 安全预览（不会安装）");
    // STARTUPINFO can hide the first ShowWindow when the console is launched
    // hidden. Show only the preview HWND explicitly after that first call.
    ::ShowWindow(fixture.window.handle(), SW_SHOWNORMAL);
    UpdateWindow(fixture.window.handle());
    std::cout << "Isolated preview: " << wideToUtf8(fixture.base.wstring())
              << "\nInstallation, registration, shortcuts and external launches are disabled.\n"
              << "Press Escape to close the preview.\n";
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            // Preview lifetime is independent of the production install lock.
            // No worker exists, so it is always safe to close this fixture.
            DestroyWindow(fixture.window.handle());
            continue;
        }
        if (!fixture.window.preprocessKeyboardMessage(message)
            && !IsDialogMessageW(fixture.window.handle(), &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    std::cout << "Preview ended: page=" << static_cast<int>(InstallerWindowTestAccess::page(fixture.window))
              << " installing=" << InstallerWindowTestAccess::installing(fixture.window)
              << " renderUnavailable=" << InstallerWindowTestAccess::renderUnavailable(fixture.window)
              << " modals=" << WorkflowHarness::modalMessages.size() << std::endl;
    return 0;
}
} // namespace

int main() {
    enablePerMonitorDpi();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitializeCom = SUCCEEDED(com);
    int result = 0;
    try {
        const fs::path root = makeSuiteRoot();
        const auto arguments = commandLineArguments();
        if (!arguments.empty()) {
            require(arguments.size() == 2 && arguments.front() == L"--preview",
                    "Expected --preview followed by a scenario name");
            result = runPreview(root, arguments.back());
        } else {
            const std::vector<std::pair<const char*, void (*)(const fs::path&)>> tests{
                {"native first-show centered and later user moves preserved", firstShowIsCenteredAndLaterMovesArePreserved},
                {"startup placement across monitors, work areas, and DPI", centeredPlacementGeometry},
                {"success with shortcut warning", successWithShortcutWarning},
                {"nonblocking recovery and shortcut choices", failureOffersRecoveryAndPreservesChoices},
                {"newly registered installation repairs in place", newlyRegisteredInstallRepairsInPlace},
                {"changed registration requires confirmation", changedRegistrationRequiresConfirmation},
                {"nonempty selected directory resolves safely", nonemptySelectionResolvesWithoutDestructiveChanges},
                {"empty selected directory is used directly", emptySelectionIsUsedDirectly},
                {"real WM_PAINT has no GDI foreground preclear", rootPaintDoesNotPreclearWithGdi},
                {"continuous progress timer and animation lifetime", installingAnimationLifecycle},
                {"native minimized and zero-sized Options recover", minimizedAndZeroSizedOptionsRecover},
                {"suspended fake worker completion preserves outcome", suspendedWorkerCompletionPreservesOutcome},
                {"empty UI bounds differ from genuinely invalid drawing", emptyBoundsDifferFromInvalidDrawing},
                {"native control bounds at 96/144/192 DPI", layoutsAtMultipleDpis},
            };
            for (const auto& [name, test] : tests) {
                test(root);
                std::cout << "PASS: " << name << '\n';
            }
            std::cout << "All installer workflow tests passed. Fixtures retained: "
                      << wideToUtf8(root.wstring()) << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    if (uninitializeCom) {
        CoUninitialize();
    }
    return result;
}
