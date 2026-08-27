#include "InputModalityController.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>
#include <QWindow>

#include <SDL3/SDL.h>

namespace {

QString sdlError()
{
    return QString::fromUtf8(SDL_GetError());
}

class KeyEventWindow final : public QWindow
{
public:
    QVector<int> pressedKeys;
    QVector<int> releasedKeys;

    void clearRecordedKeys()
    {
        pressedKeys.clear();
        releasedKeys.clear();
    }

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
        return QWindow::event(event);
    }
};

class VirtualController final
{
public:
    VirtualController() = default;

    ~VirtualController()
    {
        (void) detach();
    }

    Q_DISABLE_COPY_MOVE(VirtualController)

    bool attach(const QString &name, quint16 vendorId, quint16 productId)
    {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            m_error = QStringLiteral("SDL gamepad initialization failed: %1")
                          .arg(sdlError());
            return false;
        }

        m_encodedName = name.toUtf8();
        SDL_VirtualJoystickDesc descriptor{};
        SDL_INIT_INTERFACE(&descriptor);
        descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        descriptor.vendor_id = vendorId;
        descriptor.product_id = productId;
        descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
        descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        descriptor.name = m_encodedName.constData();

        m_id = SDL_AttachVirtualJoystick(&descriptor);
        if (m_id == 0) {
            m_error = QStringLiteral("SDL virtual joystick attach failed: %1")
                          .arg(sdlError());
            return false;
        }

        m_joystick = SDL_OpenJoystick(m_id);
        if (!m_joystick) {
            m_error = QStringLiteral("SDL virtual joystick open failed: %1")
                          .arg(sdlError());
            return false;
        }

        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
            if (!SDL_SetJoystickVirtualButton(m_joystick, button, false)) {
                m_error = QStringLiteral(
                              "SDL virtual button neutralization failed: %1")
                              .arg(sdlError());
                return false;
            }
        }
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
            if (!SDL_SetJoystickVirtualAxis(m_joystick, axis, 0)) {
                m_error = QStringLiteral(
                              "SDL virtual axis neutralization failed: %1")
                              .arg(sdlError());
                return false;
            }
        }
        SDL_UpdateGamepads();
        return true;
    }

    bool detach()
    {
        if (m_joystick) {
            for (int button = 0;
                 button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
                (void) SDL_SetJoystickVirtualButton(
                    m_joystick, button, false);
            }
            for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
                (void) SDL_SetJoystickVirtualAxis(m_joystick, axis, 0);
            SDL_UpdateGamepads();
            SDL_CloseJoystick(m_joystick);
            m_joystick = nullptr;
        }
        if (m_id != 0) {
            if (!SDL_DetachVirtualJoystick(m_id)) {
                m_error = QStringLiteral(
                              "SDL virtual joystick detach failed: %1")
                              .arg(sdlError());
                return false;
            }
            m_id = 0;
            SDL_UpdateGamepads();
        }
        return true;
    }

    bool setButton(SDL_GamepadButton button, bool pressed)
    {
        if (m_joystick
            && SDL_SetJoystickVirtualButton(
                m_joystick, static_cast<int>(button), pressed)) {
            return true;
        }
        m_error = QStringLiteral(
                      "SDL virtual button %1 update failed: %2")
                      .arg(static_cast<int>(button))
                      .arg(sdlError());
        return false;
    }

    [[nodiscard]] QString error() const
    {
        return m_error;
    }

private:
    SDL_JoystickID m_id = 0;
    SDL_Joystick *m_joystick = nullptr;
    QByteArray m_encodedName;
    QString m_error;
};

QVariantMap descriptorNamed(const QVariantList &devices,
                            const QString &expectedName)
{
    for (const QVariant &value : devices) {
        const QVariantMap descriptor = value.toMap();
        if (descriptor.value(QStringLiteral("name")).toString()
            == expectedName) {
            return descriptor;
        }
    }
    return {};
}

QString hexadecimalId(int value)
{
    return QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0'));
}

} // namespace

class SdlVirtualControllerIntegrationTests final : public QObject
{
    Q_OBJECT

private slots:
    void virtualFamilyButtonsReachPublicActionSignals_data();
    void virtualFamilyButtonsReachPublicActionSignals();
    void virtualDevicesHotSwitchWithoutRestart();
};

