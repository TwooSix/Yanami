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

#include <limits>

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
    void controllerSourceConstructionAndShutdownStayLazy();
    void controllerSourceActivationIsExplicitAndIdempotent();
    void keyboardAndControllerShareFocusNavigationPresentation();
    void stationaryPointerDoesNotOverrideKeyboard();
    void realPointerInputOverridesFocusNavigation();
    void modifierOnlyKeyDoesNotOverridePointer();
    void ordinaryArrowRemainsKeyboardWithoutRemoteEvidence();
    void remoteMediaKeysUseSemanticPipelineWithoutReinjection();
    void remoteKeysNormalizeOrStaySemanticOnly();
    void rawRemoteCorrelationRequiresExactFreshDeviceMatch();
    void remoteActionThenKeyboardArrowSwitchesImmediately();
    void controllerStateMapsDirectionsButtonsAndRepeat();
    void controllerNeutralGateRejectsHeldInputUntilReleased();
    void controllerSemanticActionsIncludeReleaseAndScroll();
    void controllerFamilyClassificationCoversEverySdlBoundary();
    void enumerationSuccessPolicyDistinguishesEmptyFromFailure();
    void controllerFaceButtonsFollowPlatformConvention();
    void verticalAxisInversionSaturatesAtNativeMinimum();
    void controllerDispatchUsesKeyEventsAndPreservesModality();
    void semanticActionUpdatesDeviceAndDiagnostics();
    void controllerInputTestOwnershipIsExclusiveAndOwnerBound();
    void controllerInputTestSuppressesGlobalAndLegacyDispatch();
    void controllerInputTestKeepsCapturedSequenceNeutralUntilRelease();
    void controllerInputTestConsumesRemoteKeysWithoutReinjection();
    void multipleFacadesShareOneDispatchAndState();
};

void InputModalityControllerTests::
    controllerSourceConstructionAndShutdownStayLazy()
{
    auto service = std::unique_ptr<InputModalityService>(
        new InputModalityService);

    QVERIFY(!service->m_controllerNavigation);
    QCOMPARE(service->controllerBackend(), QStringLiteral("none"));
    QVERIFY(service->connectedDevices().isEmpty());

    // Destruction before activation must not assume that an SDL/XInput source
    // or its timers and application-state connections ever existed.
    service.reset();
}

void InputModalityControllerTests::
    controllerSourceActivationIsExplicitAndIdempotent()
{
    auto service = std::unique_ptr<InputModalityService>(
        new InputModalityService);
    QSignalSpy backendSpy(
        service.get(), &InputModalityService::controllerBackendChanged);

    QVERIFY(!service->m_controllerNavigation);

    service->initializeControllerNavigation();
    QVERIFY(service->m_controllerNavigation);
    ControllerNavigationSource *const source =
        service->m_controllerNavigation.get();
    const int signalCountAfterInitialization = backendSpy.count();
    if (service->controllerBackend() == QStringLiteral("none"))
        QCOMPARE(signalCountAfterInitialization, 0);
    else
        QCOMPARE(signalCountAfterInitialization, 1);

    service->initializeControllerNavigation();
    QCOMPARE(service->m_controllerNavigation.get(), source);
    QCOMPARE(backendSpy.count(), signalCountAfterInitialization);

    // Explicit shutdown after activation owns and tears down the source once.
    service.reset();
}

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
    ordinaryArrowRemainsKeyboardWithoutRemoteEvidence()
{
    InputModalityController controller;
    QWindow window;
    controller.notePointerInput();
    QSignalSpy actionSpy(&controller,
                         &InputModalityController::actionPressed);
    QKeyEvent arrowPress(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &arrowPress);

    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Keyboard);
    QCOMPARE(actionSpy.count(), 0);
}

