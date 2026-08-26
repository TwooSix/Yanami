#include "ControllerNavigation.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#ifdef YANAMI_HAS_SDL3
#include <SDL3/SDL.h>
#endif

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#endif

Q_LOGGING_CATEGORY(lcControllerInput, "yanami.input.controller")

namespace {

constexpr int PollIntervalMs = 16;
constexpr qint64 DeviceScanIntervalMs = 250;
constexpr qint64 RemoteScanIntervalMs = 2'000;

constexpr std::array<ControllerInputAction, 12> ButtonActions{
    ControllerInputAction::Activate,
    ControllerInputAction::Back,
    ControllerInputAction::Context,
    ControllerInputAction::Menu,
    ControllerInputAction::Search,
    ControllerInputAction::PagePrevious,
    ControllerInputAction::PageNext,
    ControllerInputAction::PlayPause,
    ControllerInputAction::SeekBackward,
    ControllerInputAction::SeekForward,
    ControllerInputAction::PreviousItem,
    ControllerInputAction::NextItem,
};

std::array<bool, ButtonActions.size()> buttonValues(
    const ControllerNavigationState::Snapshot &snapshot)
{
    return {
        snapshot.confirm,
        snapshot.back,
        snapshot.context,
        snapshot.menu,
        snapshot.search,
        snapshot.pagePrevious,
        snapshot.pageNext,
        snapshot.playPause,
        snapshot.seekBackward,
        snapshot.seekForward,
        snapshot.previousItem,
        snapshot.nextItem,
    };
}

QString hexId(quint16 value)
{
    return QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0'));
}

QVariantMap makeDescriptor(const QString &id,
                           const QString &name,
                           const QString &family,
                           const QString &supportTier,
                           const QString &backend,
                           quint16 vendorId = 0,
                           quint16 productId = 0)
{
    QVariantMap descriptor{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("family"), family},
        {QStringLiteral("supportTier"), supportTier},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("connected"), true},
    };
    if (vendorId != 0)
        descriptor.insert(QStringLiteral("vendorId"), hexId(vendorId));
    if (productId != 0)
        descriptor.insert(QStringLiteral("productId"), hexId(productId));
    return descriptor;
}

QString descriptorId(const QVariantMap &descriptor)
{
    return descriptor.value(QStringLiteral("id")).toString();
}

QString descriptorName(const QVariantMap &descriptor)
{
    return descriptor.value(QStringLiteral("name")).toString();
}

void sortDescriptors(QVariantList &descriptors)
{
    std::sort(descriptors.begin(), descriptors.end(),
              [](const QVariant &left, const QVariant &right) {
        return descriptorId(left.toMap()) < descriptorId(right.toMap());
    });
}

struct RemoteVidPidProfile {
    quint16 vendorId;
    quint16 productId;
    const char *displayName;
};

// Narrow, reviewable exceptions belong here. A generic Consumer Control HID
// collection is deliberately insufficient because multimedia keyboards use
// the same standard usage page.
constexpr std::array<RemoteVidPidProfile, 2> RemoteProfiles{{
    {0x20a0, 0x0006, "Flirc Remote"},
    {0x0471, 0x0815, "eHome Infrared Remote"},
}};

QString profileName(quint16 vendorId, quint16 productId)
{
    for (const RemoteVidPidProfile &profile : RemoteProfiles) {
        if (profile.vendorId == vendorId
            && profile.productId == productId) {
            return QString::fromLatin1(profile.displayName);
        }
    }
    return {};
}

#ifdef Q_OS_WIN

