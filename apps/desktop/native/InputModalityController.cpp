#include "InputModalityController.hpp"

#include "ControllerNavigation.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QTabletEvent>
#include <QThread>
#include <QWheelEvent>
#include <QWindow>

#include <array>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Action = InputModalityController::Action;
using Modality = InputModalityController::Modality;

constexpr qint64 RawKeyCorrelationWindowMs = 120;

Q_LOGGING_CATEGORY(lcInputModality, "yanami.input.controller")

QString valueFor(const QVariantMap &descriptor, const char *key)
{
    return descriptor.value(QString::fromLatin1(key)).toString();
}

QVariantMap makeVirtualRemoteDescriptor()
{
    return {
        {QStringLiteral("id"), QStringLiteral("remote:qt-key")},
        {QStringLiteral("name"),
         QStringLiteral("TV Remote / Media Keys")},
        {QStringLiteral("family"), QStringLiteral("remote")},
        {QStringLiteral("supportTier"), QStringLiteral("experimental")},
        {QStringLiteral("backend"), QStringLiteral("qt-key")},
        // This describes one uncorrelated media-key action, not a discovered
        // physical device. It may be active for diagnostics but must never be
        // presented as a connected remote.
        {QStringLiteral("connected"), false},
    };
}

bool remoteKeyPassesThrough(int key)
{
    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Escape:
        return true;
    default:
        return false;
    }
}

int normalizedRemoteKey(int key)
{
    switch (key) {
    case Qt::Key_Select:
        return Qt::Key_Return;
    case Qt::Key_Back:
    case Qt::Key_Exit:
        return Qt::Key_Escape;
    case Qt::Key_Info:
        return Qt::Key_Menu;
    default:
        return Qt::Key_unknown;
    }
}

static_assert(static_cast<int>(Action::NavigateUp)
              == static_cast<int>(ControllerInputAction::NavigateUp));
static_assert(static_cast<int>(Action::NextItem)
              == static_cast<int>(ControllerInputAction::NextItem));

} // namespace

InputModalityController::InputModalityController(QObject *parent)
    : QObject(parent)
{
    InputModalityService &service = InputModalityService::instance();
    connect(&service, &InputModalityService::modalityChanged,
            this, &InputModalityController::modalityChanged);
    connect(&service, &InputModalityService::actionPressed,
            this, &InputModalityController::actionPressed);
    connect(&service, &InputModalityService::actionReleased,
            this, &InputModalityController::actionReleased);
    connect(&service, &InputModalityService::controllerInputTestAction,
            this, &InputModalityController::controllerInputTestAction);
    connect(&service, &InputModalityService::devicesChanged,
            this, &InputModalityController::devicesChanged);
    connect(&service, &InputModalityService::activeDeviceChanged,
            this, &InputModalityController::activeDeviceChanged);
    connect(&service, &InputModalityService::controllerBackendChanged,
            this, &InputModalityController::controllerBackendChanged);
    connect(&service, &InputModalityService::lastActionChanged,
            this, &InputModalityController::lastActionChanged);
    connect(&service,
            &InputModalityService::controllerInputTestActiveChanged,
            this,
            &InputModalityController::controllerInputTestActiveChanged);
}

InputModalityController::~InputModalityController() = default;

InputModalityController::Modality InputModalityController::modality() const
{
    return InputModalityService::instance().modality();
}

bool InputModalityController::focusNavigationActive() const
{
    return InputModalityService::instance().focusNavigationActive();
}

bool InputModalityController::controllerConnected() const
{
    return InputModalityService::instance().controllerConnected();
}

QString InputModalityController::activeDeviceName() const
{
    return InputModalityService::instance().activeDeviceName();
}

QString InputModalityController::activeDeviceFamily() const
{
    return InputModalityService::instance().activeDeviceFamily();
}

QString InputModalityController::activeSupportTier() const
{
    return InputModalityService::instance().activeSupportTier();
}

QString InputModalityController::controllerBackend() const
{
    return InputModalityService::instance().controllerBackend();
}

QVariantList InputModalityController::connectedDevices() const
{
    return InputModalityService::instance().connectedDevices();
}

QString InputModalityController::lastActionName() const
{
    return InputModalityService::instance().lastActionName();
}