void InputModalityControllerTests::
    remoteMediaKeysUseSemanticPipelineWithoutReinjection()
{
    InputModalityController controller;
    KeyEventReceiver receiver;
    controller.notePointerInput();
    QSignalSpy pressedSpy(&controller,
                          &InputModalityController::actionPressed);
    QSignalSpy releasedSpy(&controller,
                           &InputModalityController::actionReleased);

    QKeyEvent mediaPress(QEvent::KeyPress,
                         Qt::Key_MediaTogglePlayPause,
                         Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &mediaPress);
    QKeyEvent mediaRelease(QEvent::KeyRelease,
                           Qt::Key_MediaTogglePlayPause,
                           Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &mediaRelease);

    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Remote);
    QCOMPARE(controller.activeDeviceFamily(), QStringLiteral("remote"));
    QCOMPARE(controller.activeSupportTier(),
             QStringLiteral("experimental"));
    QCOMPARE(controller.lastActionName(), QStringLiteral("playPause"));
    QCOMPARE(pressedSpy.count(), 1);
    QCOMPARE(pressedSpy.at(0).at(0).value<InputModalityController::Action>(),
             InputModalityController::Action::PlayPause);
    QCOMPARE(releasedSpy.count(), 1);
    // Semantic-only media keys are consumed and never re-injected.
    QVERIFY(receiver.pressedKeys.isEmpty());
    QVERIFY(receiver.releasedKeys.isEmpty());

    bool foundVirtualRemote = false;
    for (const QVariant &value : controller.connectedDevices()) {
        const QVariantMap descriptor = value.toMap();
        if (descriptor.value(QStringLiteral("id")).toString()
            == QStringLiteral("remote:qt-key")) {
            foundVirtualRemote = true;
        }
    }
    QVERIFY(!foundVirtualRemote);

    QKeyEvent ordinaryArrow(QEvent::KeyPress, Qt::Key_Right,
                            Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &ordinaryArrow);
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Keyboard);
    QCOMPARE(pressedSpy.count(), 1);
}