void parseVidPid(const QString &devicePath,
                 quint16 &vendorId,
                 quint16 &productId)
{
    static const QRegularExpression expression(
        QStringLiteral("VID_([0-9A-F]{4}).*PID_([0-9A-F]{4})"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(devicePath);
    if (!match.hasMatch())
        return;
    bool vendorOk = false;
    bool productOk = false;
    const quint16 parsedVendor = match.captured(1).toUShort(&vendorOk, 16);
    const quint16 parsedProduct = match.captured(2).toUShort(&productOk, 16);
    if (vendorOk)
        vendorId = parsedVendor;
    if (productOk)
        productId = parsedProduct;
}

struct RawRemoteScan {
    QVariantList descriptors;
    QHash<quintptr, QString> handleToId;
    QString error;
};

struct RawDeviceCandidate {
    HANDLE handle = nullptr;
    DWORD type = 0;
    quint16 usagePage = 0;
    quint16 usage = 0;
    quint16 vendorId = 0;
    quint16 productId = 0;
    QString devicePath;
};

RawRemoteScan enumerateRawRemoteDevices()
{
    RawRemoteScan result;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count,
                              sizeof(RAWINPUTDEVICELIST)) != 0) {
        result.error = QStringLiteral("GetRawInputDeviceList(count): %1")
                           .arg(GetLastError());
        return result;
    }
    if (count == 0) {
        return result;
    }

    QVector<RAWINPUTDEVICELIST> devices(static_cast<qsizetype>(count));
    UINT actualCount = count;
    if (GetRawInputDeviceList(devices.data(), &actualCount,
                              sizeof(RAWINPUTDEVICELIST))
        == static_cast<UINT>(-1)) {
        result.error = QStringLiteral("GetRawInputDeviceList(data): %1")
                           .arg(GetLastError());
        return result;
    }

    QVector<RawDeviceCandidate> candidates;
    for (UINT index = 0; index < actualCount; ++index) {
        const RAWINPUTDEVICELIST &device = devices.at(
            static_cast<qsizetype>(index));
        if (device.dwType != RIM_TYPEHID
            && device.dwType != RIM_TYPEKEYBOARD) {
            continue;
        }

        UINT nameLength = 0;
        GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME,
                               nullptr, &nameLength);
        if (nameLength == 0)
            continue;
        QVector<wchar_t> nameBuffer(static_cast<qsizetype>(nameLength + 1),
                                    L'\0');
        UINT requestedLength = nameLength;
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME,
                                   nameBuffer.data(), &requestedLength)
            == static_cast<UINT>(-1)) {
            continue;
        }
        const QString devicePath = QString::fromWCharArray(nameBuffer.data());

        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT infoSize = sizeof(info);
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO,
                                   &info, &infoSize)
            == static_cast<UINT>(-1)) {
            continue;
        }

        quint16 usagePage = 0x01;
        quint16 usage = 0x06;
        quint16 vendorId = 0;
        quint16 productId = 0;
        if (info.dwType == RIM_TYPEHID) {
            usagePage = info.hid.usUsagePage;
            usage = info.hid.usUsage;
            vendorId = static_cast<quint16>(info.hid.dwVendorId);
            productId = static_cast<quint16>(info.hid.dwProductId);
        }
        parseVidPid(devicePath, vendorId, productId);
        candidates.append({
            .handle = device.hDevice,
            .type = device.dwType,
            .usagePage = usagePage,
            .usage = usage,
            .vendorId = vendorId,
            .productId = productId,
            .devicePath = devicePath,
        });
    }

    QSet<QString> confirmedRemotePhysicalDevices;
    for (const RawDeviceCandidate &candidate : candidates) {
        const auto classification =
            ControllerNavigationSource::classifyRawInputDevice(
                candidate.usagePage, candidate.usage,
                candidate.vendorId, candidate.productId,
                candidate.devicePath);
        if (classification.family == QStringLiteral("remote")) {
            confirmedRemotePhysicalDevices.insert(
                ControllerNavigationSource::rawInputPhysicalPathKey(
                    candidate.devicePath));
        }
    }

    for (const RawDeviceCandidate &candidate : candidates) {
        auto classification =
            ControllerNavigationSource::classifyRawInputDevice(
                candidate.usagePage, candidate.usage,
                candidate.vendorId, candidate.productId,
                candidate.devicePath);
        const QString physicalPath =
            ControllerNavigationSource::rawInputPhysicalPathKey(
                candidate.devicePath);
        if (candidate.type == RIM_TYPEKEYBOARD
            && ControllerNavigationSource::shouldAssociateRawKeyboard(
                classification,
                confirmedRemotePhysicalDevices.contains(physicalPath))) {
            classification = {QStringLiteral("remote"),
                              QStringLiteral("experimental")};
        }
        if (classification.family != QStringLiteral("remote"))
            continue;

        const QByteArray digest = QCryptographicHash::hash(
            physicalPath.toUtf8(),
            QCryptographicHash::Sha256).toHex().left(16);
        const QString id = QStringLiteral("raw:%1")
                               .arg(QString::fromLatin1(digest));
        QString displayName = profileName(candidate.vendorId,
                                          candidate.productId);
        if (displayName.isEmpty())
            displayName = QStringLiteral("HID TV Remote");
        bool descriptorExists = false;
        for (const QVariant &value : result.descriptors) {
            if (descriptorId(value.toMap()) == id) {
                descriptorExists = true;
                break;
            }
        }
        if (!descriptorExists) {
            result.descriptors.append(makeDescriptor(
                id, displayName, classification.family,
                classification.supportTier, QStringLiteral("raw-input"),
                candidate.vendorId, candidate.productId));
        }
        result.handleToId.insert(
            reinterpret_cast<quintptr>(candidate.handle), id);
    }
    sortDescriptors(result.descriptors);
    return result;
}

#endif

} // namespace

QVector<int> ControllerNavigationState::update(
    const Snapshot &snapshot,
    qint64 nowMs)
{
    QVector<int> requestedKeys;
    const QVector<ActionEvent> events = updateActions(snapshot, nowMs);
    for (const ActionEvent &event : events) {
        if (!event.pressed)
            continue;
        const int key = qtKeyForAction(event.action);
        if (key != Qt::Key_unknown)
            requestedKeys.append(key);
    }
    return requestedKeys;
}