void SdlVirtualControllerIntegrationTests::
    virtualFamilyButtonsReachPublicActionSignals_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<int>("vendorId");
    QTest::addColumn<int>("productId");
    QTest::addColumn<QString>("family");
    QTest::addColumn<QString>("supportTier");
    QTest::addColumn<int>("activateButton");
    QTest::addColumn<int>("backButton");
    QTest::addColumn<int>("searchButton");
    QTest::addColumn<QString>("activatePrompt");
    QTest::addColumn<QString>("backPrompt");
    QTest::addColumn<QString>("searchPrompt");

    QTest::newRow("xbox")
        << QStringLiteral("Yanami SDL Virtual Xbox Integration Test")
        << 0x045e << 0x028e
        << QStringLiteral("xbox")
        << QStringLiteral("hardware-pending")
        << static_cast<int>(SDL_GAMEPAD_BUTTON_SOUTH)
        << static_cast<int>(SDL_GAMEPAD_BUTTON_EAST)
        << static_cast<int>(SDL_GAMEPAD_BUTTON_NORTH)
        << QStringLiteral("A") << QStringLiteral("B")
        << QStringLiteral("Y");
    QTest::newRow("playstation")
        << QStringLiteral("Yanami SDL Virtual PlayStation Integration Test")
        << 0x054c << 0x0ce6
        << QStringLiteral("playstation")
        << QStringLiteral("experimental")
        << static_cast<int>(SDL_GAMEPAD_BUTTON_SOUTH)
        << static_cast<int>(SDL_GAMEPAD_BUTTON_EAST)
        << static_cast<int>(SDL_GAMEPAD_BUTTON_NORTH)
        << QStringLiteral("Cross") << QStringLiteral("Circle")
        << QStringLiteral("Triangle");
    QTest::newRow("nintendo-switch")
        << QStringLiteral("Yanami SDL Virtual Nintendo Switch Integration Test")
        << 0x057e << 0x2009
        << QStringLiteral("nintendo")
        << QStringLiteral("experimental")
        << static_cast<int>(SDL_GAMEPAD_BUTTON_EAST)
        << static_cast<int>(SDL_GAMEPAD_BUTTON_SOUTH)
        << static_cast<int>(SDL_GAMEPAD_BUTTON_NORTH)
        << QStringLiteral("A") << QStringLiteral("B")
        << QStringLiteral("X");
}