void InputModalityControllerTests::remoteKeysNormalizeOrStaySemanticOnly()
{
    InputModalityController controller;
    KeyEventReceiver receiver;
    QSignalSpy pressedSpy(&controller,
                          &InputModalityController::actionPressed);

    auto sendKey = [&receiver](int key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(&receiver, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(&receiver, &release);
    };

    sendKey(Qt::Key_Select);
    QCOMPARE(receiver.pressedKeys, QVector<int>{Qt::Key_Return});
    QCOMPARE(receiver.releasedKeys, QVector<int>{Qt::Key_Return});
    QCOMPARE(pressedSpy.takeFirst().at(0)
                 .value<InputModalityController::Action>(),
             InputModalityController::Action::Activate);

    receiver.pressedKeys.clear();
    receiver.releasedKeys.clear();
    sendKey(Qt::Key_Back);
    QCOMPARE(receiver.pressedKeys, QVector<int>{Qt::Key_Escape});
    QCOMPARE(receiver.releasedKeys, QVector<int>{Qt::Key_Escape});
    QCOMPARE(pressedSpy.takeFirst().at(0)
                 .value<InputModalityController::Action>(),
             InputModalityController::Action::Back);

    receiver.pressedKeys.clear();
    receiver.releasedKeys.clear();
    sendKey(Qt::Key_Info);
    QCOMPARE(receiver.pressedKeys, QVector<int>{Qt::Key_Menu});
    QCOMPARE(receiver.releasedKeys, QVector<int>{Qt::Key_Menu});
    QCOMPARE(pressedSpy.takeFirst().at(0)
                 .value<InputModalityController::Action>(),
             InputModalityController::Action::Context);

    receiver.pressedKeys.clear();
    receiver.releasedKeys.clear();
    sendKey(Qt::Key_Menu);
    QVERIFY(receiver.pressedKeys.isEmpty());
    QVERIFY(receiver.releasedKeys.isEmpty());
    QCOMPARE(pressedSpy.takeFirst().at(0)
                 .value<InputModalityController::Action>(),
             InputModalityController::Action::Menu);
}

void InputModalityControllerTests::
    rawRemoteCorrelationRequiresExactFreshDeviceMatch()
{
    QVERIFY(InputModalityService::rawKeyEventMatchesRemote(
        QStringLiteral("raw:remote"), 0x26, true, 0x26, true, 0));
    QVERIFY(InputModalityService::rawKeyEventMatchesRemote(
        QStringLiteral("raw:remote"), 0x26, true, 0x26, true, 120));
    QVERIFY(!InputModalityService::rawKeyEventMatchesRemote(
        {}, 0x26, true, 0x26, true, 0));
    QVERIFY(!InputModalityService::rawKeyEventMatchesRemote(
        QStringLiteral("raw:remote"), 0x26, true, 0x28, true, 0));
    QVERIFY(!InputModalityService::rawKeyEventMatchesRemote(
        QStringLiteral("raw:remote"), 0x26, true, 0x26, false, 0));
    QVERIFY(!InputModalityService::rawKeyEventMatchesRemote(
        QStringLiteral("raw:remote"), 0x26, true, 0x26, true, -1));
    QVERIFY(!InputModalityService::rawKeyEventMatchesRemote(
        QStringLiteral("raw:remote"), 0x26, true, 0x26, true, 121));
    QVERIFY(!InputModalityService::rawKeyEventMatchesRemote(
        QStringLiteral("raw:remote"), 0x26, true, 0, true, 0));
}

void InputModalityControllerTests::
    remoteActionThenKeyboardArrowSwitchesImmediately()
{
    InputModalityController controller;
    KeyEventReceiver receiver;
    const QVariantMap remoteDevice{
        {QStringLiteral("id"), QStringLiteral("test:associated-remote")},
        {QStringLiteral("name"), QStringLiteral("Associated TV Remote")},
        {QStringLiteral("family"), QStringLiteral("remote")},
        {QStringLiteral("supportTier"), QStringLiteral("experimental")},
        {QStringLiteral("backend"), QStringLiteral("raw-input")},
        {QStringLiteral("connected"), true},
    };
    QSignalSpy actionSpy(&controller,
                         &InputModalityController::actionPressed);
    controller.dispatchActionForTest(
        InputModalityController::Action::NavigateUp,
        false,
        remoteDevice);
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Remote);
    QCOMPARE(actionSpy.count(), 1);

    QKeyEvent keyboardArrow(QEvent::KeyPress, Qt::Key_Down,
                            Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &keyboardArrow);
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Keyboard);
    QCOMPARE(actionSpy.count(), 1);
    QCOMPARE(receiver.pressedKeys, QVector<int>{Qt::Key_Down});
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
    controllerNeutralGateRejectsHeldInputUntilReleased()
{
    ControllerNavigationState state;
    ControllerNavigationState::Snapshot snapshot{
        .connected = true,
        .confirm = true,
    };
    QVERIFY(state.updateActions(snapshot, 0).isEmpty());
    QVERIFY(state.updateActions(snapshot, 1'000).isEmpty());

    snapshot.confirm = false;
    QVERIFY(state.updateActions(snapshot, 1'001).isEmpty());
    snapshot.confirm = true;
    const auto events = state.updateActions(snapshot, 1'002);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().action, ControllerInputAction::Activate);
    QVERIFY(events.first().pressed);
    QVERIFY(!events.first().repeated);
}

void InputModalityControllerTests::
    controllerSemanticActionsIncludeReleaseAndScroll()
{
    ControllerNavigationState state;
    ControllerNavigationState::Snapshot snapshot{.connected = true};
    QVERIFY(state.updateActions(snapshot, 0).isEmpty());

    snapshot.context = true;
    auto events = state.updateActions(snapshot, 1);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().action, ControllerInputAction::Context);
    QVERIFY(events.first().pressed);

    snapshot.context = false;
    events = state.updateActions(snapshot, 2);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().action, ControllerInputAction::Context);
    QVERIFY(!events.first().pressed);

    snapshot.rightThumbY = ControllerNavigationState::RightStickThreshold;
    events = state.updateActions(snapshot, 3);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().action, ControllerInputAction::ScrollUp);
    QVERIFY(events.first().pressed);
    QVERIFY(state.updateActions(snapshot, 352).isEmpty());
    events = state.updateActions(snapshot, 353);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().action, ControllerInputAction::ScrollUp);
    QVERIFY(events.first().repeated);

    snapshot.connected = false;
    events = state.updateActions(snapshot, 354);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().action, ControllerInputAction::ScrollUp);
    QVERIFY(!events.first().pressed);
}