QVector<ControllerNavigationState::ActionEvent>
ControllerNavigationState::updateActions(
    const Snapshot &snapshot,
    qint64 nowMs)
{
    QVector<ActionEvent> events;
    if (!snapshot.connected) {
        for (std::size_t index = 0; index < m_buttonPressed.size(); ++index) {
            if (m_buttonPressed[index]) {
                events.append({ButtonActions[index], false, false});
            }
        }
        if (m_direction != Direction::None) {
            events.append({directionAction(m_direction, false), false, false});
        }
        if (m_scrollDirection != Direction::None) {
            events.append(
                {directionAction(m_scrollDirection, true), false, false});
        }
        reset();
        return events;
    }

    if (m_awaitingNeutral) {
        if (!isNeutral(snapshot))
            return events;
        m_awaitingNeutral = false;
        m_initialized = true;
        return events;
    }

    if (!m_initialized) {
        m_initialized = true;
        return events;
    }

    const auto currentButtons = buttonValues(snapshot);
    for (std::size_t index = 0; index < currentButtons.size(); ++index) {
        if (currentButtons[index] == m_buttonPressed[index])
            continue;
        events.append({ButtonActions[index], currentButtons[index], false});
        m_buttonPressed[index] = currentButtons[index];
    }

    const Direction direction = directionFor(snapshot);
    if (direction != m_direction) {
        appendDirectionTransition(events, m_direction, direction,
                                  nowMs, false);
        m_direction = direction;
    } else if (direction != Direction::None
               && nowMs >= m_nextRepeatAtMs) {
        events.append({directionAction(direction, false), true, true});
        m_nextRepeatAtMs = nowMs + RepeatIntervalMs;
    }

    const Direction scrollDirection = scrollDirectionFor(snapshot);
    if (scrollDirection != m_scrollDirection) {
        appendDirectionTransition(events, m_scrollDirection,
                                  scrollDirection, nowMs, true);
        m_scrollDirection = scrollDirection;
    } else if (scrollDirection != Direction::None
               && nowMs >= m_nextScrollRepeatAtMs) {
        events.append(
            {directionAction(scrollDirection, true), true, true});
        m_nextScrollRepeatAtMs = nowMs + RepeatIntervalMs;
    }

    return events;
}

void ControllerNavigationState::reset()
{
    m_initialized = false;
    m_awaitingNeutral = true;
    m_buttonPressed.fill(false);
    m_direction = Direction::None;
    m_scrollDirection = Direction::None;
    m_nextRepeatAtMs = 0;
    m_nextScrollRepeatAtMs = 0;
}

ControllerNavigationState::Direction ControllerNavigationState::directionFor(
    const Snapshot &snapshot)
{
    if (snapshot.dpadUp != snapshot.dpadDown)
        return snapshot.dpadUp ? Direction::Up : Direction::Down;
    if (snapshot.dpadLeft != snapshot.dpadRight)
        return snapshot.dpadLeft ? Direction::Left : Direction::Right;

    const int x = snapshot.leftThumbX;
    const int y = snapshot.leftThumbY;
    const int absoluteX = std::abs(x);
    const int absoluteY = std::abs(y);
    if (std::max(absoluteX, absoluteY) < LeftStickThreshold)
        return Direction::None;
    if (absoluteY >= absoluteX)
        return y > 0 ? Direction::Up : Direction::Down;
    return x > 0 ? Direction::Right : Direction::Left;
}

ControllerNavigationState::Direction
ControllerNavigationState::scrollDirectionFor(const Snapshot &snapshot)
{
    Snapshot rightStick;
    rightStick.leftThumbX = snapshot.rightThumbX;
    rightStick.leftThumbY = snapshot.rightThumbY;
    const int x = rightStick.leftThumbX;
    const int y = rightStick.leftThumbY;
    if (std::max(std::abs(x), std::abs(y)) < RightStickThreshold)
        return Direction::None;
    if (std::abs(y) >= std::abs(x))
        return y > 0 ? Direction::Up : Direction::Down;
    return x > 0 ? Direction::Right : Direction::Left;
}

bool ControllerNavigationState::isNeutral(const Snapshot &snapshot)
{
    if (!snapshot.connected)
        return true;
    const auto buttons = buttonValues(snapshot);
    return std::none_of(buttons.begin(), buttons.end(),
                        [](bool pressed) { return pressed; })
        && directionFor(snapshot) == Direction::None
        && scrollDirectionFor(snapshot) == Direction::None;
}

qint16 ControllerNavigationState::invertVerticalAxis(qint16 value)
{
    if (value == std::numeric_limits<qint16>::min())
        return std::numeric_limits<qint16>::max();
    return static_cast<qint16>(-value);
}

int ControllerNavigationState::qtKeyForAction(
    ControllerInputAction action)
{
    switch (action) {
    case ControllerInputAction::NavigateUp:
        return Qt::Key_Up;
    case ControllerInputAction::NavigateDown:
        return Qt::Key_Down;
    case ControllerInputAction::NavigateLeft:
        return Qt::Key_Left;
    case ControllerInputAction::NavigateRight:
        return Qt::Key_Right;
    case ControllerInputAction::Activate:
        return Qt::Key_Return;
    case ControllerInputAction::Back:
        return Qt::Key_Escape;
    case ControllerInputAction::Context:
        return Qt::Key_Menu;
    default:
        return Qt::Key_unknown;
    }
}

