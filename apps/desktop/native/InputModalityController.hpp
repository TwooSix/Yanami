#pragma once

#include <QtGlobal>
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#endif
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

class QEvent;
class QKeyEvent;
class ControllerNavigationSource;
class InputModalityControllerTests;
class InputModalityService;

class InputModalityController final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(InputModality)
    QML_SINGLETON
    Q_PROPERTY(Modality modality READ modality NOTIFY modalityChanged FINAL)
    Q_PROPERTY(bool focusNavigationActive READ focusNavigationActive
                   NOTIFY modalityChanged FINAL)
    Q_PROPERTY(bool controllerConnected READ controllerConnected
                   NOTIFY devicesChanged FINAL)
    Q_PROPERTY(QString activeDeviceName READ activeDeviceName
                   NOTIFY activeDeviceChanged FINAL)
    Q_PROPERTY(QString activeDeviceFamily READ activeDeviceFamily
                   NOTIFY activeDeviceChanged FINAL)
    Q_PROPERTY(QString activeSupportTier READ activeSupportTier
                   NOTIFY activeDeviceChanged FINAL)
    Q_PROPERTY(QString controllerBackend READ controllerBackend
                   NOTIFY controllerBackendChanged FINAL)
    Q_PROPERTY(QVariantList connectedDevices READ connectedDevices
                   NOTIFY devicesChanged FINAL)
    Q_PROPERTY(QString lastActionName READ lastActionName
                   NOTIFY lastActionChanged FINAL)
    Q_PROPERTY(bool controllerInputTestActive READ controllerInputTestActive
                   NOTIFY controllerInputTestActiveChanged FINAL)

public:
    enum class Modality {
        Pointer,
        Keyboard,
        Controller,
        Remote,
    };
    Q_ENUM(Modality)

    enum class Action {
        NavigateUp,
        NavigateDown,
        NavigateLeft,
        NavigateRight,
        Activate,
        Back,
        Context,
        Menu,
        Search,
        PagePrevious,
        PageNext,
        PageUp,
        PageDown,
        ScrollUp,
        ScrollDown,
        ScrollLeft,
        ScrollRight,
        PlayPause,
        SeekBackward,
        SeekForward,
        VolumeUp,
        VolumeDown,
        PreviousItem,
        NextItem,
    };
    Q_ENUM(Action)

    explicit InputModalityController(QObject *parent = nullptr);
    ~InputModalityController() override;

    Q_DISABLE_COPY_MOVE(InputModalityController)

    [[nodiscard]] Modality modality() const;
    [[nodiscard]] bool focusNavigationActive() const;
    [[nodiscard]] bool controllerConnected() const;
    [[nodiscard]] QString activeDeviceName() const;
    [[nodiscard]] QString activeDeviceFamily() const;
    [[nodiscard]] QString activeSupportTier() const;
    [[nodiscard]] QString controllerBackend() const;
    [[nodiscard]] QVariantList connectedDevices() const;
    [[nodiscard]] QString lastActionName() const;
    [[nodiscard]] bool controllerInputTestActive() const;

    Q_INVOKABLE void notePointerInput();
    Q_INVOKABLE void noteKeyboardNavigation();
    Q_INVOKABLE void noteControllerNavigation();
    Q_INVOKABLE QString promptForAction(Action action) const;
    Q_INVOKABLE bool acquireControllerInputTest(QObject *owner);
    Q_INVOKABLE void releaseControllerInputTest(QObject *owner);

signals:
    void modalityChanged();
    void actionPressed(InputModalityController::Action action, bool repeated);
    void actionReleased(InputModalityController::Action action);
    void controllerInputTestAction(InputModalityController::Action action,
                                   bool repeated);
    void devicesChanged();
    void activeDeviceChanged();
    void controllerBackendChanged();
    void lastActionChanged();
    void controllerInputTestActiveChanged();

private:
    friend class InputModalityControllerTests;

    void dispatchControllerNavigationKeyTo(QObject *target, int key);
    void dispatchActionForTest(Action action,
                               bool repeated,
                               const QVariantMap &device,
                               QObject *legacyTarget = nullptr);
};

class InputModalityService final : public QObject
#ifdef Q_OS_WIN
    , public QAbstractNativeEventFilter