void InputModalityControllerTests::
    controllerFamilyClassificationCoversEverySdlBoundary()
{
    using Source = ControllerNavigationSource;
    for (const int type : {2, 3}) {
        const auto classification = Source::classifyGamepad(type, 0, {});
        QCOMPARE(classification.family, QStringLiteral("xbox"));
        QCOMPARE(classification.supportTier,
                 QStringLiteral("hardware-pending"));
    }
    for (const int type : {4, 5, 6}) {
        const auto classification = Source::classifyGamepad(type, 0, {});
        QCOMPARE(classification.family, QStringLiteral("playstation"));
        QCOMPARE(classification.supportTier,
                 QStringLiteral("experimental"));
    }
    for (const int type : {7, 8, 9, 10}) {
        const auto classification = Source::classifyGamepad(type, 0, {});
        QCOMPARE(classification.family, QStringLiteral("nintendo"));
        QCOMPARE(classification.supportTier,
                 QStringLiteral("experimental"));
    }
    // Unknown, Standard, GameCube, and future Steam-style values do not get
    // promoted to a tested console family without vendor/name evidence.
    for (const int type : {0, 1, 11, 12}) {
        const auto classification = Source::classifyGamepad(type, 0, {});
        QCOMPARE(classification.family, QStringLiteral("generic"));
        QCOMPARE(classification.supportTier, QStringLiteral("generic"));
    }

    QCOMPARE(Source::classifyGamepad(0, 0x045e, {}).family,
             QStringLiteral("xbox"));
    QCOMPARE(Source::classifyGamepad(0, 0x054c, {}).family,
             QStringLiteral("playstation"));
    QCOMPARE(Source::classifyGamepad(0, 0x057e, {}).family,
             QStringLiteral("nintendo"));
    QCOMPARE(Source::classifyRawInputDevice(0x0c, 0x01, 0, 0, {}).family,
             QStringLiteral("generic"));
    QCOMPARE(Source::classifyRawInputDevice(
                 0x0c, 0x01, 0x20a0, 0x0006, {}).family,
             QStringLiteral("remote"));
    QCOMPARE(Source::classifyRawInputDevice(
                 0x01, 0x06, 0, 0,
                 QStringLiteral("RC6 infrared remote")).family,
             QStringLiteral("remote"));
    QCOMPARE(Source::classifyRawInputDevice(0x01, 0x06, 0, 0,
                                            QStringLiteral("Keyboard")).family,
             QStringLiteral("generic"));
    QCOMPARE(Source::rawInputPhysicalPathKey(
                 QStringLiteral("HID#VID_20A0&PID_0006&COL01#ABC")),
             Source::rawInputPhysicalPathKey(
                 QStringLiteral("hid#vid_20a0&pid_0006&col02#abc")));
    const auto genericKeyboard = Source::classifyRawInputDevice(
        0x01, 0x06, 0, 0, QStringLiteral("Keyboard"));
    QVERIFY(Source::shouldAssociateRawKeyboard(genericKeyboard, true));
    QVERIFY(!Source::shouldAssociateRawKeyboard(genericKeyboard, false));
}