void ControllerNavigationState::appendDirectionTransition(
    QVector<ActionEvent> &events,
    Direction previous,
    Direction current,
    qint64 nowMs,
    bool scroll)
{
    if (previous != Direction::None)
        events.append({directionAction(previous, scroll), false, false});
    if (current != Direction::None)
        events.append({directionAction(current, scroll), true, false});
    if (scroll)
        m_nextScrollRepeatAtMs = nowMs + InitialRepeatDelayMs;
    else
        m_nextRepeatAtMs = nowMs + InitialRepeatDelayMs;
}

ControllerInputAction ControllerNavigationState::directionAction(
    Direction direction,
    bool scroll)
{
    if (scroll) {
        switch (direction) {
        case Direction::Up:
            return ControllerInputAction::ScrollUp;
        case Direction::Down:
            return ControllerInputAction::ScrollDown;
        case Direction::Left:
            return ControllerInputAction::ScrollLeft;
        case Direction::Right:
            return ControllerInputAction::ScrollRight;
        case Direction::None:
            break;
        }
    } else {
        switch (direction) {
        case Direction::Up:
            return ControllerInputAction::NavigateUp;
        case Direction::Down:
            return ControllerInputAction::NavigateDown;
        case Direction::Left:
            return ControllerInputAction::NavigateLeft;
        case Direction::Right:
            return ControllerInputAction::NavigateRight;
        case Direction::None:
            break;
        }
    }
    return ControllerInputAction::Activate;
}

struct ControllerNavigationSource::Impl {
    enum class Backend {
        None,
        Sdl,
        XInput,
    };

#ifdef YANAMI_HAS_SDL3
    struct SdlSlot {
        SDL_Gamepad *gamepad = nullptr;
        ControllerNavigationState navigation;
        QVariantMap descriptor;
        QString family;
    };
    QHash<quint32, SdlSlot> sdlSlots;
    bool sdlInitialized = false;
#endif

#ifdef Q_OS_WIN
    struct XInputSlot {
        ControllerNavigationState navigation;
        bool connected = false;
        qint64 nextProbeAtMs = 0;
        QVariantMap descriptor;
    };
    std::array<XInputSlot, 4> xinputSlots;
#endif

    Backend backend = Backend::None;
    QString backendName = QStringLiteral("none");
    QVariantList publishedDevices;
    QVariantList remoteDescriptors;
    QHash<quintptr, QString> remoteHandleToId;
    qint64 nextDeviceScanAtMs = 0;
    qint64 nextRemoteScanAtMs = 0;
    QString lastSdlScanError;
    QSet<quint32> reportedSdlOpenFailures;
    QString lastRawScanError;
};

ControllerNavigationSource::ControllerNavigationSource(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    m_elapsedTimer.start();
    const QString preference = qEnvironmentVariable(
        "YANAMI_CONTROLLER_BACKEND", "auto").trimmed().toLower();

    const bool disabled = preference == QStringLiteral("disabled")
        || preference == QStringLiteral("none");
    const bool forceXInput = preference == QStringLiteral("xinput");
    const bool forceSdl = preference == QStringLiteral("sdl")
        || preference == QStringLiteral("sdl3");

#ifdef YANAMI_HAS_SDL3
    if (!disabled && !forceXInput) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "auto");
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            SDL_SetGamepadEventsEnabled(false);
            m_impl->sdlInitialized = true;
            m_impl->backend = Impl::Backend::Sdl;
            m_impl->backendName = QStringLiteral("sdl");
        } else {
            qCWarning(lcControllerInput)
                << "SDL gamepad initialization failed:" << SDL_GetError();
        }
    }
#else
    Q_UNUSED(forceSdl)
#endif

#ifdef Q_OS_WIN
    if (!disabled && m_impl->backend == Impl::Backend::None && !forceSdl) {
        m_impl->backend = Impl::Backend::XInput;
        m_impl->backendName = QStringLiteral("xinput");
    }
#endif

    qCInfo(lcControllerInput)
        << "Controller backend selected:" << m_impl->backendName;

    m_pollTimer.setTimerType(Qt::PreciseTimer);
    m_pollTimer.setInterval(PollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout,
            this, &ControllerNavigationSource::poll);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged,
            this,
            &ControllerNavigationSource::handleApplicationStateChanged);
    handleApplicationStateChanged(QGuiApplication::applicationState());
}

ControllerNavigationSource::~ControllerNavigationSource()
{
    m_pollTimer.stop();
    resetInputStates(false);
#ifdef YANAMI_HAS_SDL3
    for (auto iterator = m_impl->sdlSlots.begin();
         iterator != m_impl->sdlSlots.end(); ++iterator) {
        if (iterator->gamepad)
            SDL_CloseGamepad(iterator->gamepad);
    }
    m_impl->sdlSlots.clear();
    if (m_impl->sdlInitialized)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
}