bool InputModalityController::controllerInputTestActive() const
{
    return InputModalityService::instance().controllerInputTestActive();
}

void InputModalityController::notePointerInput()
{
    InputModalityService::instance().notePointerInput();
}

void InputModalityController::noteKeyboardNavigation()
{
    InputModalityService::instance().noteKeyboardNavigation();
}

void InputModalityController::noteControllerNavigation()
{
    InputModalityService::instance().noteControllerNavigation();
}

QString InputModalityController::promptForAction(Action action) const
{
    return InputModalityService::instance().promptForAction(action);
}

bool InputModalityController::acquireControllerInputTest(QObject *owner)
{
    return InputModalityService::instance().acquireControllerInputTest(owner);
}

void InputModalityController::releaseControllerInputTest(QObject *owner)
{
    InputModalityService::instance().releaseControllerInputTest(owner);
}

void InputModalityController::dispatchControllerNavigationKeyTo(
    QObject *target,
    int key)
{
    InputModalityService::instance().dispatchControllerNavigationKeyTo(
        target, key);
}

void InputModalityController::dispatchActionForTest(
    Action action,
    bool repeated,
    const QVariantMap &device,
    QObject *legacyTarget)
{
    InputModalityService::instance().dispatchActionForTest(
        action, repeated, device, legacyTarget);
}

InputModalityService &InputModalityService::instance()
{
    QCoreApplication *application = QCoreApplication::instance();
    if (!application
        || QThread::currentThread() != application->thread()) {
        qFatal("InputModalityService must be used on the application thread");
    }

    static QPointer<InputModalityService> service;
    if (!service)
        service = new InputModalityService(application);
    return *service;
}

InputModalityService::InputModalityService(QObject *parent)
    : QObject(parent)
    , m_controllerNavigation(
          std::make_unique<ControllerNavigationSource>())
    , m_lastPointerPosition(QCursor::pos())
    , m_pointerPositionKnown(true)
{
    m_inputClock.start();
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
#ifdef Q_OS_WIN
        QCoreApplication::instance()->installNativeEventFilter(this);
        const std::array<RAWINPUTDEVICE, 2> rawDevices{{
            {.usUsagePage = 0x01, .usUsage = 0x06,
             .dwFlags = 0, .hwndTarget = nullptr},
            {.usUsagePage = 0x0c, .usUsage = 0x01,
             .dwFlags = 0, .hwndTarget = nullptr},
        }};
        m_rawInputRegistered = RegisterRawInputDevices(
            rawDevices.data(), static_cast<UINT>(rawDevices.size()),
            sizeof(RAWINPUTDEVICE));
        if (!m_rawInputRegistered) {
            qCWarning(lcInputModality)
                << "Raw Input remote correlation is unavailable;"
                << "Qt key fallback remains active. Error:"
                << GetLastError();
        }
#endif
    }
    connect(m_controllerNavigation.get(),
            &ControllerNavigationSource::actionPressed,
            this,
            &InputModalityService::handleControllerActionPressed);
    connect(m_controllerNavigation.get(),
            &ControllerNavigationSource::actionReleased,
            this,
            &InputModalityService::handleControllerActionReleased);
    connect(m_controllerNavigation.get(),
            &ControllerNavigationSource::devicesChanged,
            this,
            &InputModalityService::handleSourceDevicesChanged);
    connect(m_controllerNavigation.get(),
            &ControllerNavigationSource::backendChanged,
            this,
            &InputModalityService::controllerBackendChanged);
}

InputModalityService::~InputModalityService()
{
    if (QCoreApplication::instance()) {
#ifdef Q_OS_WIN
        if (m_rawInputRegistered) {
            const std::array<RAWINPUTDEVICE, 2> rawDevices{{
                {.usUsagePage = 0x01, .usUsage = 0x06,
                 .dwFlags = RIDEV_REMOVE, .hwndTarget = nullptr},
                {.usUsagePage = 0x0c, .usUsage = 0x01,
                 .dwFlags = RIDEV_REMOVE, .hwndTarget = nullptr},
            }};
            RegisterRawInputDevices(
                rawDevices.data(), static_cast<UINT>(rawDevices.size()),
                sizeof(RAWINPUTDEVICE));
        }
        QCoreApplication::instance()->removeNativeEventFilter(this);
#endif
        QCoreApplication::instance()->removeEventFilter(this);
    }
}

