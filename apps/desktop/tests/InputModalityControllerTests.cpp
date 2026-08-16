#include "InputModalityController.hpp"

#include "ControllerNavigation.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <QWindow>

class KeyEventReceiver final : public QObject
{
public:
    QVector<int> pressedKeys;
    QVector<int> releasedKeys;

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress) {
            pressedKeys.append(static_cast<QKeyEvent *>(event)->key());
            return true;
        }
        if (event->type() == QEvent::KeyRelease) {
            releasedKeys.append(static_cast<QKeyEvent *>(event)->key());
            return true;
        }
        return QObject::event(event);
    }
};

class InputModalityControllerTests final : public QObject
{
    Q_OBJECT

private slots:
    void keyboardAndControllerShareFocusNavigationPresentation();
    void stationaryPointerDoesNotOverrideKeyboard();
    void realPointerInputOverridesFocusNavigation();
    void modifierOnlyKeyDoesNotOverridePointer();
    void controllerStateMapsDirectionsButtonsAndRepeat();
    void controllerDispatchUsesKeyEventsAndPreservesModality();
    void multipleFacadesShareOneDispatchAndState();
};

void InputModalityControllerTests::
    keyboardAndControllerShareFocusNavigationPresentation()
{
    InputModalityController controller;
    controller.notePointerInput();
    QSignalSpy changedSpy(&controller,
                          &InputModalityController::modalityChanged);

    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Pointer);
    QVERIFY(!controller.focusNavigationActive());

    controller.noteKeyboardNavigation();
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Keyboard);
    QVERIFY(controller.focusNavigationActive());

    controller.noteControllerNavigation();
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Controller);
    QVERIFY(controller.focusNavigationActive());
    QCOMPARE(changedSpy.count(), 2);
}

void InputModalityControllerTests::stationaryPointerDoesNotOverrideKeyboard()
{
    InputModalityController controller;
    QWindow window;
    controller.notePointerInput();
    controller.noteKeyboardNavigation();

    const QPointF globalPosition = QCursor::pos();
    QMouseEvent stationaryMove(
        QEvent::MouseMove,
        QPointF(10.0, 10.0),
        globalPosition,
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &stationaryMove);

    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Keyboard);
}

void InputModalityControllerTests::realPointerInputOverridesFocusNavigation()
{
    InputModalityController controller;
    QWindow window;
    controller.notePointerInput();
    controller.noteControllerNavigation();

    const QPointF movedPosition = QPointF(QCursor::pos())
        + QPointF(4.0, 0.0);
    QMouseEvent realMove(
        QEvent::MouseMove,
        QPointF(14.0, 10.0),
        movedPosition,
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &realMove);
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Pointer);

    controller.noteKeyboardNavigation();
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(14.0, 10.0),
        movedPosition,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Pointer);

    controller.noteControllerNavigation();
    QWheelEvent wheel(
        QPointF(14.0, 10.0),
        movedPosition,
        QPoint(),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false);
    QCoreApplication::sendEvent(&window, &wheel);
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Pointer);
}

void InputModalityControllerTests::modifierOnlyKeyDoesNotOverridePointer()
{
    InputModalityController controller;
    QWindow window;
    controller.notePointerInput();
    QKeyEvent shiftPress(
        QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    QCoreApplication::sendEvent(&window, &shiftPress);

    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Pointer);
}

void InputModalityControllerTests::
    controllerStateMapsDirectionsButtonsAndRepeat()
{
    ControllerNavigationState state;
    ControllerNavigationState::Snapshot snapshot{.connected = true};

    QVERIFY(state.update(snapshot, 0).isEmpty());

    snapshot.dpadDown = true;
    QCOMPARE(state.update(snapshot, 10), QVector<int>{Qt::Key_Down});
    QVERIFY(state.update(snapshot, 359).isEmpty());
    QCOMPARE(state.update(snapshot, 360), QVector<int>{Qt::Key_Down});
    QVERIFY(state.update(snapshot, 449).isEmpty());
    QCOMPARE(state.update(snapshot, 450), QVector<int>{Qt::Key_Down});

    snapshot.dpadDown = false;
    snapshot.dpadUp = true;
    QCOMPARE(state.update(snapshot, 451), QVector<int>{Qt::Key_Up});

    snapshot.dpadUp = false;
    snapshot.confirm = true;
    QCOMPARE(state.update(snapshot, 452), QVector<int>{Qt::Key_Return});
    QVERIFY(state.update(snapshot, 453).isEmpty());

    snapshot.confirm = false;
    snapshot.back = true;
    QCOMPARE(state.update(snapshot, 454), QVector<int>{Qt::Key_Escape});

    snapshot.back = false;
    snapshot.leftThumbX =
        ControllerNavigationState::LeftStickThreshold - 1;
    QCOMPARE(ControllerNavigationState::directionFor(snapshot),
             ControllerNavigationState::Direction::None);
    snapshot.leftThumbX = ControllerNavigationState::LeftStickThreshold;
    QCOMPARE(ControllerNavigationState::directionFor(snapshot),
             ControllerNavigationState::Direction::Right);

    snapshot.connected = false;
    QVERIFY(state.update(snapshot, 455).isEmpty());
    snapshot.connected = true;
    snapshot.confirm = true;
    QVERIFY(state.update(snapshot, 456).isEmpty());
}

void InputModalityControllerTests::
    controllerDispatchUsesKeyEventsAndPreservesModality()
{
    InputModalityController controller;
    KeyEventReceiver receiver;
    controller.notePointerInput();
    controller.noteKeyboardNavigation();

    controller.dispatchControllerNavigationKeyTo(
        &receiver, Qt::Key_Down);

    QCOMPARE(receiver.pressedKeys, QVector<int>{Qt::Key_Down});
    QCOMPARE(receiver.releasedKeys, QVector<int>{Qt::Key_Down});
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Controller);
    QVERIFY(controller.focusNavigationActive());
}

void InputModalityControllerTests::multipleFacadesShareOneDispatchAndState()
{
    InputModalityController firstFacade;
    InputModalityController secondFacade;
    KeyEventReceiver receiver;
    firstFacade.notePointerInput();
    QSignalSpy firstSpy(&firstFacade,
                        &InputModalityController::modalityChanged);
    QSignalSpy secondSpy(&secondFacade,
                         &InputModalityController::modalityChanged);

    firstFacade.dispatchControllerNavigationKeyTo(
        &receiver, Qt::Key_Right);

    QCOMPARE(receiver.pressedKeys, QVector<int>{Qt::Key_Right});
    QCOMPARE(receiver.releasedKeys, QVector<int>{Qt::Key_Right});
    QCOMPARE(firstFacade.modality(),
             InputModalityController::Modality::Controller);
    QCOMPARE(secondFacade.modality(),
             InputModalityController::Modality::Controller);
    QCOMPARE(firstSpy.count(), 1);
    QCOMPARE(secondSpy.count(), 1);
}

QTEST_MAIN(InputModalityControllerTests)

#include "InputModalityControllerTests.moc"