QVariantList ControllerNavigationSource::connectedDevices() const
{
    return m_impl->publishedDevices;
}

QString ControllerNavigationSource::backend() const
{
    return m_impl->backendName;
}

QVariantMap ControllerNavigationSource::deviceDescriptor(
    const QString &deviceId) const
{
    for (const QVariant &value : m_impl->publishedDevices) {
        const QVariantMap descriptor = value.toMap();
        if (descriptorId(descriptor) == deviceId)
            return descriptor;
    }
    return {};
}

QString ControllerNavigationSource::remoteDeviceIdForNativeHandle(
    quintptr nativeHandle) const
{
    return m_impl->remoteHandleToId.value(nativeHandle);
}

ControllerNavigationSource::DeviceClassification
ControllerNavigationSource::classifyGamepad(int nativeType,
                                            quint16 vendorId,
                                            const QString &name)
{
    const QString normalized = name.toLower();
    // SDL_GamepadType values are stable API values. Keeping numeric values out
    // of the public header lets non-SDL builds compile the pure classifier.
    if (nativeType == 2 || nativeType == 3 || vendorId == 0x045e
        || normalized.contains(QStringLiteral("xbox"))) {
        return {QStringLiteral("xbox"), QStringLiteral("hardware-pending")};
    }
    if (nativeType >= 4 && nativeType <= 6
        || vendorId == 0x054c
        || normalized.contains(QStringLiteral("dualshock"))
        || normalized.contains(QStringLiteral("dualsense"))
        || normalized.contains(QStringLiteral("playstation"))) {
        return {QStringLiteral("playstation"),
                QStringLiteral("experimental")};
    }
    if ((nativeType >= 7 && nativeType <= 10) || vendorId == 0x057e
        || normalized.contains(QStringLiteral("nintendo"))
        || normalized.contains(QStringLiteral("switch"))
        || normalized.contains(QStringLiteral("joy-con"))) {
        return {QStringLiteral("nintendo"),
                QStringLiteral("experimental")};
    }
    return {QStringLiteral("generic"), QStringLiteral("generic")};
}

ControllerNavigationSource::DeviceClassification
ControllerNavigationSource::classifyRawInputDevice(
    quint16 usagePage,
    quint16 usage,
    quint16 vendorId,
    quint16 productId,
    const QString &name)
{
    const QString normalized = name.toLower();
    const bool knownProfile =
        !profileName(vendorId, productId).isEmpty();
    Q_UNUSED(usagePage)
    Q_UNUSED(usage)
    const bool nameLooksRemote =
        normalized.contains(QStringLiteral("remote"))
        || normalized.contains(QStringLiteral("flirc"))
        || normalized.contains(QStringLiteral("ehome"))
        || normalized.contains(QStringLiteral("infrared"))
        || normalized.contains(QStringLiteral("rc6"));
    if (knownProfile || nameLooksRemote) {
        return {QStringLiteral("remote"),
                QStringLiteral("experimental")};
    }
    return {QStringLiteral("generic"), QStringLiteral("generic")};
}

QString ControllerNavigationSource::rawInputPhysicalPathKey(
    const QString &devicePath)
{
    QString physicalPath = devicePath.toLower();
    static const QRegularExpression collectionSuffix(
        QStringLiteral("&col[0-9a-f]{2}"));
    physicalPath.remove(collectionSuffix);
    return physicalPath;
}

bool ControllerNavigationSource::shouldAssociateRawKeyboard(
    const DeviceClassification &ownClassification,
    bool physicalDeviceHasConfirmedRemoteCollection)
{
    return ownClassification.family != QStringLiteral("remote")
        && physicalDeviceHasConfirmedRemoteCollection;
}

bool ControllerNavigationSource::sdlEnumerationSucceeded(
    bool resultBufferReturned)
{
    // SDL_GetGamepads() returns a 0-terminated allocation even when the
    // successful result contains zero devices; null is the documented
    // failure signal. Failed scans retain the last known device snapshot.
    return resultBufferReturned;
}

bool ControllerNavigationSource::rawRemoteEnumerationSucceeded(
    const QString &error)
{
    return error.isEmpty();
}

bool ControllerNavigationSource::nintendoConfirmUsesEastButton(
    const DeviceClassification &classification)
{
    return classification.family == QStringLiteral("nintendo");
}

ControllerNavigationSource::FaceButtonState
ControllerNavigationSource::mapFaceButtons(
    const DeviceClassification &classification,
    bool southPressed,
    bool eastPressed)
{
    if (nintendoConfirmUsesEastButton(classification))
        return {.confirm = eastPressed, .back = southPressed};
    return {.confirm = southPressed, .back = eastPressed};
}