InputModalityController::Modality InputModalityService::modality() const
{
    return m_modality;
}

bool InputModalityService::focusNavigationActive() const
{
    return m_modality == Modality::Keyboard
        || m_modality == Modality::Controller
        || m_modality == Modality::Remote;
}

bool InputModalityService::controllerConnected() const
{
    for (const QVariant &value : connectedDevices()) {
        const QVariantMap descriptor = value.toMap();
        const QString family = valueFor(descriptor, "family");
        if (family != QStringLiteral("remote")
            && descriptor.value(QStringLiteral("connected")).toBool()) {
            return true;
        }
    }
    return false;
}

QString InputModalityService::activeDeviceName() const
{
    return valueFor(m_activeDevice, "name");
}

QString InputModalityService::activeDeviceFamily() const
{
    return valueFor(m_activeDevice, "family");
}

QString InputModalityService::activeSupportTier() const
{
    return valueFor(m_activeDevice, "supportTier");
}

QString InputModalityService::controllerBackend() const
{
    return m_controllerNavigation->backend();
}

QVariantList InputModalityService::connectedDevices() const
{
    return m_controllerNavigation->connectedDevices();
}

QString InputModalityService::lastActionName() const
{
    return m_lastActionName;
}

bool InputModalityService::controllerInputTestActive() const
{
    return !m_controllerInputTestOwner.isNull();
}

bool InputModalityService::acquireControllerInputTest(QObject *owner)
{
    if (!owner)
        return false;
    if (m_controllerInputTestOwner == owner)
        return true;
    if (controllerInputTestActive())
        return false;

    disconnect(m_controllerInputTestOwnerDestroyed);
    m_controllerInputTestOwner = owner;
    const quint64 generation = ++m_controllerInputTestOwnerGeneration;
    m_controllerInputTestOwnerDestroyed = connect(
        owner,
        &QObject::destroyed,
        this,
        [this, generation] {
            if (generation != m_controllerInputTestOwnerGeneration)
                return;
            m_controllerInputTestOwner.clear();
            m_controllerInputTestOwnerDestroyed = {};
            emit controllerInputTestActiveChanged();
        });
    emit controllerInputTestActiveChanged();
    return true;
}

void InputModalityService::releaseControllerInputTest(QObject *owner)
{
    if (!owner || m_controllerInputTestOwner != owner)
        return;

    disconnect(m_controllerInputTestOwnerDestroyed);
    m_controllerInputTestOwnerDestroyed = {};
    ++m_controllerInputTestOwnerGeneration;
    m_controllerInputTestOwner.clear();
    emit controllerInputTestActiveChanged();
}

