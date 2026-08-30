#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "BootstrapProtocol.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {

using namespace std::chrono_literals;

constexpr char desktopExecutableName[] = "Yanami";
constexpr char readyFileName[] = "desktop-ready.json";

} // namespace

@interface YanamiBootstrapController : NSObject <NSApplicationDelegate>
@property(nonatomic, assign) BOOL cancelRequested;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *statusLabel;
@end

@implementation YanamiBootstrapController
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    self.cancelRequested = YES;
    self.statusLabel.stringValue = @"Cancelling safely…";
    // The polling loop owns child-process cleanup. Cancel AppKit termination
    // now so Cmd-Q and Dock Quit take the same graceful path as other systems.
    return NSTerminateCancel;
}
@end

namespace {

std::filesystem::path executableDirectory()
{
    NSString *absolute = NSBundle.mainBundle.executablePath;
    if (!absolute.length)
        absolute = NSProcessInfo.processInfo.arguments.firstObject;
    if (!absolute.isAbsolutePath) {
        absolute = [NSFileManager.defaultManager.currentDirectoryPath
            stringByAppendingPathComponent:absolute];
    }
    absolute = absolute.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    return std::filesystem::path(
        absolute.stringByDeletingLastPathComponent.fileSystemRepresentation);
}

std::optional<std::filesystem::path> createPrivateReadyDirectory()
{
    NSString *temporary = NSTemporaryDirectory();
    std::string pattern = std::string(temporary.fileSystemRepresentation)
        + "/YanamiBootstrap-XXXXXX";
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

int acquireInstanceLock()
{
    const std::string lockPath = std::string(NSTemporaryDirectory().fileSystemRepresentation)
        + "/YanamiBootstrap-" + std::to_string(getuid()) + ".lock";
    const int descriptor = open(lockPath.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
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

void activateExistingApplication()
{
    NSArray<NSRunningApplication *> *applications =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:
            @"io.github.TwooSix.Yanami"];
    for (NSRunningApplication *application in applications) {
        if (application.processIdentifier == getpid())
            continue;
        [application activateWithOptions:NSApplicationActivateIgnoringOtherApps];
        break;
    }
}

YanamiBootstrapController *createSplash()
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    auto *controller = [[YanamiBootstrapController alloc] init];
    const NSRect frame = NSMakeRect(0, 0, 540, 320);
    controller.window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered
        defer:NO];
    controller.window.opaque = YES;
    controller.window.backgroundColor =
        [NSColor colorWithCalibratedRed:8.0 / 255.0
                                  green:13.0 / 255.0
                                   blue:23.0 / 255.0
                                  alpha:1.0];
    controller.window.hasShadow = YES;
    controller.window.movableByWindowBackground = YES;
    controller.window.contentView.wantsLayer = YES;
    controller.window.contentView.layer.cornerRadius = 24.0;
    controller.window.contentView.layer.cornerCurve = kCACornerCurveContinuous;
    controller.window.contentView.layer.masksToBounds = YES;

    NSString *logoPath = [[NSBundle mainBundle]
        pathForResource:@"yanami-logo" ofType:@"png"];
    NSImage *logo = logoPath ? [[NSImage alloc] initWithContentsOfFile:logoPath]
                             : NSApp.applicationIconImage;
    NSImageView *imageView = [[NSImageView alloc]
        initWithFrame:NSMakeRect(214, 170, 112, 112)];
    imageView.image = logo;
    imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
    imageView.wantsLayer = YES;
    imageView.layer.cornerRadius = 20.0;
    imageView.layer.borderWidth = 1.0;
    imageView.layer.borderColor =
        [NSColor colorWithCalibratedRed:27.0 / 255.0
                                  green:37.0 / 255.0
                                   blue:52.0 / 255.0
                                  alpha:1.0].CGColor;
    [controller.window.contentView addSubview:imageView];

    NSProgressIndicator *spinner = [[NSProgressIndicator alloc]
        initWithFrame:NSMakeRect(260, 38, 20, 20)];
    spinner.style = NSProgressIndicatorStyleSpinning;
    spinner.indeterminate = YES;
    [spinner startAnimation:nil];
    [controller.window.contentView addSubview:spinner];

    NSTextField *title = [NSTextField labelWithString:@"Yanami"];
    title.frame = NSMakeRect(20, 112, 500, 40);
    title.alignment = NSTextAlignmentCenter;
    title.font = [NSFont systemFontOfSize:30 weight:NSFontWeightSemibold];
    title.textColor =
        [NSColor colorWithCalibratedRed:245.0 / 255.0
                                  green:247.0 / 255.0
                                   blue:250.0 / 255.0
                                  alpha:1.0];
    [controller.window.contentView addSubview:title];

    controller.statusLabel = [NSTextField labelWithString:@"Starting Yanami…"];
    controller.statusLabel.frame = NSMakeRect(30, 76, 480, 28);
    controller.statusLabel.alignment = NSTextAlignmentCenter;
    controller.statusLabel.font = [NSFont systemFontOfSize:16];
    controller.statusLabel.textColor =
        [NSColor colorWithCalibratedRed:154.0 / 255.0
                                  green:163.0 / 255.0
                                   blue:178.0 / 255.0
                                  alpha:1.0];
    [controller.window.contentView addSubview:controller.statusLabel];

    [controller.window center];
    [controller.window makeKeyAndOrderFront:nil];
    NSApp.delegate = controller;
    [NSApp activateIgnoringOtherApps:YES];
    return controller;
}

void pumpAppKitUntil(std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() < deadline) {
        @autoreleasepool {
            NSEvent *event = [NSApp
                nextEventMatchingMask:NSEventMaskAny
                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                inMode:NSDefaultRunLoopMode
                dequeue:YES];
            if (event)
                [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    }
}

bool cancelChild(pid_t child, YanamiBootstrapController *controller)
{
    kill(child, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + 2500ms;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child)
            return true;
        pumpAppKitUntil(std::chrono::steady_clock::now() + 20ms);
    }
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"Yanami could not close gracefully";
    alert.informativeText =
        @"Force stop the startup process? No user library data will be removed.";
    [alert addButtonWithTitle:@"Force Stop"];
    [alert addButtonWithTitle:@"Keep Waiting"];
    if ([alert runModal] != NSAlertFirstButtonReturn) {
        controller.cancelRequested = NO;
        controller.statusLabel.stringValue = @"Continuing to start Yanami…";
        return false;
    }
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    return true;
}

int childExitCode(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
}

} // namespace