void ControllerNavigationSource::handleApplicationStateChanged(
    Qt::ApplicationState state)
{
    resetInputStates(true);
    if (state != Qt::ApplicationActive) {
        m_pollTimer.stop();
        return;
    }

    const qint64 nowMs = m_elapsedTimer.elapsed();
    m_impl->nextDeviceScanAtMs = nowMs;
    m_impl->nextRemoteScanAtMs = nowMs;
#ifdef Q_OS_WIN
    for (Impl::XInputSlot &slot : m_impl->xinputSlots)
        slot.nextProbeAtMs = nowMs;
#endif
    m_pollTimer.setInterval(PollIntervalMs);
    m_pollTimer.start();
}

void ControllerNavigationSource::poll()
{
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        m_pollTimer.stop();
        resetInputStates(true);
        return;
    }

    const qint64 nowMs = m_elapsedTimer.elapsed();
    bool devicesMayHaveChanged = false;
    bool anyControllerConnected = false;

#ifdef YANAMI_HAS_SDL3
    if (m_impl->backend == Impl::Backend::Sdl) {
        SDL_UpdateGamepads();
        if (nowMs >= m_impl->nextDeviceScanAtMs) {
            m_impl->nextDeviceScanAtMs = nowMs + DeviceScanIntervalMs;
            int count = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&count);
            QSet<quint32> connectedIds;
            const bool scanSucceeded = sdlEnumerationSucceeded(ids != nullptr);
            if (ids) {
                for (int index = 0; index < count; ++index)
                    connectedIds.insert(static_cast<quint32>(ids[index]));
                SDL_free(ids);
            }
            if (scanSucceeded) {
                m_impl->lastSdlScanError.clear();
            } else {
                const QString error = QString::fromUtf8(SDL_GetError());
                if (error != m_impl->lastSdlScanError) {
                    qCWarning(lcControllerInput)
                        << "SDL gamepad scan failed:" << error;
                    m_impl->lastSdlScanError = error;
                }
                // A transient enumeration error is not evidence that every
                // currently open controller disconnected. Preserve the last
                // successful snapshot and retry on the next scan interval.
                for (auto iterator = m_impl->sdlSlots.cbegin();
                     iterator != m_impl->sdlSlots.cend(); ++iterator) {
                    connectedIds.insert(iterator.key());
                }
            }

            QVector<quint32> removedIds;
            for (auto iterator = m_impl->sdlSlots.cbegin();
                 iterator != m_impl->sdlSlots.cend(); ++iterator) {
                if (!connectedIds.contains(iterator.key()))
                    removedIds.append(iterator.key());
            }
            for (const quint32 id : removedIds) {
                m_impl->reportedSdlOpenFailures.remove(id);
                auto iterator = m_impl->sdlSlots.find(id);
                const QString deviceId = descriptorId(iterator->descriptor);
                ControllerNavigationState::Snapshot disconnected;
                emitEvents(iterator->navigation.updateActions(disconnected,
                                                               nowMs),
                           deviceId);
                qCInfo(lcControllerInput)
                    << "Controller disconnected:"
                    << descriptorName(iterator->descriptor) << deviceId;
                if (iterator->gamepad)
                    SDL_CloseGamepad(iterator->gamepad);
                m_impl->sdlSlots.erase(iterator);
                devicesMayHaveChanged = true;
            }

            for (const quint32 id : connectedIds) {
                if (m_impl->sdlSlots.contains(id))
                    continue;
                SDL_Gamepad *gamepad = SDL_OpenGamepad(
                    static_cast<SDL_JoystickID>(id));
                if (!gamepad) {
                    if (!m_impl->reportedSdlOpenFailures.contains(id)) {
                        qCWarning(lcControllerInput)
                            << "SDL could not open gamepad" << id
                            << SDL_GetError();
                        m_impl->reportedSdlOpenFailures.insert(id);
                    }
                    continue;
                }
                m_impl->reportedSdlOpenFailures.remove(id);
                const char *nativeName = SDL_GetGamepadName(gamepad);
                const QString name = nativeName
                    ? QString::fromUtf8(nativeName)
                    : QStringLiteral("Gamepad");
                const quint16 vendorId = SDL_GetGamepadVendor(gamepad);
                const quint16 productId = SDL_GetGamepadProduct(gamepad);
                const auto classification = classifyGamepad(
                    static_cast<int>(SDL_GetGamepadType(gamepad)),
                    vendorId, name);
                char guidBuffer[33]{};
                SDL_GUIDToString(SDL_GetGamepadGUIDForID(
                                     static_cast<SDL_JoystickID>(id)),
                                 guidBuffer,
                                 static_cast<int>(sizeof(guidBuffer)));
                const QString deviceId = QStringLiteral("sdl:%1:%2")
                                             .arg(QString::fromLatin1(
                                                      guidBuffer),
                                                  QString::number(id));
                Impl::SdlSlot slot;
                slot.gamepad = gamepad;
                slot.family = classification.family;
                slot.descriptor = makeDescriptor(
                    deviceId, name, classification.family,
                    classification.supportTier, QStringLiteral("sdl"),
                    vendorId, productId);
                m_impl->sdlSlots.insert(id, slot);
                qCInfo(lcControllerInput)
                    << "Controller connected:" << name << deviceId
                    << classification.family << classification.supportTier;
                devicesMayHaveChanged = true;
            }
        }

        for (auto iterator = m_impl->sdlSlots.begin();
             iterator != m_impl->sdlSlots.end(); ++iterator) {
            anyControllerConnected = true;
            SDL_Gamepad *gamepad = iterator->gamepad;
            const bool south = SDL_GetGamepadButton(
                gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
            const bool east = SDL_GetGamepadButton(
                gamepad, SDL_GAMEPAD_BUTTON_EAST);
            const auto faceButtons = mapFaceButtons(
                {.family = iterator->family, .supportTier = {}},
                south, east);
            ControllerNavigationState::Snapshot snapshot{
                .connected = true,
                .dpadUp = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP),
                .dpadDown = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                .dpadLeft = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT),
                .dpadRight = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
                .confirm = faceButtons.confirm,
                .back = faceButtons.back,
                .context = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_WEST)
                    || SDL_GetGamepadButton(
                        gamepad, SDL_GAMEPAD_BUTTON_BACK),
                .menu = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_START),
                .search = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_NORTH),
                .pagePrevious = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
                .pageNext = SDL_GetGamepadButton(
                    gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
                .seekBackward = SDL_GetGamepadAxis(
                    gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 18'000,
                .seekForward = SDL_GetGamepadAxis(
                    gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 18'000,
                .leftThumbX = SDL_GetGamepadAxis(
                    gamepad, SDL_GAMEPAD_AXIS_LEFTX),
                .leftThumbY = ControllerNavigationState::invertVerticalAxis(
                    SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)),
                .rightThumbX = SDL_GetGamepadAxis(
                    gamepad, SDL_GAMEPAD_AXIS_RIGHTX),
                .rightThumbY = ControllerNavigationState::invertVerticalAxis(
                    SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY)),
            };
            emitEvents(iterator->navigation.updateActions(snapshot, nowMs),
                       descriptorId(iterator->descriptor));
        }
    }