QString InputModalityService::promptForAction(Action action) const
{
    const QString family = activeDeviceFamily();
    const bool playStation = family == QStringLiteral("playstation");
    const bool nintendo = family == QStringLiteral("nintendo");
    const bool remote = family == QStringLiteral("remote");
    const bool xbox = family == QStringLiteral("xbox");

    switch (action) {
    case Action::NavigateUp:
    case Action::NavigateDown:
    case Action::NavigateLeft:
    case Action::NavigateRight:
        return remote ? QStringLiteral("D-pad")
                      : QStringLiteral("D-pad / Left Stick");
    case Action::Activate:
        if (playStation)
            return QStringLiteral("Cross");
        if (nintendo)
            return QStringLiteral("A");
        if (remote)
            return QStringLiteral("OK");
        return xbox ? QStringLiteral("A") : QStringLiteral("Select");
    case Action::Back:
        if (playStation)
            return QStringLiteral("Circle");
        if (nintendo || xbox)
            return QStringLiteral("B");
        return remote ? QStringLiteral("Back") : QStringLiteral("Cancel");
    case Action::Context:
        if (playStation)
            return QStringLiteral("Square");
        if (nintendo)
            return QStringLiteral("Y");
        if (xbox)
            return QStringLiteral("X");
        return remote ? QStringLiteral("Info") : QStringLiteral("Context");
    case Action::Menu:
        if (playStation)
            return QStringLiteral("Options");
        if (nintendo)
            return QStringLiteral("+");
        return remote ? QStringLiteral("Menu") : QStringLiteral("Menu");
    case Action::Search:
        if (playStation)
            return QStringLiteral("Triangle");
        if (nintendo)
            return QStringLiteral("X");
        if (xbox)
            return QStringLiteral("Y");
        return QStringLiteral("Search");
    case Action::PagePrevious:
        if (playStation)
            return QStringLiteral("L1");
        if (nintendo)
            return QStringLiteral("L");
        return remote ? QStringLiteral("Page Up") : QStringLiteral("LB");
    case Action::PageNext:
        if (playStation)
            return QStringLiteral("R1");
        if (nintendo)
            return QStringLiteral("R");
        return remote ? QStringLiteral("Page Down") : QStringLiteral("RB");
    case Action::ScrollUp:
    case Action::ScrollDown:
    case Action::ScrollLeft:
    case Action::ScrollRight:
        return remote ? QStringLiteral("D-pad")
                      : QStringLiteral("Right Stick");
    case Action::SeekBackward:
        if (playStation)
            return QStringLiteral("L2");
        if (nintendo)
            return QStringLiteral("ZL");
        return remote ? QStringLiteral("Rewind") : QStringLiteral("LT");
    case Action::SeekForward:
        if (playStation)
            return QStringLiteral("R2");
        if (nintendo)
            return QStringLiteral("ZR");
        return remote ? QStringLiteral("Fast Forward") : QStringLiteral("RT");
    case Action::PlayPause:
        return remote ? QStringLiteral("Play / Pause")
                      : promptForAction(Action::Activate);
    case Action::VolumeUp:
        return QStringLiteral("Volume Up");
    case Action::VolumeDown:
        return QStringLiteral("Volume Down");
    case Action::PreviousItem:
        return remote ? QStringLiteral("Previous")
                      : promptForAction(Action::PagePrevious);
    case Action::NextItem:
        return remote ? QStringLiteral("Next")
                      : promptForAction(Action::PageNext);
    case Action::PageUp:
        return QStringLiteral("Page Up");
    case Action::PageDown:
        return QStringLiteral("Page Down");
    }
    return {};
}

void InputModalityService::notePointerInput()
{
    setModality(Modality::Pointer);
}

void InputModalityService::noteKeyboardNavigation()
{
    setModality(Modality::Keyboard);
}

void InputModalityService::noteControllerNavigation()
{
    setModality(Modality::Controller);
}

bool InputModalityService::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress: {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (m_dispatchingControllerKey) {
            // The originating semantic source has already set modality.
        } else {
            bool consume = false;
            if (handleRemoteKeyEvent(watched, keyEvent, consume)) {
                if (consume)
                    return true;
            } else if (!isModifierOnlyKey(keyEvent->key())) {
                noteKeyboardNavigation();
            }
        }
        break;
    }
    case QEvent::KeyRelease:
        if (!m_dispatchingControllerKey) {
            bool consume = false;
            if (handleRemoteKeyEvent(watched,
                                     static_cast<QKeyEvent *>(event),
                                     consume)
                && consume) {
                return true;
            }
        }
        break;
    case QEvent::MouseMove: {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        notePointerPosition(mouseEvent->globalPosition().toPoint());
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        m_lastPointerPosition = mouseEvent->globalPosition().toPoint();
        m_pointerPositionKnown = true;
        notePointerInput();
        break;
    }
    case QEvent::Wheel: {
        const auto *wheelEvent = static_cast<QWheelEvent *>(event);
        m_lastPointerPosition = wheelEvent->globalPosition().toPoint();
        m_pointerPositionKnown = true;
        notePointerInput();
        break;
    }
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        notePointerInput();
        break;
    case QEvent::TabletMove: {
        const auto *tabletEvent = static_cast<QTabletEvent *>(event);
        notePointerPosition(tabletEvent->globalPosition().toPoint());
        break;
    }
    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}