void SdlVirtualControllerIntegrationTests::
    virtualFamilyButtonsReachPublicActionSignals()
{
    QFETCH(QString, name);
    QFETCH(int, vendorId);
    QFETCH(int, productId);
    QFETCH(QString, family);
    QFETCH(QString, supportTier);
    QFETCH(int, activateButton);
    QFETCH(int, backButton);
    QFETCH(int, searchButton);
    QFETCH(QString, activatePrompt);
    QFETCH(QString, backPrompt);
    QFETCH(QString, searchPrompt);

    KeyEventWindow activeWindow;
    activeWindow.setTitle(QStringLiteral("Yanami SDL controller test host"));
    activeWindow.resize(160, 90);
    activeWindow.show();
    activeWindow.requestActivate();

    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::applicationState(),
                              Qt::ApplicationActive, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::focusWindow(),
                              &activeWindow, 2'000);

    VirtualController virtualController;
    QVERIFY2(virtualController.attach(
                 name, static_cast<quint16>(vendorId),
                 static_cast<quint16>(productId)),
             qPrintable(virtualController.error()));

    // This public facade owns the production ControllerNavigationSource. The
    // test never calls dispatchActionForTest or bypasses SDL polling.
    InputModalityController inputModality;
    InputModalityService::instance().initializeControllerNavigation();
    QCOMPARE(inputModality.controllerBackend(), QStringLiteral("sdl"));

    QVariantMap virtualDescriptor;
    QTRY_VERIFY_WITH_TIMEOUT(
        !(virtualDescriptor = descriptorNamed(
              inputModality.connectedDevices(), name)).isEmpty(),
        2'000);
    QCOMPARE(virtualDescriptor.value(QStringLiteral("backend")).toString(),
             QStringLiteral("sdl"));
    QCOMPARE(virtualDescriptor.value(QStringLiteral("family")).toString(),
             family);
    QCOMPARE(virtualDescriptor.value(
                 QStringLiteral("supportTier")).toString(),
             supportTier);
    QCOMPARE(virtualDescriptor.value(QStringLiteral("vendorId")).toString(),
             hexadecimalId(vendorId));
    QCOMPARE(virtualDescriptor.value(QStringLiteral("productId")).toString(),
             hexadecimalId(productId));
    QVERIFY(virtualDescriptor.value(QStringLiteral("connected")).toBool());
    QVERIFY(virtualDescriptor.value(QStringLiteral("id")).toString()
                .startsWith(QStringLiteral("sdl:")));

    // The production source rejects held input until one neutral snapshot has
    // been observed after connection.
    QTest::qWait(50);

    QSignalSpy pressedSpy(&inputModality,
                          &InputModalityController::actionPressed);
    QSignalSpy releasedSpy(&inputModality,
                           &InputModalityController::actionReleased);
    QVERIFY(pressedSpy.isValid());
    QVERIFY(releasedSpy.isValid());

    const auto exerciseButton =
        [&](int nativeButton,
            InputModalityController::Action expectedAction,
            const QString &expectedActionName,
            int expectedLegacyKey) {
            pressedSpy.clear();
            releasedSpy.clear();
            activeWindow.clearRecordedKeys();

            QVERIFY2(virtualController.setButton(
                         static_cast<SDL_GamepadButton>(nativeButton), true),
                     qPrintable(virtualController.error()));
            QTRY_COMPARE_WITH_TIMEOUT(pressedSpy.count(), 1, 2'000);
            const QList<QVariant> pressed = pressedSpy.takeFirst();
            QCOMPARE(pressed.at(0)
                         .value<InputModalityController::Action>(),
                     expectedAction);
            QCOMPARE(pressed.at(1).toBool(), false);
            QCOMPARE(inputModality.modality(),
                     InputModalityController::Modality::Controller);
            QCOMPARE(inputModality.activeDeviceName(), name);
            QCOMPARE(inputModality.activeDeviceFamily(), family);
            QCOMPARE(inputModality.activeSupportTier(), supportTier);
            QCOMPARE(inputModality.lastActionName(), expectedActionName);
            if (expectedLegacyKey == Qt::Key_unknown) {
                QVERIFY(activeWindow.pressedKeys.isEmpty());
                QVERIFY(activeWindow.releasedKeys.isEmpty());
            } else {
                QCOMPARE(activeWindow.pressedKeys,
                         QVector<int>{expectedLegacyKey});
                QCOMPARE(activeWindow.releasedKeys,
                         QVector<int>{expectedLegacyKey});
            }

            QVERIFY2(virtualController.setButton(
                         static_cast<SDL_GamepadButton>(nativeButton), false),
                     qPrintable(virtualController.error()));
            QTRY_COMPARE_WITH_TIMEOUT(releasedSpy.count(), 1, 2'000);
            QCOMPARE(releasedSpy.takeFirst().at(0)
                         .value<InputModalityController::Action>(),
                     expectedAction);
            QCOMPARE(inputModality.activeDeviceName(), name);
        };

    exerciseButton(activateButton,
                   InputModalityController::Action::Activate,
                   QStringLiteral("activate"), Qt::Key_Return);
    QCOMPARE(inputModality.promptForAction(
                 InputModalityController::Action::Activate),
             activatePrompt);
    exerciseButton(backButton,
                   InputModalityController::Action::Back,
                   QStringLiteral("back"), Qt::Key_Escape);
    QCOMPARE(inputModality.promptForAction(
                 InputModalityController::Action::Back),
             backPrompt);
    exerciseButton(searchButton,
                   InputModalityController::Action::Search,
                   QStringLiteral("search"), Qt::Key_unknown);
    QCOMPARE(inputModality.promptForAction(
                 InputModalityController::Action::Search),
             searchPrompt);
    exerciseButton(static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
                   InputModalityController::Action::NavigateRight,
                   QStringLiteral("navigateRight"), Qt::Key_Right);
    exerciseButton(static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_UP),
                   InputModalityController::Action::NavigateUp,
                   QStringLiteral("navigateUp"), Qt::Key_Up);

    QVERIFY2(virtualController.detach(),
             qPrintable(virtualController.error()));
    QTRY_VERIFY_WITH_TIMEOUT(
        descriptorNamed(inputModality.connectedDevices(), name).isEmpty(),
        2'000);
}

void SdlVirtualControllerIntegrationTests::
    virtualDevicesHotSwitchWithoutRestart()
{
    struct HotSwitchProfile {
        QString name;
        quint16 vendorId;
        quint16 productId;
        QString family;
        QString supportTier;
        SDL_GamepadButton activateButton;
        QString activatePrompt;
    };
    const QList<HotSwitchProfile> profiles{
        {QStringLiteral("Yanami SDL Virtual Xbox Integration Test"),
         0x045e, 0x028e, QStringLiteral("xbox"),
         QStringLiteral("hardware-pending"),
         SDL_GAMEPAD_BUTTON_SOUTH, QStringLiteral("A")},
        {QStringLiteral("Yanami SDL Virtual PlayStation Integration Test"),
         0x054c, 0x0ce6, QStringLiteral("playstation"),
         QStringLiteral("experimental"),
         SDL_GAMEPAD_BUTTON_SOUTH, QStringLiteral("Cross")},
        {QStringLiteral("Yanami SDL Virtual Nintendo Switch Integration Test"),
         0x057e, 0x2009, QStringLiteral("nintendo"),
         QStringLiteral("experimental"),
         SDL_GAMEPAD_BUTTON_EAST, QStringLiteral("A")},
    };

    InputModalityController inputModality;
    InputModalityService::instance().initializeControllerNavigation();
    QSignalSpy pressedSpy(&inputModality,
                          &InputModalityController::actionPressed);
    QSignalSpy releasedSpy(&inputModality,
                           &InputModalityController::actionReleased);
    QVERIFY(pressedSpy.isValid());
    QVERIFY(releasedSpy.isValid());

    for (const HotSwitchProfile &profile : profiles) {
        VirtualController virtualController;
        QVERIFY2(virtualController.attach(profile.name,
                                           profile.vendorId,
                                           profile.productId),
                 qPrintable(virtualController.error()));
        QVariantMap descriptor;
        QTRY_VERIFY_WITH_TIMEOUT(
            !(descriptor = descriptorNamed(inputModality.connectedDevices(),
                                             profile.name)).isEmpty(),
            2'000);
        QCOMPARE(descriptor.value(QStringLiteral("family")).toString(),
                 profile.family);
        QCOMPARE(descriptor.value(
                     QStringLiteral("supportTier")).toString(),
                 profile.supportTier);

        // Each freshly attached controller must provide a neutral poll before
        // its first press is accepted by the production state machine.
        QTest::qWait(50);
        pressedSpy.clear();
        releasedSpy.clear();
        QVERIFY2(virtualController.setButton(profile.activateButton, true),
                 qPrintable(virtualController.error()));
        QTRY_COMPARE_WITH_TIMEOUT(pressedSpy.count(), 1, 2'000);
        QCOMPARE(pressedSpy.takeFirst().at(0)
                     .value<InputModalityController::Action>(),
                 InputModalityController::Action::Activate);
        QCOMPARE(inputModality.activeDeviceName(), profile.name);
        QCOMPARE(inputModality.activeDeviceFamily(), profile.family);
        QCOMPARE(inputModality.activeSupportTier(), profile.supportTier);
        QCOMPARE(inputModality.promptForAction(
                     InputModalityController::Action::Activate),
                 profile.activatePrompt);

        QVERIFY2(virtualController.setButton(profile.activateButton, false),
                 qPrintable(virtualController.error()));
        QTRY_COMPARE_WITH_TIMEOUT(releasedSpy.count(), 1, 2'000);
        QCOMPARE(releasedSpy.takeFirst().at(0)
                     .value<InputModalityController::Action>(),
                 InputModalityController::Action::Activate);
        QVERIFY2(virtualController.detach(),
                 qPrintable(virtualController.error()));
        QTRY_VERIFY_WITH_TIMEOUT(
            descriptorNamed(inputModality.connectedDevices(),
                            profile.name).isEmpty(),
            2'000);
    }
}

int main(int argc, char **argv)
{
    qputenv("YANAMI_CONTROLLER_BACKEND", "sdl");
    int exitCode = 0;
    {
        QGuiApplication application(argc, argv);
        SdlVirtualControllerIntegrationTests tests;
        exitCode = QTest::qExec(&tests, argc, argv);
    }
    SDL_Quit();
    return exitCode;
}

#include "SdlVirtualControllerIntegrationTests.moc"
