#include "BootstrapProtocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef YANAMI_BOOTSTRAP_HAS_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace {

using namespace std::chrono_literals;

constexpr char desktopExecutableName[] = "yanami-desktop";
constexpr char readyFileName[] = "desktop-ready.json";

std::filesystem::path executableDirectory()
{
    std::vector<char> buffer(4096, '\0');
    const ssize_t length = readlink(
        "/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0)
        return std::filesystem::current_path();
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer.data(), buffer.data() + buffer.size())
        .parent_path();
}

int acquireInstanceLock()
{
    const char *runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path base = runtimeDirectory && *runtimeDirectory
        ? std::filesystem::path(runtimeDirectory)
        : std::filesystem::temp_directory_path();
    const std::filesystem::path lockPath = base
        / ("yanami-bootstrap-" + std::to_string(getuid()) + ".lock");
    const int descriptor = open(
        lockPath.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
        return -1;
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor);
        return -2;
    }
    const int flags = fcntl(descriptor, F_GETFD);
    if (flags >= 0)
        fcntl(descriptor, F_SETFD, flags & ~FD_CLOEXEC);
    return descriptor;
}

std::optional<std::filesystem::path> createPrivateReadyDirectory()
{
    std::filesystem::path base = std::filesystem::temp_directory_path();
    std::string pattern = (base / "YanamiBootstrap-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char *created = mkdtemp(buffer.data());
    if (!created)
        return std::nullopt;
    chmod(created, S_IRWXU);
    return std::filesystem::path(created);
}

void cleanupReadyDirectory(const std::filesystem::path &directory)
{
    std::error_code ignored;
    std::filesystem::remove(directory / readyFileName, ignored);
    std::filesystem::remove(directory, ignored);
}

std::vector<char *> argumentPointers(std::vector<std::string> &arguments)
{
    std::vector<char *> pointers;
    pointers.reserve(arguments.size() + 1);
    for (std::string &argument : arguments)
        pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return pointers;
}

[[noreturn]] void execDesktop(
    const std::filesystem::path &desktopPath,
    std::vector<std::string> arguments)
{
    arguments.insert(arguments.begin(), desktopPath.string());
    std::vector<char *> pointers = argumentPointers(arguments);
    execv(desktopPath.c_str(), pointers.data());
    std::cerr << "Yanami Desktop could not be started: "
              << std::strerror(errno) << '\n';
    _exit(static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed));
}

int childExitCode(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
}

#ifdef YANAMI_BOOTSTRAP_HAS_X11

struct X11Splash {
    Display *display = nullptr;
    Window window = 0;
    Atom deleteMessage = None;
    Atom opacityAtom = None;
    GC graphics = nullptr;
    unsigned long backgroundPixel = 0;
    unsigned long borderPixel = 0;
    unsigned long bluePixel = 0;
    unsigned long purplePixel = 0;
    unsigned long textPixel = 0;
    unsigned long mutedTextPixel = 0;
    bool cancelRequested = false;
    std::string status = "Starting desktop services...";
    std::chrono::steady_clock::time_point epoch =
        std::chrono::steady_clock::now();
};

unsigned long colorPixel(Display *display, Colormap colormap, const char *name)
{
    XColor exact {};
    XColor screen {};
    if (XAllocNamedColor(display, colormap, name, &screen, &exact))
        return screen.pixel;
    return BlackPixel(display, DefaultScreen(display));
}

std::optional<X11Splash> createX11Splash()
{
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        return std::nullopt;
    const int screen = DefaultScreen(display);
    const int width = 540;
    const int height = 320;
    const int x = (DisplayWidth(display, screen) - width) / 2;
    const int y = (DisplayHeight(display, screen) - height) / 2;
    const Colormap colormap = DefaultColormap(display, screen);
    const unsigned long background = colorPixel(display, colormap, "#080d17");
    const unsigned long border = colorPixel(display, colormap, "#27334e");
    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), x, y, width, height, 1,
        border, background);
    if (!window) {
        XCloseDisplay(display);
        return std::nullopt;
    }
    XStoreName(display, window, "Yanami is starting");
    XSelectInput(display, window,
                 ExposureMask | StructureNotifyMask);
    Atom deleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &deleteMessage, 1);
    const long processId = static_cast<long>(getpid());
    Atom pidAtom = XInternAtom(display, "_NET_WM_PID", False);
    XChangeProperty(display, window, pidAtom, XA_CARDINAL, 32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&processId), 1);
    XMapRaised(display, window);
    XFlush(display);
    X11Splash splash;
    splash.display = display;
    splash.window = window;
    splash.deleteMessage = deleteMessage;
    splash.opacityAtom = XInternAtom(
        display, "_NET_WM_WINDOW_OPACITY", False);
    splash.graphics = XCreateGC(display, window, 0, nullptr);
    splash.backgroundPixel = background;
    splash.borderPixel = border;
    splash.bluePixel = colorPixel(display, colormap, "#4f8fff");
    splash.purplePixel = colorPixel(display, colormap, "#b25cff");
    splash.textPixel = colorPixel(display, colormap, "#eff5ff");
    splash.mutedTextPixel = colorPixel(display, colormap, "#97a6c2");
    return splash;
}