#ifdef Q_OS_WIN
bool InputModalityService::nativeEventFilter(const QByteArray &eventType,
                                             void *message,
                                             qintptr *result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)
    const auto *nativeMessage = static_cast<MSG *>(message);
    if (!nativeMessage || nativeMessage->message != WM_INPUT)
        return false;

    UINT byteCount = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(nativeMessage->lParam),
                        RID_INPUT, nullptr, &byteCount,
                        sizeof(RAWINPUTHEADER)) != 0
        || byteCount < sizeof(RAWINPUTHEADER)) {
        return false;
    }
    QByteArray bytes(static_cast<qsizetype>(byteCount), Qt::Uninitialized);
    UINT requestedBytes = byteCount;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(nativeMessage->lParam),
                        RID_INPUT, bytes.data(), &requestedBytes,
                        sizeof(RAWINPUTHEADER))
        != byteCount) {
        return false;
    }
    const auto *rawInput = reinterpret_cast<const RAWINPUT *>(bytes.constData());
    if (rawInput->header.dwType != RIM_TYPEKEYBOARD)
        return false;

    const quintptr handle = reinterpret_cast<quintptr>(
        rawInput->header.hDevice);
    const QString remoteDeviceId =
        m_controllerNavigation->remoteDeviceIdForNativeHandle(handle);
    if (remoteDeviceId.isEmpty()) {
        // A real keyboard event is authoritative evidence for an immediate
        // modality handoff. It must also invalidate an earlier remote event
        // so an equal VKey cannot be correlated within the 120 ms window.
        clearRawKeyCorrelation();
        return false;
    }

    m_lastRawDeviceHandle = handle;
    m_lastRawVirtualKey = rawInput->data.keyboard.VKey;
    m_lastRawPressed =
        (rawInput->data.keyboard.Flags & RI_KEY_BREAK) == 0;
    m_lastRawEventAtMs = m_inputClock.elapsed();
    return false;
}
#endif

void InputModalityService::dispatchControllerNavigationKey(int key)
{
    if (QWindow *window = QGuiApplication::focusWindow())
        dispatchControllerNavigationKeyTo(window, key);
}

void InputModalityService::dispatchControllerNavigationKeyTo(
    QObject *target,
    int key)
{
    if (!target)
        return;

    noteControllerNavigation();
    QScopedValueRollback dispatchGuard(m_dispatchingControllerKey, true);
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &release);
}

void InputModalityService::dispatchActionForTest(
    Action action,
    bool repeated,
    const QVariantMap &device,
    QObject *legacyTarget)
{
    const Modality sourceModality = valueFor(device, "family")
            == QStringLiteral("remote")
        ? Modality::Remote : Modality::Controller;
    const bool captured = routeActionPressed(
        action, repeated, device, sourceModality);
    if (!captured && legacyTarget) {
        const int key = legacyQtKeyForAction(action);
        if (key != Qt::Key_unknown)
            dispatchNormalizedRemoteKey(legacyTarget, key);
    }
}

bool InputModalityService::rawKeyEventMatchesRemote(
    const QString &remoteDeviceId,
    quint32 rawVirtualKey,
    bool rawPressed,
    quint32 qtVirtualKey,
    bool qtPressed,
    qint64 ageMs)
{
    return !remoteDeviceId.isEmpty()
        && rawVirtualKey != 0
        && qtVirtualKey != 0
        && rawVirtualKey == qtVirtualKey
        && rawPressed == qtPressed
        && ageMs >= 0
        && ageMs <= RawKeyCorrelationWindowMs;
}

void InputModalityService::handleControllerActionPressed(
    int action,
    bool repeated,
    const QString &deviceId)
{
    if (action < static_cast<int>(Action::NavigateUp)
        || action > static_cast<int>(Action::NextItem)) {
        return;
    }
    const Action semanticAction = static_cast<Action>(action);
    const auto capturedIterator = m_capturedControllerActions.constFind(
        deviceId);
    if (capturedIterator != m_capturedControllerActions.cend()
        && capturedIterator->contains(action)) {
        if (controllerInputTestActive()) {
            (void) routeActionPressed(
                semanticAction,
                repeated,
                m_controllerNavigation->deviceDescriptor(deviceId),
                Modality::Controller);
        }
        return;
    }

    const bool captured = routeActionPressed(
        semanticAction,
        repeated,
        m_controllerNavigation->deviceDescriptor(deviceId),
        Modality::Controller);
    if (captured) {
        m_capturedControllerActions[deviceId].insert(action);
        return;
    }
    dispatchLegacyKeyForAction(semanticAction);
}