#endif
{
    Q_OBJECT

public:
    static InputModalityService &instance();
    ~InputModalityService() override;

    Q_DISABLE_COPY_MOVE(InputModalityService)

    [[nodiscard]] InputModalityController::Modality modality() const;
    [[nodiscard]] bool focusNavigationActive() const;
    [[nodiscard]] bool controllerConnected() const;
    [[nodiscard]] QString activeDeviceName() const;
    [[nodiscard]] QString activeDeviceFamily() const;
    [[nodiscard]] QString activeSupportTier() const;
    [[nodiscard]] QString controllerBackend() const;
    [[nodiscard]] QVariantList connectedDevices() const;
    [[nodiscard]] QString lastActionName() const;
    [[nodiscard]] bool controllerInputTestActive() const;
    [[nodiscard]] QString promptForAction(
        InputModalityController::Action action) const;

    void notePointerInput();
    void noteKeyboardNavigation();
    void noteControllerNavigation();
    bool acquireControllerInputTest(QObject *owner);
    void releaseControllerInputTest(QObject *owner);
    void dispatchControllerNavigationKeyTo(QObject *target, int key);
    void dispatchActionForTest(InputModalityController::Action action,
                               bool repeated,
                               const QVariantMap &device,
                               QObject *legacyTarget = nullptr);
    [[nodiscard]] static bool rawKeyEventMatchesRemote(
        const QString &remoteDeviceId,
        quint32 rawVirtualKey,
        bool rawPressed,
        quint32 qtVirtualKey,
        bool qtPressed,
        qint64 ageMs);

signals:
    void modalityChanged();
    void actionPressed(InputModalityController::Action action, bool repeated);
    void actionReleased(InputModalityController::Action action);
    void controllerInputTestAction(InputModalityController::Action action,
                                   bool repeated);
    void devicesChanged();
    void activeDeviceChanged();
    void controllerBackendChanged();
    void lastActionChanged();
    void controllerInputTestActiveChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
#ifdef Q_OS_WIN
    bool nativeEventFilter(const QByteArray &eventType,
                           void *message,
                           qintptr *result) override;
#endif

private:
    friend class InputModalityControllerTests;

    explicit InputModalityService(QObject *parent = nullptr);

    static bool isModifierOnlyKey(int key);
    static QString actionName(InputModalityController::Action action);
    static bool isRemoteSpecificKey(int key);
    static bool remoteActionForKey(int key,
                                   InputModalityController::Action &action);
    static int legacyQtKeyForAction(InputModalityController::Action action);

    void dispatchControllerNavigationKey(int key);
    void handleControllerActionPressed(int action,
                                       bool repeated,
                                       const QString &deviceId);
    void handleControllerActionReleased(int action,
                                        const QString &deviceId);
    void handleSourceDevicesChanged();
    bool handleRemoteKeyEvent(QObject *target,
                              QKeyEvent *event,
                              bool &consume);
    [[nodiscard]] bool routeActionPressed(
        InputModalityController::Action action,
        bool repeated,
        const QVariantMap &device,
        InputModalityController::Modality modality);
    void emitActionReleased(InputModalityController::Action action);
    void markActiveDevice(const QVariantMap &device);
    QVariantMap remoteDescriptorForKeyEvent(const QKeyEvent &event);
    void clearRawKeyCorrelation();
    void dispatchNormalizedRemoteKey(QObject *target, int key);
    void dispatchLegacyKeyForAction(InputModalityController::Action action);
    void notePointerPosition(const QPoint &globalPosition);
    void setModality(InputModalityController::Modality modality);

    std::unique_ptr<ControllerNavigationSource> m_controllerNavigation;
    InputModalityController::Modality m_modality =
        InputModalityController::Modality::Pointer;
    QVariantMap m_activeDevice;
    QVariantMap m_virtualRemoteDevice;
    QString m_lastActionName;
    QPoint m_lastPointerPosition;
    bool m_pointerPositionKnown = false;
    bool m_dispatchingControllerKey = false;
    QElapsedTimer m_inputClock;
    QHash<int, InputModalityController::Action> m_remotePressedKeys;
    QHash<QString, QSet<int>> m_capturedControllerActions;
    QSet<int> m_capturedRemoteKeys;
    QPointer<QObject> m_controllerInputTestOwner;
    QMetaObject::Connection m_controllerInputTestOwnerDestroyed;
    quint64 m_controllerInputTestOwnerGeneration = 0;
#ifdef Q_OS_WIN
    bool m_rawInputRegistered = false;
    quintptr m_lastRawDeviceHandle = 0;
    quint32 m_lastRawVirtualKey = 0;
    bool m_lastRawPressed = false;
    qint64 m_lastRawEventAtMs = -1;
#endif
};