void InputModalityControllerTests::
    controllerFaceButtonsFollowPlatformConvention()
{
    using Source = ControllerNavigationSource;
    const auto xbox = Source::classifyGamepad(2, 0, {});
    const auto playStation = Source::classifyGamepad(6, 0, {});
    const auto nintendo = Source::classifyGamepad(7, 0, {});

    auto buttons = Source::mapFaceButtons(xbox, true, false);
    QVERIFY(buttons.confirm);
    QVERIFY(!buttons.back);
    buttons = Source::mapFaceButtons(playStation, true, false);
    QVERIFY(buttons.confirm);
    QVERIFY(!buttons.back);
    buttons = Source::mapFaceButtons(nintendo, true, false);
    QVERIFY(!buttons.confirm);
    QVERIFY(buttons.back);
    buttons = Source::mapFaceButtons(nintendo, false, true);
    QVERIFY(buttons.confirm);
    QVERIFY(!buttons.back);
}

void InputModalityControllerTests::
    enumerationSuccessPolicyDistinguishesEmptyFromFailure()
{
    using Source = ControllerNavigationSource;

    // A successful empty SDL result still returns its 0-terminated buffer;
    // null is failure regardless of the output count and must not commit an
    // empty device snapshot.
    QVERIFY(Source::sdlEnumerationSucceeded(true));
    QVERIFY(!Source::sdlEnumerationSucceeded(false));

    QVERIFY(Source::rawRemoteEnumerationSucceeded({}));
    QVERIFY(!Source::rawRemoteEnumerationSucceeded(
        QStringLiteral("GetRawInputDeviceList failed")));
}