void InputModalityService::handleControllerActionReleased(
    int action,
    const QString &deviceId)
{
    if (action < static_cast<int>(Action::NavigateUp)
        || action > static_cast<int>(Action::NextItem)) {
        return;
    }

    bool capturedSequence = false;
    auto capturedIterator = m_capturedControllerActions.find(deviceId);
    if (capturedIterator != m_capturedControllerActions.end()) {
        capturedSequence = capturedIterator->remove(action);
        if (capturedIterator->isEmpty())
            m_capturedControllerActions.erase(capturedIterator);
    }
    if (capturedSequence || controllerInputTestActive())
        return;
    emitActionReleased(static_cast<Action>(action));
}

void InputModalityService::handleSourceDevicesChanged()
{
    QSet<QString> connectedIds;
    for (const QVariant &value : connectedDevices()) {
        const QVariantMap descriptor = value.toMap();
        if (descriptor.value(QStringLiteral("connected")).toBool())
            connectedIds.insert(valueFor(descriptor, "id"));
    }
    for (auto iterator = m_capturedControllerActions.begin();
         iterator != m_capturedControllerActions.end();) {
        if (!connectedIds.contains(iterator.key()))
            iterator = m_capturedControllerActions.erase(iterator);
        else
            ++iterator;
    }

    const QString activeId = valueFor(m_activeDevice, "id");
    if (!activeId.isEmpty()
        && activeId != valueFor(m_virtualRemoteDevice, "id")) {
        QVariantMap refreshed;
        for (const QVariant &value : connectedDevices()) {
            const QVariantMap descriptor = value.toMap();
            if (valueFor(descriptor, "id") == activeId) {
                refreshed = descriptor;
                break;
            }
        }
        if (refreshed.isEmpty()) {
            m_activeDevice.clear();
            emit activeDeviceChanged();
        } else if (refreshed != m_activeDevice) {
            m_activeDevice = refreshed;
            emit activeDeviceChanged();
        }
    }
    emit devicesChanged();
}

bool InputModalityService::handleRemoteKeyEvent(QObject *target,
                                                QKeyEvent *event,
                                                bool &consume)
{
    consume = false;
    if (!event)
        return false;

    if (event->type() == QEvent::KeyRelease) {
        const auto iterator = m_remotePressedKeys.find(event->key());
        const bool capturedSequence = m_capturedRemoteKeys.remove(
            event->key());
        if (iterator == m_remotePressedKeys.end()) {
            if (capturedSequence) {
                clearRawKeyCorrelation();
                consume = true;
                return true;
            }
            return false;
        }
        if (!capturedSequence && !controllerInputTestActive())
            emitActionReleased(iterator.value());
        m_remotePressedKeys.erase(iterator);
        clearRawKeyCorrelation();
        consume = capturedSequence || controllerInputTestActive()
            || !remoteKeyPassesThrough(event->key());
        return true;
    }

    Action action = Action::Activate;
    if (!remoteActionForKey(event->key(), action))
        return false;
    const bool capturedSequence = m_capturedRemoteKeys.contains(event->key());
    const QVariantMap device = remoteDescriptorForKeyEvent(*event);
    if (capturedSequence) {
        if (controllerInputTestActive()) {
            (void) routeActionPressed(action,
                                      event->isAutoRepeat(),
                                      device.isEmpty() ? m_activeDevice : device,
                                      Modality::Remote);
        }
        consume = true;
        return true;
    }
    if (device.isEmpty())
        return false;

    if (!event->isAutoRepeat())
        m_remotePressedKeys.insert(event->key(), action);
    const bool captured = routeActionPressed(
        action, event->isAutoRepeat(), device, Modality::Remote);
    if (captured) {
        m_remotePressedKeys.insert(event->key(), action);
        m_capturedRemoteKeys.insert(event->key());
        consume = true;
        return true;
    }
    consume = !remoteKeyPassesThrough(event->key());
    const int normalizedKey = normalizedRemoteKey(event->key());
    if (normalizedKey != Qt::Key_unknown && !event->isAutoRepeat())
        dispatchNormalizedRemoteKey(target, normalizedKey);
    return true;
}

