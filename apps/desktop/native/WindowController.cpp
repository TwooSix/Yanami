#include "WindowController.hpp"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QTimer>

#include <optional>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <qt_windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
QString colorSpaceName(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    switch (colorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return QStringLiteral("RGB_FULL_G22_P709 (SDR)");
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return QStringLiteral("RGB_FULL_G10_P709 (scRGB)");
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return QStringLiteral("RGB_FULL_G2084_P2020 (HDR10/PQ)");
    default:
        return QStringLiteral("DXGI_COLOR_SPACE_%1").arg(static_cast<int>(colorSpace));
    }
}

std::optional<DXGI_OUTPUT_DESC1> dxgiOutputDescription(HMONITOR targetMonitor)
{
    IDXGIFactory1 *factory = nullptr;
    const HRESULT factoryResult = CreateDXGIFactory1(
        IID_IDXGIFactory1, reinterpret_cast<void **>(&factory));
    if (FAILED(factoryResult) || !factory)
        return std::nullopt;

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            continue;
        for (UINT outputIndex = 0;; ++outputIndex) {
            IDXGIOutput *output = nullptr;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!output)
                continue;
            DXGI_OUTPUT_DESC basicDescription{};
            output->GetDesc(&basicDescription);
            if (basicDescription.Monitor == targetMonitor) {
                IDXGIOutput6 *output6 = nullptr;
                const HRESULT queryResult = output->QueryInterface(
                    IID_IDXGIOutput6, reinterpret_cast<void **>(&output6));
                if (SUCCEEDED(queryResult) && output6) {
                    DXGI_OUTPUT_DESC1 description{};
                    const HRESULT descriptionResult = output6->GetDesc1(&description);
                    if (SUCCEEDED(descriptionResult)) {
                        output6->Release();
                        output->Release();
                        adapter->Release();
                        factory->Release();
                        return description;
                    }
                    output6->Release();
                }
                output->Release();
                adapter->Release();
                factory->Release();
                return std::nullopt;
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return std::nullopt;
}

QString dxgiOutputState(HMONITOR targetMonitor)
{
    const auto description = dxgiOutputDescription(targetMonitor);
    if (!description)
        return QStringLiteral("dxgiOutput=unavailable");
    return QStringLiteral("output=%1 bits=%2 colorSpace=%3 luminance=%4/%5/%6")
        .arg(QString::fromWCharArray(description->DeviceName))
        .arg(description->BitsPerColor)
        .arg(colorSpaceName(description->ColorSpace))
        .arg(description->MinLuminance, 0, 'f', 3)
        .arg(description->MaxLuminance, 0, 'f', 1)
        .arg(description->MaxFullFrameLuminance, 0, 'f', 1);
}

bool isAdvancedColorOutput(HMONITOR monitor)
{
    const auto description = dxgiOutputDescription(monitor);
    if (!description)
        return false;
    switch (description->ColorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        return true;
    default:
        return false;
    }
}

void extendOnePixelBeyondVirtualDesktop(RECT &bounds)
{
    const LONG virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG virtualRight = virtualLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG virtualBottom = virtualTop + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (bounds.left <= virtualLeft)
        --bounds.left;
    else if (bounds.right >= virtualRight)
        ++bounds.right;
    else if (bounds.top <= virtualTop)
        --bounds.top;
    else if (bounds.bottom >= virtualBottom)
        ++bounds.bottom;
    else
        --bounds.left;
}

void logWindowAndOutputState(QWindow *window, const QString &phase, int elapsedMilliseconds)
{
    if (!window)
        return;
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    RECT rectangle{};
    GetWindowRect(handle, &rectangle);
    const HMONITOR monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    qInfo().noquote()
        << QStringLiteral("fullscreen-transition phase=%1 t=%2ms").arg(phase).arg(elapsedMilliseconds)
        << QStringLiteral("window=[%1,%2 %3x%4]")
               .arg(rectangle.left)
               .arg(rectangle.top)
               .arg(rectangle.right - rectangle.left)
               .arg(rectangle.bottom - rectangle.top)
        << QStringLiteral("monitor=[%1,%2 %3x%4]")
               .arg(monitorInfo.rcMonitor.left)
               .arg(monitorInfo.rcMonitor.top)
               .arg(monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left)
               .arg(monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top)
        << "zoomed=" << (IsZoomed(handle) != FALSE)
        << "qtVisibility=" << static_cast<int>(window->visibility())
        << dxgiOutputState(monitor);
}

void logTransitionTimeline(QWindow *window, const QString &phase)
{
    if (!qEnvironmentVariableIsSet("YANAMI_DEV_LOG_PATH")
        && !qEnvironmentVariableIsSet("YANAMI_DEV_FULLSCREEN_DIAGNOSTICS"))
        return;
    const int checkpoints[] = {0, 16, 33, 67, 125, 250, 500, 1000, 2000};
    for (const int milliseconds : checkpoints) {
        QTimer::singleShot(milliseconds, window, [guardedWindow = QPointer<QWindow>(window), phase, milliseconds] {
            if (guardedWindow)
                logWindowAndOutputState(guardedWindow, phase, milliseconds);
        });
    }
}
#else
void logTransitionTimeline(QWindow *, const QString &)
{
}
#endif

} // namespace

WindowController::WindowController(QObject *parent)
    : QObject(parent)
{
    m_cursorIdleTimer.setInterval(3200);
    m_cursorIdleTimer.setSingleShot(true);
    connect(&m_cursorIdleTimer, &QTimer::timeout, this, [this] {
        if (m_fullScreen)
            setCursorHidden(true);
    });
}