void InputModalityControllerTests::
    verticalAxisInversionSaturatesAtNativeMinimum()
{
    QCOMPARE(ControllerNavigationState::invertVerticalAxis(0), qint16(0));
    QCOMPARE(ControllerNavigationState::invertVerticalAxis(12'345),
             qint16(-12'345));
    QCOMPARE(ControllerNavigationState::invertVerticalAxis(-12'345),
             qint16(12'345));
    QCOMPARE(ControllerNavigationState::invertVerticalAxis(
                 std::numeric_limits<qint16>::min()),
             std::numeric_limits<qint16>::max());

    ControllerNavigationState::Snapshot snapshot;
    snapshot.leftThumbY = ControllerNavigationState::invertVerticalAxis(
        std::numeric_limits<qint16>::min());
    QCOMPARE(ControllerNavigationState::directionFor(snapshot),
             ControllerNavigationState::Direction::Up);
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

void InputModalityControllerTests::semanticActionUpdatesDeviceAndDiagnostics()
{
    InputModalityController controller;
    controller.notePointerInput();
    const QVariantMap xboxDevice{
        {QStringLiteral("id"), QStringLiteral("test:xbox")},
        {QStringLiteral("name"), QStringLiteral("Test Xbox Controller")},
        {QStringLiteral("family"), QStringLiteral("xbox")},
        {QStringLiteral("supportTier"), QStringLiteral("hardware-pending")},
        {QStringLiteral("backend"), QStringLiteral("test")},
        {QStringLiteral("connected"), true},
    };
    QSignalSpy actionSpy(&controller,
                         &InputModalityController::actionPressed);
    QSignalSpy activeSpy(&controller,
                         &InputModalityController::activeDeviceChanged);
    QSignalSpy lastActionSpy(&controller,
                             &InputModalityController::lastActionChanged);

    controller.dispatchActionForTest(
        InputModalityController::Action::Search, false, xboxDevice);
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Controller);
    QCOMPARE(controller.activeDeviceName(),
             QStringLiteral("Test Xbox Controller"));
    QCOMPARE(controller.activeDeviceFamily(), QStringLiteral("xbox"));
    QCOMPARE(controller.activeSupportTier(),
             QStringLiteral("hardware-pending"));
    QCOMPARE(controller.lastActionName(), QStringLiteral("search"));
    QCOMPARE(controller.promptForAction(
                 InputModalityController::Action::Activate),
             QStringLiteral("A"));
    QCOMPARE(actionSpy.count(), 1);
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(lastActionSpy.count(), 1);

    // Repeats are activity but do not spuriously change the active device.
    controller.dispatchActionForTest(
        InputModalityController::Action::Search, true, xboxDevice);
    QCOMPARE(actionSpy.count(), 2);
    QVERIFY(actionSpy.at(1).at(1).toBool());
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(lastActionSpy.count(), 2);

    const QStringList supportedBackends{
        QStringLiteral("sdl"),
        QStringLiteral("xinput"),
        QStringLiteral("none"),
    };
    QVERIFY(supportedBackends.contains(controller.controllerBackend()));
}

void InputModalityControllerTests::
    controllerInputTestOwnershipIsExclusiveAndOwnerBound()
{
    InputModalityController controller;
    QObject competingOwner;
    auto *owner = new QObject;
    QSignalSpy activeSpy(
        &controller,
        &InputModalityController::controllerInputTestActiveChanged);

    QVERIFY(!controller.controllerInputTestActive());
    QVERIFY(!controller.acquireControllerInputTest(nullptr));
    QVERIFY(controller.acquireControllerInputTest(owner));
    QVERIFY(controller.controllerInputTestActive());
    QCOMPARE(activeSpy.count(), 1);

    // Re-acquiring the same lease is idempotent; another component cannot
    // silently steal the diagnostic stream.
    QVERIFY(controller.acquireControllerInputTest(owner));
    QVERIFY(!controller.acquireControllerInputTest(&competingOwner));
    controller.releaseControllerInputTest(&competingOwner);
    QVERIFY(controller.controllerInputTestActive());
    QCOMPARE(activeSpy.count(), 1);

    delete owner;
    QVERIFY(!controller.controllerInputTestActive());
    QCOMPARE(activeSpy.count(), 2);
}

void InputModalityControllerTests::
    controllerInputTestSuppressesGlobalAndLegacyDispatch()
{
    InputModalityController controller;
    QObject owner;
    KeyEventReceiver receiver;
    const QVariantMap xboxDevice{
        {QStringLiteral("id"), QStringLiteral("test:capture-xbox")},
        {QStringLiteral("name"), QStringLiteral("Captured Xbox Controller")},
        {QStringLiteral("family"), QStringLiteral("xbox")},
        {QStringLiteral("supportTier"), QStringLiteral("verified")},
        {QStringLiteral("backend"), QStringLiteral("test")},
        {QStringLiteral("connected"), true},
    };
    QSignalSpy globalSpy(&controller,
                         &InputModalityController::actionPressed);
    QSignalSpy diagnosticSpy(
        &controller,
        &InputModalityController::controllerInputTestAction);

    QVERIFY(controller.acquireControllerInputTest(&owner));
    controller.dispatchActionForTest(
        InputModalityController::Action::Activate,
        false,
        xboxDevice,
        &receiver);

    QCOMPARE(diagnosticSpy.count(), 1);
    QCOMPARE(diagnosticSpy.at(0).at(0)
                 .value<InputModalityController::Action>(),
             InputModalityController::Action::Activate);
    QVERIFY(!diagnosticSpy.at(0).at(1).toBool());
    QCOMPARE(globalSpy.count(), 0);
    QVERIFY(receiver.pressedKeys.isEmpty());
    QVERIFY(receiver.releasedKeys.isEmpty());
    QCOMPARE(controller.lastActionName(), QStringLiteral("activate"));
    QCOMPARE(controller.activeDeviceName(),
             QStringLiteral("Captured Xbox Controller"));
    QCOMPARE(controller.modality(),
             InputModalityController::Modality::Controller);

    controller.dispatchActionForTest(
        InputModalityController::Action::Back,
        false,
        xboxDevice,
        &receiver);
    QCOMPARE(diagnosticSpy.count(), 2);
    QCOMPARE(diagnosticSpy.at(1).at(0)
                 .value<InputModalityController::Action>(),
             InputModalityController::Action::Back);
    QCOMPARE(globalSpy.count(), 0);
    QVERIFY(receiver.pressedKeys.isEmpty());
    QVERIFY(receiver.releasedKeys.isEmpty());

    controller.releaseControllerInputTest(&owner);
    controller.dispatchActionForTest(
        InputModalityController::Action::Activate,
        false,
        xboxDevice,
        &receiver);
    QCOMPARE(globalSpy.count(), 1);
    QCOMPARE(receiver.pressedKeys, QVector<int>{Qt::Key_Return});
    QCOMPARE(receiver.releasedKeys, QVector<int>{Qt::Key_Return});
}

void InputModalityControllerTests::
    controllerInputTestKeepsCapturedSequenceNeutralUntilRelease()
{
    InputModalityController controller;
    InputModalityService &service = InputModalityService::instance();
    QObject owner;
    constexpr auto Activate = InputModalityController::Action::Activate;
    const int activate = static_cast<int>(Activate);
    const QString deviceId = QStringLiteral("test:held-capture-xbox");
    QSignalSpy globalPressSpy(&controller,
                              &InputModalityController::actionPressed);
    QSignalSpy globalReleaseSpy(&controller,
                                &InputModalityController::actionReleased);
    QSignalSpy diagnosticSpy(
        &controller,
        &InputModalityController::controllerInputTestAction);

    QVERIFY(controller.acquireControllerInputTest(&owner));
    service.handleControllerActionPressed(activate, false, deviceId);
    QCOMPARE(diagnosticSpy.count(), 1);
    QCOMPARE(globalPressSpy.count(), 0);

    // Releasing the lease while the physical control is held must not turn
    // its repeat or eventual release into an unmatched global sequence.
    controller.releaseControllerInputTest(&owner);
    service.handleControllerActionPressed(activate, true, deviceId);
    service.handleControllerActionReleased(activate, deviceId);
    QCOMPARE(globalPressSpy.count(), 0);
    QCOMPARE(globalReleaseSpy.count(), 0);

    service.handleControllerActionPressed(activate, false, deviceId);
    service.handleControllerActionReleased(activate, deviceId);
    QCOMPARE(globalPressSpy.count(), 1);
    QCOMPARE(globalReleaseSpy.count(), 1);
}

void InputModalityControllerTests::
    controllerInputTestConsumesRemoteKeysWithoutReinjection()
{
    InputModalityController controller;
    QObject owner;
    KeyEventReceiver receiver;
    QSignalSpy globalPressSpy(&controller,
                              &InputModalityController::actionPressed);
    QSignalSpy globalReleaseSpy(&controller,
                                &InputModalityController::actionReleased);
    QSignalSpy diagnosticSpy(
        &controller,
        &InputModalityController::controllerInputTestAction);

    QVERIFY(controller.acquireControllerInputTest(&owner));
    QKeyEvent press(QEvent::KeyPress,
                    Qt::Key_MediaTogglePlayPause,
                    Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &press);
    controller.releaseControllerInputTest(&owner);
    QKeyEvent release(QEvent::KeyRelease,
                      Qt::Key_MediaTogglePlayPause,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &release);

    QCOMPARE(diagnosticSpy.count(), 1);
    QCOMPARE(diagnosticSpy.at(0).at(0)
                 .value<InputModalityController::Action>(),
             InputModalityController::Action::PlayPause);
    QCOMPARE(globalPressSpy.count(), 0);
    QCOMPARE(globalReleaseSpy.count(), 0);
    QVERIFY(receiver.pressedKeys.isEmpty());
    QVERIFY(receiver.releasedKeys.isEmpty());

    // The captured-key neutral gate clears on release; the next physical
    // sequence follows the ordinary semantic-only remote path.
    QKeyEvent nextPress(QEvent::KeyPress,
                        Qt::Key_MediaTogglePlayPause,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &nextPress);
    QKeyEvent nextRelease(QEvent::KeyRelease,
                          Qt::Key_MediaTogglePlayPause,
                          Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &nextRelease);
    QCOMPARE(globalPressSpy.count(), 1);
    QCOMPARE(globalReleaseSpy.count(), 1);
    QVERIFY(receiver.pressedKeys.isEmpty());
    QVERIFY(receiver.releasedKeys.isEmpty());
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