bool InputModalityService::routeActionPressed(
    Action action,
    bool repeated,
    const QVariantMap &device,
    Modality sourceModality)
{
    setModality(sourceModality);
    markActiveDevice(device);
    m_lastActionName = actionName(action);
    // Repeated actions intentionally notify as well: the settings input test
    // uses this as a live activity pulse even when the string is unchanged.
    emit lastActionChanged();
    if (controllerInputTestActive()) {
        emit controllerInputTestAction(action, repeated);
        return true;
    }
    emit actionPressed(action, repeated);
    return false;
}

void InputModalityService::emitActionReleased(Action action)
{
    emit actionReleased(action);
}

void InputModalityService::markActiveDevice(const QVariantMap &device)
{
    if (device.isEmpty() || device == m_activeDevice)
        return;
    m_activeDevice = device;
    emit activeDeviceChanged();
}

QVariantMap InputModalityService::remoteDescriptorForKeyEvent(
    const QKeyEvent &event)
{
    const qint64 nowMs = m_inputClock.elapsed();
    QString correlatedDeviceId;
#ifdef Q_OS_WIN
    const bool isPress = event.type() == QEvent::KeyPress;
    const quint32 nativeVirtualKey = event.nativeVirtualKey();
    const QString candidateDeviceId =
        m_controllerNavigation->remoteDeviceIdForNativeHandle(
            m_lastRawDeviceHandle);
    if (rawKeyEventMatchesRemote(
            candidateDeviceId,
            m_lastRawVirtualKey,
            m_lastRawPressed,
            nativeVirtualKey,
            isPress,
            nowMs - m_lastRawEventAtMs)) {
        correlatedDeviceId = candidateDeviceId;
    }
    // A Raw Input observation is single-use regardless of whether it matched.
    // This prevents a later equal VKey from inheriting stale device identity.
    clearRawKeyCorrelation();
#endif
    if (!correlatedDeviceId.isEmpty()) {
        return m_controllerNavigation->deviceDescriptor(correlatedDeviceId);
    }

    if (isRemoteSpecificKey(event.key())) {
        // Without a matching WM_INPUT keyboard handle, a media key may have
        // come from an ordinary multimedia keyboard. Represent only this
        // action as the generic Qt-key source and do not start a remote
        // session that would misclassify subsequent keyboard arrows.
        if (m_virtualRemoteDevice.isEmpty()) {
            m_virtualRemoteDevice = makeVirtualRemoteDescriptor();
        }
        return m_virtualRemoteDevice;
    }

    return {};
}

void InputModalityService::clearRawKeyCorrelation()
{
#ifdef Q_OS_WIN
    m_lastRawDeviceHandle = 0;
    m_lastRawVirtualKey = 0;
    m_lastRawPressed = false;
    m_lastRawEventAtMs = -1;
#endif
}

void InputModalityService::dispatchNormalizedRemoteKey(QObject *target,
                                                       int key)
{
    if (!target || key == Qt::Key_unknown)
        return;
    QScopedValueRollback dispatchGuard(m_dispatchingControllerKey, true);
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &release);
}

void InputModalityService::dispatchLegacyKeyForAction(Action action)
{
    const int key = legacyQtKeyForAction(action);
    if (key != Qt::Key_unknown)
        dispatchControllerNavigationKey(key);
}

bool InputModalityService::isModifierOnlyKey(int key)
{
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
        return true;
    default:
        return false;
    }
}

QString InputModalityService::actionName(Action action)
{
    static constexpr std::array<const char *, 24> Names{
        "navigateUp", "navigateDown", "navigateLeft", "navigateRight",
        "activate", "back", "context", "menu", "search",
        "pagePrevious", "pageNext", "pageUp", "pageDown",
        "scrollUp", "scrollDown", "scrollLeft", "scrollRight",
        "playPause", "seekBackward", "seekForward", "volumeUp",
        "volumeDown", "previousItem", "nextItem",
    };
    const int index = static_cast<int>(action);
    if (index < 0 || index >= static_cast<int>(Names.size()))
        return {};
    return QString::fromLatin1(Names[static_cast<std::size_t>(index)]);
}