void drawX11Splash(X11Splash &splash)
{
    Display *display = splash.display;
    XSetForeground(display, splash.graphics, splash.backgroundPixel);
    XFillRectangle(display, splash.window, splash.graphics, 0, 0, 540, 320);

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - splash.epoch).count();
    const int pulse = static_cast<int>((std::sin(elapsed * 3.2) + 1.0) * 2.0);
    XSetLineAttributes(display, splash.graphics, 16,
                       LineSolid, CapRound, JoinRound);
    XSetForeground(display, splash.graphics, splash.bluePixel);
    XDrawArc(display, splash.window, splash.graphics,
             218 - pulse, 42 - pulse, 104 + pulse * 2, 104 + pulse * 2,
             25 * 64, 286 * 64);
    XSetForeground(display, splash.graphics, splash.purplePixel);
    XDrawArc(display, splash.window, splash.graphics,
             224 - pulse, 48 - pulse, 92 + pulse * 2, 92 + pulse * 2,
             205 * 64, 130 * 64);
    XPoint play[] {{260, 72}, {260, 119}, {298, 95}};
    XFillPolygon(display, splash.window, splash.graphics,
                 play, 3, Convex, CoordModeOrigin);

    XSetLineAttributes(display, splash.graphics, 3,
                       LineSolid, CapRound, JoinRound);
    const int spinnerStart = static_cast<int>(elapsed * 110.0) % 360;
    XDrawArc(display, splash.window, splash.graphics,
             202, 27, 136, 136, spinnerStart * 64, 74 * 64);

    XSetForeground(display, splash.graphics, splash.textPixel);
    XDrawString(display, splash.window, splash.graphics,
                247, 211, "Yanami", 6);
    XSetForeground(display, splash.graphics, splash.mutedTextPixel);
    const int statusX = std::max(24, 270 - static_cast<int>(splash.status.size()) * 3);
    XDrawString(display, splash.window, splash.graphics,
                statusX, 245, splash.status.c_str(),
                static_cast<int>(splash.status.size()));

    XFlush(display);
}

void pumpX11(X11Splash &splash)
{
    while (XPending(splash.display)) {
        XEvent event {};
        XNextEvent(splash.display, &event);
        if (event.type == ClientMessage
            && static_cast<Atom>(event.xclient.data.l[0])
                == splash.deleteMessage) {
            splash.cancelRequested = true;
        }
    }
    drawX11Splash(splash);
}

Window findWindowForPid(Display *display, Window root, pid_t processId)
{
    Atom pidAtom = XInternAtom(display, "_NET_WM_PID", False);
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char *property = nullptr;
    if (XGetWindowProperty(display, root, pidAtom, 0, 1, False, XA_CARDINAL,
                           &actualType, &actualFormat, &itemCount, &bytesAfter,
                           &property) == Success
        && property && itemCount == 1
        && *reinterpret_cast<unsigned long *>(property)
            == static_cast<unsigned long>(processId)) {
        XFree(property);
        return root;
    }
    if (property)
        XFree(property);
    Window rootReturn = 0;
    Window parentReturn = 0;
    Window *children = nullptr;
    unsigned int childCount = 0;
    if (!XQueryTree(display, root, &rootReturn, &parentReturn,
                    &children, &childCount)) {
        return 0;
    }
    Window result = 0;
    for (unsigned int index = 0; index < childCount && !result; ++index)
        result = findWindowForPid(display, children[index], processId);
    if (children)
        XFree(children);
    return result;
}

void activateDesktopWindow(X11Splash &splash, pid_t child)
{
    Window target = findWindowForPid(
        splash.display, DefaultRootWindow(splash.display), child);
    if (!target)
        return;
    Atom activeWindow = XInternAtom(
        splash.display, "_NET_ACTIVE_WINDOW", False);
    XEvent event {};
    event.xclient.type = ClientMessage;
    event.xclient.window = target;
    event.xclient.message_type = activeWindow;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(splash.display, DefaultRootWindow(splash.display), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &event);
    XRaiseWindow(splash.display, target);
    XFlush(splash.display);
}

void fadeAndClose(X11Splash &splash)
{
    constexpr int steps = 11;
    for (int step = 0; step <= steps; ++step) {
        const double progress = static_cast<double>(step) / steps;
        const double remaining = 1.0 - progress;
        const unsigned long opacity = static_cast<unsigned long>(
            std::llround(0xffffffffULL
                * remaining * remaining * remaining));
        XChangeProperty(
            splash.display, splash.window, splash.opacityAtom, XA_CARDINAL,
            32, PropModeReplace,
            reinterpret_cast<const unsigned char *>(&opacity), 1);
        XFlush(splash.display);
        if (step < steps)
            std::this_thread::sleep_for(20ms);
    }
    XUnmapWindow(splash.display, splash.window);
    XFlush(splash.display);
}