int main(int argc, char *argv[])
{
    @autoreleasepool {
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index)
            arguments.emplace_back(argv[index]);
        const YanamiBootstrap::LauncherOptions options =
            YanamiBootstrap::parseLauncherOptions(arguments);
        std::vector<std::string> forwarded;
        for (const std::string &argument : arguments) {
            if (!argument.starts_with(YanamiBootstrap::timeoutArgumentPrefix))
                forwarded.push_back(argument);
        }

        const std::filesystem::path desktopPath =
            executableDirectory() / desktopExecutableName;
        if (!std::filesystem::is_regular_file(desktopPath)) {
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"Yanami installation is incomplete";
            alert.informativeText =
                @"Yanami Desktop is missing. Reinstall Yanami and try again.";
            [alert runModal];
            return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
        }

        const int instanceLock = acquireInstanceLock();
        if (instanceLock == -2) {
            activateExistingApplication();
            return 0;
        }
        if (instanceLock < 0)
            return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);

        YanamiBootstrap::TraceWriter trace(
            options.performanceTracePath,
            static_cast<std::uint64_t>(getpid()));
        trace.mark("bootstrap_entered", false, std::nullopt, "indeterminate");
        YanamiBootstrapController *controller = createSplash();
        trace.mark(
            "bootstrap_first_visible", false, std::nullopt, "indeterminate");

        const auto readyDirectory = createPrivateReadyDirectory();
        if (!readyDirectory) {
            close(instanceLock);
            return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
        }
        const std::filesystem::path readyFile = *readyDirectory / readyFileName;
        forwarded.push_back(YanamiBootstrap::readyFileArgument(readyFile));

        std::vector<char *> childArguments;
        std::string desktopEncoded = desktopPath.string();
        childArguments.push_back(desktopEncoded.data());
        for (std::string &argument : forwarded)
            childArguments.push_back(argument.data());
        childArguments.push_back(nullptr);

        pid_t child = 0;
        const int spawnResult = posix_spawn(
            &child, desktopPath.c_str(), nullptr, nullptr,
            childArguments.data(), environ);
        if (spawnResult != 0) {
            cleanupReadyDirectory(*readyDirectory);
            close(instanceLock);
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"Yanami Desktop could not be started";
            alert.informativeText = @"Retry after reinstalling the application.";
            [alert runModal];
            return static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
        }
        trace.mark("bootstrap_desktop_spawned", false,
                   static_cast<std::uint64_t>(child), "indeterminate");
        controller.statusLabel.stringValue = @"Starting Yanami…";

        auto deadline = std::chrono::steady_clock::now() + options.readyTimeout;
        bool ready = false;
        bool handedOff = false;
        int result = 0;
        while (true) {
            pumpAppKitUntil(std::chrono::steady_clock::now() + 16ms);
            if (controller.cancelRequested) {
                if (cancelChild(child, controller)) {
                    result = static_cast<int>(YanamiBootstrap::ExitCode::Cancelled);
                    break;
                }
                deadline = std::chrono::steady_clock::now() + options.readyTimeout;
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
                NSRunningApplication *application =
                    [NSRunningApplication runningApplicationWithProcessIdentifier:child];
                [application activateWithOptions:NSApplicationActivateIgnoringOtherApps];
                [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
                    context.duration = 0.22;
                    context.timingFunction = [CAMediaTimingFunction
                        functionWithName:kCAMediaTimingFunctionEaseOut];
                    controller.window.animator.alphaValue = 0.0;
                } completionHandler:^{}];
                pumpAppKitUntil(std::chrono::steady_clock::now() + 220ms);
                [controller.window orderOut:nil];
                handedOff = true;
                trace.mark("handoff_complete", true,
                           static_cast<std::uint64_t>(child));
                if (!options.waitForDesktopExit()) {
                    result = 0;
                    break;
                }
            }

            int status = 0;
            const pid_t waitResult = waitpid(child, &status, WNOHANG);
            if (waitResult == child) {
                result = childExitCode(status);
                if (!ready && !options.waitForDesktopExit()) {
                    NSAlert *alert = [[NSAlert alloc] init];
                    alert.messageText = @"Yanami could not start";
                    alert.informativeText =
                        @"Yanami Desktop exited before its first window was ready.";
                    [alert runModal];
                    result = static_cast<int>(YanamiBootstrap::ExitCode::DesktopFailed);
                }
                break;
            }

            if (!ready && std::chrono::steady_clock::now() >= deadline) {
                if (options.waitForDesktopExit()) {
                    kill(child, SIGTERM);
                    result = static_cast<int>(YanamiBootstrap::ExitCode::ReadyTimeout);
                    break;
                }
                NSAlert *alert = [[NSAlert alloc] init];
                alert.messageText = @"Yanami is taking longer than expected";
                alert.informativeText =
                    @"Keep waiting, or safely cancel the startup process.";
                [alert addButtonWithTitle:@"Keep Waiting"];
                [alert addButtonWithTitle:@"Cancel"];
                if ([alert runModal] == NSAlertFirstButtonReturn)
                    deadline = std::chrono::steady_clock::now() + options.readyTimeout;
                else
                    controller.cancelRequested = YES;
            }
        }

        if (handedOff)
            [controller.window orderOut:nil];
        cleanupReadyDirectory(*readyDirectory);
        close(instanceLock);
        return result;
    }
}