bool InputModalityService::isRemoteSpecificKey(int key)
{
    switch (key) {
    case Qt::Key_Select:
    case Qt::Key_Back:
    case Qt::Key_Exit:
    case Qt::Key_Menu:
    case Qt::Key_Guide:
    case Qt::Key_Info:
    case Qt::Key_Search:
    case Qt::Key_VolumeUp:
    case Qt::Key_VolumeDown:
    case Qt::Key_MediaPlay:
    case Qt::Key_MediaPause:
    case Qt::Key_MediaTogglePlayPause:
    case Qt::Key_MediaPrevious:
    case Qt::Key_MediaNext:
    case Qt::Key_AudioRewind:
    case Qt::Key_AudioForward:
    case Qt::Key_ChannelUp:
    case Qt::Key_ChannelDown:
        return true;
    default:
        return false;
    }
}

bool InputModalityService::remoteActionForKey(int key, Action &action)
{
    switch (key) {
    case Qt::Key_Up:
        action = Action::NavigateUp;
        return true;
    case Qt::Key_Down:
        action = Action::NavigateDown;
        return true;
    case Qt::Key_Left:
        action = Action::NavigateLeft;
        return true;
    case Qt::Key_Right:
        action = Action::NavigateRight;
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Select:
        action = Action::Activate;
        return true;
    case Qt::Key_Escape:
    case Qt::Key_Back:
    case Qt::Key_Exit:
        action = Action::Back;
        return true;
    case Qt::Key_Info:
        action = Action::Context;
        return true;
    case Qt::Key_Menu:
    case Qt::Key_Guide:
        action = Action::Menu;
        return true;
    case Qt::Key_Search:
        action = Action::Search;
        return true;
    case Qt::Key_PageUp:
        action = Action::PageUp;
        return true;
    case Qt::Key_PageDown:
        action = Action::PageDown;
        return true;
    case Qt::Key_MediaPlay:
    case Qt::Key_MediaPause:
    case Qt::Key_MediaTogglePlayPause:
        action = Action::PlayPause;
        return true;
    case Qt::Key_VolumeUp:
        action = Action::VolumeUp;
        return true;
    case Qt::Key_VolumeDown:
        action = Action::VolumeDown;
        return true;
    case Qt::Key_AudioRewind:
        action = Action::SeekBackward;
        return true;
    case Qt::Key_AudioForward:
        action = Action::SeekForward;
        return true;
    case Qt::Key_MediaPrevious:
    case Qt::Key_ChannelUp:
        action = Action::PreviousItem;
        return true;
    case Qt::Key_MediaNext:
    case Qt::Key_ChannelDown:
        action = Action::NextItem;
        return true;
    default:
        return false;
    }
}

int InputModalityService::legacyQtKeyForAction(Action action)
{
    switch (action) {
    case Action::NavigateUp:
        return Qt::Key_Up;
    case Action::NavigateDown:
        return Qt::Key_Down;
    case Action::NavigateLeft:
        return Qt::Key_Left;
    case Action::NavigateRight:
        return Qt::Key_Right;
    case Action::Activate:
        return Qt::Key_Return;
    case Action::Back:
        return Qt::Key_Escape;
    case Action::Context:
        return Qt::Key_Menu;
    default:
        return Qt::Key_unknown;
    }
}

void InputModalityService::notePointerPosition(
    const QPoint &globalPosition)
{
    if (m_pointerPositionKnown && globalPosition == m_lastPointerPosition)
        return;

    m_lastPointerPosition = globalPosition;
    m_pointerPositionKnown = true;
    notePointerInput();
}

void InputModalityService::setModality(Modality modality)
{
    if (modality != Modality::Pointer) {
        // Seed from the operating-system cursor so geometry/focus updates at
        // the same physical point cannot masquerade as a mouse movement.
        m_lastPointerPosition = QCursor::pos();
        m_pointerPositionKnown = true;
    }

    if (m_modality == modality)
        return;
    m_modality = modality;
    emit modalityChanged();
}