void destroyX11Splash(X11Splash &splash)
{
    if (splash.graphics)
        XFreeGC(splash.display, splash.graphics);
    if (splash.window)
        XDestroyWindow(splash.display, splash.window);
    XCloseDisplay(splash.display);
}

#endif

} // namespace

int main(int argc, char *argv[])
{
    std::vector<std::string> rawArguments;
    for (int index = 1; index < argc; ++index)
        rawArguments.emplace_back(argv[index]);
    const YanamiBootstrap::LauncherOptions options =
        YanamiBootstrap::parseLauncherOptions(rawArguments);
    std::vector<std::string> forwarded;
    for (const std::string &argument : rawArguments) {
        if (!argument.starts_with(YanamiBootstrap::timeoutArgumentPrefix))
            forwarded.push_back(argument);
    }

    const std::filesystem::path desktopPath =
        executableDirectory() / desktopExecutableName;
    if (!std::filesystem::is_regular_file(desktopPath)) {
        std::cerr << "Yanami Desktop is missing. Reinstall Yanami.\n";
        return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
    }
    const int instanceLock = acquireInstanceLock();
    if (instanceLock == -2)
        return 0;
    if (instanceLock < 0)
        return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);

#ifdef YANAMI_BOOTSTRAP_HAS_X11
    std::optional<X11Splash> splash = createX11Splash();
    if (!splash) {
        // Native Wayland has no stable zero-dependency surface protocol. Keep
        // the launcher Qt-free and transparently replace it with the desktop;
        // the inherited lock descriptor preserves single-instance ownership.
        execDesktop(desktopPath, forwarded);
    }
#else
    execDesktop(desktopPath, forwarded);
#endif

#ifdef YANAMI_BOOTSTRAP_HAS_X11
    YanamiBootstrap::TraceWriter trace(
        options.performanceTracePath,
        static_cast<std::uint64_t>(getpid()));
    trace.mark("bootstrap_entered", false, std::nullopt, "indeterminate");
    trace.mark("bootstrap_first_visible", false, std::nullopt, "indeterminate");

    const auto readyDirectory = createPrivateReadyDirectory();
    if (!readyDirectory) {
        destroyX11Splash(*splash);
        return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
    }
    const std::filesystem::path readyFile = *readyDirectory / readyFileName;
    forwarded.push_back(YanamiBootstrap::readyFileArgument(readyFile));

    const pid_t child = fork();
    if (child < 0) {
        cleanupReadyDirectory(*readyDirectory);
        destroyX11Splash(*splash);
        return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
    }
    if (child == 0)
        execDesktop(desktopPath, forwarded);

    trace.mark("bootstrap_desktop_spawned", false,
               static_cast<std::uint64_t>(child), "indeterminate");
    auto deadline = std::chrono::steady_clock::now() + options.readyTimeout;
    bool ready = false;
    int result = 0;
    while (true) {
        pumpX11(*splash);
        if (splash->cancelRequested) {
            kill(child, SIGTERM);
            std::this_thread::sleep_for(500ms);
            if (waitpid(child, nullptr, WNOHANG) == 0)
                kill(child, SIGKILL);
            result = static_cast<int>(YanamiBootstrap::ExitCode::Cancelled);
            break;
        }

        if (!ready && std::filesystem::exists(readyFile)) {
            std::string error;
            if (!YanamiBootstrap::validateReadyFile(
                    readyFile, static_cast<std::uint64_t>(child), error)) {
                kill(child, SIGTERM);
                result = static_cast<int>(
                    YanamiBootstrap::ExitCode::InvalidReadyPayload);
                break;
            }
            ready = true;
            trace.mark("desktop_ready", true,
                       static_cast<std::uint64_t>(child));
            activateDesktopWindow(*splash, child);
            fadeAndClose(*splash);
            trace.mark("handoff_complete", true,
                       static_cast<std::uint64_t>(child));
            if (!options.waitForDesktopExit()) {
                result = 0;
                break;
            }
        }

        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) {
            result = childExitCode(status);
            if (!ready && !options.waitForDesktopExit()) {
                std::cerr << "Yanami Desktop exited before its first window was ready.\n";
                result = static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
            }
            break;
        }

        if (!ready && std::chrono::steady_clock::now() >= deadline) {
            splash->status = "Yanami is taking longer than expected...";
            if (options.waitForDesktopExit()) {
                kill(child, SIGTERM);
                result = static_cast<int>(YanamiBootstrap::ExitCode::ReadyTimeout);
                break;
            }
            deadline = std::chrono::steady_clock::now() + options.readyTimeout;
        }
        std::this_thread::sleep_for(16ms);
    }

    cleanupReadyDirectory(*readyDirectory);
    destroyX11Splash(*splash);
    close(instanceLock);
    return result;
#endif
}
