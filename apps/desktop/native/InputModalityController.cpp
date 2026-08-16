#include "InputModalityController.hpp"

#include "ControllerNavigation.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QTabletEvent>
#include <QThread>
#include <QWheelEvent>
#include <QWindow>

InputModalityController::InputModalityController(QObject *parent)
    : QObject(parent)
{
    connect(&InputModalityService::instance(),
            &InputModalityService::modalityChanged,
            this,
            &InputModalityController::modalityChanged);
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

void InputModalityController::dispatchControllerNavigationKeyTo(
    QObject *target,
    int key)
{
    InputModalityService::instance().dispatchControllerNavigationKeyTo(
        target, key);
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
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installEventFilter(this);
    connect(m_controllerNavigation.get(),
            &ControllerNavigationSource::navigationRequested,
            this,
            &InputModalityService::dispatchControllerNavigationKey);
}

InputModalityService::~InputModalityService()
{
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeEventFilter(this);
}

InputModalityController::Modality InputModalityService::modality() const
{
    return m_modality;
}

bool InputModalityService::focusNavigationActive() const
{
    return m_modality == InputModalityController::Modality::Keyboard
        || m_modality == InputModalityController::Modality::Controller;
}

void InputModalityService::notePointerInput()
{
    setModality(InputModalityController::Modality::Pointer);
}

void InputModalityService::noteKeyboardNavigation()
{
    setModality(InputModalityController::Modality::Keyboard);
}

void InputModalityService::noteControllerNavigation()
{
    setModality(InputModalityController::Modality::Controller);
}

bool InputModalityService::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress: {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (m_dispatchingControllerKey)
            noteControllerNavigation();
        else if (!isModifierOnlyKey(keyEvent->key()))
            noteKeyboardNavigation();
        break;
    }
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

void InputModalityService::notePointerPosition(
    const QPoint &globalPosition)
{
    if (m_pointerPositionKnown && globalPosition == m_lastPointerPosition)
        return;

    m_lastPointerPosition = globalPosition;
    m_pointerPositionKnown = true;
    notePointerInput();
}

void InputModalityService::setModality(
    InputModalityController::Modality modality)
{
    if (modality != InputModalityController::Modality::Pointer) {
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