void WindowController::configureWindow(QWindow *window)
{
    if (m_window && m_window != window)
        m_window->removeEventFilter(this);
    m_window = window;
    if (m_window)
        m_window->installEventFilter(this);
    setRoundedCorners(window, true);
}

void WindowController::enterFullScreen(QWindow *window)
{
    if (!window || m_fullScreen)
        return;

    m_window = window;
    m_previousGeometry = window->geometry();
    m_previousVisibility = window->visibility();
    m_transitioning = true;
    logTransitionTimeline(window, QStringLiteral("enter"));
    setRoundedCorners(window, false);

#ifdef Q_OS_WIN
    if (m_previousVisibility == QWindow::Maximized) {
        applyBorderlessFullScreen(window);
    } else {
        window->showMaximized();
        QTimer::singleShot(100, this, [this, guardedWindow = QPointer<QWindow>(window)] {
            if (m_fullScreen && guardedWindow && guardedWindow == m_window) {
                applyBorderlessFullScreen(guardedWindow);
                QTimer::singleShot(250, this, [this] { m_transitioning = false; });
            }
        });
    }
#else
    window->showFullScreen();
#endif

    setFullScreen(true);
#ifdef Q_OS_WIN
    if (m_previousVisibility == QWindow::Maximized)
        QTimer::singleShot(250, this, [this] { m_transitioning = false; });
#else
    QTimer::singleShot(250, this, [this] { m_transitioning = false; });
#endif
}

void WindowController::exitFullScreen()
{
    if (!m_fullScreen || !m_window)
        return;

    m_transitioning = true;
    logTransitionTimeline(m_window, QStringLiteral("exit"));

    if (m_previousVisibility == QWindow::Maximized)
        m_window->showMaximized();
    else {
        m_window->showNormal();
        m_window->setGeometry(m_previousGeometry);
    }
    setRoundedCorners(m_window, true);

    setFullScreen(false);
    QTimer::singleShot(250, this, [this] { m_transitioning = false; });
}

void WindowController::toggleFullScreen(QWindow *window)
{
    if (m_transitioning)
        return;
    if (m_fullScreen)
        exitFullScreen();
    else
        enterFullScreen(window);
}

void WindowController::setCursorHidden(bool hidden)
{
    if (m_cursorHidden == hidden)
        return;
    m_cursorHidden = hidden;
    if (hidden)
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    else
        QGuiApplication::restoreOverrideCursor();
    if (qEnvironmentVariableIsSet("YANAMI_DEV_RENDER_DIAGNOSTICS"))
        qInfo() << "player-cursor override-hidden=" << hidden;
}

bool WindowController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && m_fullScreen
        && (event->type() == QEvent::MouseMove || event->type() == QEvent::Enter))
        notePointerActivity();
    return QObject::eventFilter(watched, event);
}

void WindowController::notePointerActivity()
{
    if (!m_fullScreen)
        return;
    setCursorHidden(false);
    m_cursorIdleTimer.start();
}

void WindowController::setFullScreen(bool fullScreen)
{
    if (m_fullScreen == fullScreen)
        return;
    m_fullScreen = fullScreen;
    if (m_fullScreen)
        notePointerActivity();
    else {
        m_cursorIdleTimer.stop();
        setCursorHidden(false);
    }
    emit fullScreenChanged();
}

void WindowController::setRoundedCorners(QWindow *window, bool rounded)
{
#ifdef Q_OS_WIN
    if (!window)
        return;
    constexpr DWORD cornerPreferenceAttribute = 33;
    const DWORD preference = rounded ? 2 : 1;
    DwmSetWindowAttribute(
        reinterpret_cast<HWND>(window->winId()),
        cornerPreferenceAttribute,
        &preference,
        sizeof(preference));
#else
    Q_UNUSED(window);
    Q_UNUSED(rounded);
#endif
}

void WindowController::applyBorderlessFullScreen(QWindow *window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    const HMONITOR monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;
    RECT bounds = monitorInfo.rcMonitor;
    const QByteArray diagnosticGeometry = qgetenv("YANAMI_DEV_FULLSCREEN_GEOMETRY").trimmed();
    if (diagnosticGeometry == "overscan-x") {
        --bounds.left;
        ++bounds.right;
    } else if (diagnosticGeometry == "overscan-left") {
        --bounds.left;
    } else if (diagnosticGeometry == "overscan-all") {
        --bounds.left;
        --bounds.top;
        ++bounds.right;
        ++bounds.bottom;
    } else if (diagnosticGeometry == "underscan-bottom") {
        --bounds.bottom;
    } else if (diagnosticGeometry.isEmpty() && isAdvancedColorOutput(monitor)) {
        // An exact monitor-sized SDR OpenGL window can be promoted to direct
        // presentation by Windows. On an HDR desktop that changes the physical
        // output from HDR/PQ to SDR and makes the display blank while it
        // renegotiates. A single off-screen pixel keeps DWM composition active
        // without changing any visible content.
        extendOnePixelBeyondVirtualDesktop(bounds);
    }
    qInfo().noquote() << "fullscreen geometry="
                      << (!diagnosticGeometry.isEmpty()
                              ? diagnosticGeometry
                              : (bounds.left != monitorInfo.rcMonitor.left
                                         || bounds.top != monitorInfo.rcMonitor.top
                                         || bounds.right != monitorInfo.rcMonitor.right
                                         || bounds.bottom != monitorInfo.rcMonitor.bottom
                                     ? QByteArrayLiteral("hdr-safe-overscan")
                                     : QByteArrayLiteral("exact")));
    SetWindowPos(
        handle,
        HWND_TOP,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    SetForegroundWindow(handle);
#else
    Q_UNUSED(window);
#endif
}