#endif

#ifdef Q_OS_WIN
    if (m_impl->backend == Impl::Backend::XInput) {
        for (std::size_t index = 0;
             index < m_impl->xinputSlots.size(); ++index) {
            Impl::XInputSlot &slot = m_impl->xinputSlots[index];
            if (!slot.connected && nowMs < slot.nextProbeAtMs)
                continue;

            XINPUT_STATE nativeState{};
            if (XInputGetState(static_cast<DWORD>(index), &nativeState)
                != ERROR_SUCCESS) {
                if (slot.connected) {
                    ControllerNavigationState::Snapshot disconnected;
                    emitEvents(slot.navigation.updateActions(disconnected,
                                                              nowMs),
                               descriptorId(slot.descriptor));
                    qCInfo(lcControllerInput)
                        << "Controller disconnected:"
                        << descriptorName(slot.descriptor)
                        << descriptorId(slot.descriptor);
                    slot.descriptor.clear();
                    devicesMayHaveChanged = true;
                }
                slot.connected = false;
                slot.nextProbeAtMs = nowMs + DeviceScanIntervalMs;
                continue;
            }

            if (!slot.connected) {
                const QString id = QStringLiteral("xinput:%1").arg(index);
                const QString name = QStringLiteral("Xbox Controller %1")
                                         .arg(index + 1);
                slot.descriptor = makeDescriptor(
                    id, name, QStringLiteral("xbox"),
                    QStringLiteral("hardware-pending"), QStringLiteral("xinput"),
                    0x045e, 0);
                slot.navigation.reset();
                slot.connected = true;
                devicesMayHaveChanged = true;
                qCInfo(lcControllerInput)
                    << "Controller connected:" << name << id
                    << "xbox hardware-pending";
            }
            anyControllerConnected = true;
            const WORD buttons = nativeState.Gamepad.wButtons;
            ControllerNavigationState::Snapshot snapshot{
                .connected = true,
                .dpadUp = (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0,
                .dpadDown = (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0,
                .dpadLeft = (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0,
                .dpadRight = (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0,
                .confirm = (buttons & XINPUT_GAMEPAD_A) != 0,
                .back = (buttons & XINPUT_GAMEPAD_B) != 0,
                .context = (buttons & XINPUT_GAMEPAD_X) != 0
                    || (buttons & XINPUT_GAMEPAD_BACK) != 0,
                .menu = (buttons & XINPUT_GAMEPAD_START) != 0,
                .search = (buttons & XINPUT_GAMEPAD_Y) != 0,
                .pagePrevious =
                    (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
                .pageNext =
                    (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0,
                .seekBackward = nativeState.Gamepad.bLeftTrigger > 140,
                .seekForward = nativeState.Gamepad.bRightTrigger > 140,
                .leftThumbX = nativeState.Gamepad.sThumbLX,
                .leftThumbY = nativeState.Gamepad.sThumbLY,
                .rightThumbX = nativeState.Gamepad.sThumbRX,
                .rightThumbY = nativeState.Gamepad.sThumbRY,
            };
            emitEvents(slot.navigation.updateActions(snapshot, nowMs),
                       descriptorId(slot.descriptor));
        }
    }
#endif

#ifdef Q_OS_WIN
    if (nowMs >= m_impl->nextRemoteScanAtMs) {
        m_impl->nextRemoteScanAtMs = nowMs + RemoteScanIntervalMs;
        const QVariantList previousRemoteDevices =
            m_impl->remoteDescriptors;
        const RawRemoteScan scan = enumerateRawRemoteDevices();
        const bool scanSucceeded = rawRemoteEnumerationSucceeded(scan.error);
        if (!scanSucceeded
            && scan.error != m_impl->lastRawScanError) {
            qCWarning(lcControllerInput)
                << "Raw Input device enumeration failed:" << scan.error;
        }
        m_impl->lastRawScanError = scan.error;
        if (scanSucceeded) {
            m_impl->remoteDescriptors = scan.descriptors;
            m_impl->remoteHandleToId = scan.handleToId;
        }
        if (previousRemoteDevices != m_impl->remoteDescriptors) {
            for (const QVariant &value : m_impl->remoteDescriptors) {
                const QVariantMap descriptor = value.toMap();
                bool existed = false;
                for (const QVariant &oldValue : previousRemoteDevices) {
                    if (descriptorId(oldValue.toMap())
                        == descriptorId(descriptor)) {
                        existed = true;
                        break;
                    }
                }
                if (!existed) {
                    qCInfo(lcControllerInput)
                        << "Remote input device connected:"
                        << descriptorName(descriptor)
                        << descriptorId(descriptor) << "experimental";
                }
            }
            for (const QVariant &value : previousRemoteDevices) {
                const QVariantMap descriptor = value.toMap();
                bool remains = false;
                for (const QVariant &newValue : m_impl->remoteDescriptors) {
                    if (descriptorId(newValue.toMap())
                        == descriptorId(descriptor)) {
                        remains = true;
                        break;
                    }
                }
                if (!remains) {
                    qCInfo(lcControllerInput)
                        << "Remote input device disconnected:"
                        << descriptorName(descriptor)
                        << descriptorId(descriptor);
                }
            }
            devicesMayHaveChanged = true;
        }
    }
#endif

    QVariantList devices;
#ifdef YANAMI_HAS_SDL3
    for (auto iterator = m_impl->sdlSlots.cbegin();
         iterator != m_impl->sdlSlots.cend(); ++iterator) {
        devices.append(iterator->descriptor);
    }
#endif
#ifdef Q_OS_WIN
    for (const Impl::XInputSlot &slot : m_impl->xinputSlots) {
        if (slot.connected)
            devices.append(slot.descriptor);
    }
#endif
    devices.append(m_impl->remoteDescriptors);
    sortDescriptors(devices);
    if (devicesMayHaveChanged || devices != m_impl->publishedDevices) {
        m_impl->publishedDevices = devices;
        emit devicesChanged();
    }

    const int desiredInterval = anyControllerConnected
        ? PollIntervalMs
        : static_cast<int>(DeviceScanIntervalMs);
    if (m_pollTimer.interval() != desiredInterval)
        m_pollTimer.setInterval(desiredInterval);
}

void ControllerNavigationSource::resetInputStates(bool emitReleases)
{
    const qint64 nowMs = m_elapsedTimer.isValid()
        ? m_elapsedTimer.elapsed() : 0;
    ControllerNavigationState::Snapshot disconnected;
#ifdef YANAMI_HAS_SDL3
    for (auto iterator = m_impl->sdlSlots.begin();
         iterator != m_impl->sdlSlots.end(); ++iterator) {
        if (emitReleases) {
            emitEvents(iterator->navigation.updateActions(disconnected, nowMs),
                       descriptorId(iterator->descriptor));
        } else {
            iterator->navigation.reset();
        }
    }
#endif
#ifdef Q_OS_WIN
    for (Impl::XInputSlot &slot : m_impl->xinputSlots) {
        if (emitReleases) {
            emitEvents(slot.navigation.updateActions(disconnected, nowMs),
                       descriptorId(slot.descriptor));
        } else {
            slot.navigation.reset();
        }
    }
#else
    Q_UNUSED(emitReleases)
    Q_UNUSED(nowMs)
#endif
}

void ControllerNavigationSource::emitEvents(
    const QVector<ControllerNavigationState::ActionEvent> &events,
    const QString &deviceId)
{
    for (const ControllerNavigationState::ActionEvent &event : events) {
        if (event.pressed) {
            emit actionPressed(static_cast<int>(event.action),
                               event.repeated, deviceId);
        } else {
            emit actionReleased(static_cast<int>(event.action), deviceId);
        }
    }
}
