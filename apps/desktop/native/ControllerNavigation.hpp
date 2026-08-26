#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtTypes>

#include <array>
#include <memory>

// Keep this transport enum independent from the QML facade. The native input
// backends can therefore be tested without constructing the QML singleton.
enum class ControllerInputAction : int {
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
    Count,
};

class ControllerNavigationState final
{
public:
    enum class Direction {
        None,
        Up,
        Down,
        Left,
        Right,
    };

    struct Snapshot {
        bool connected = false;
        bool dpadUp = false;
        bool dpadDown = false;
        bool dpadLeft = false;
        bool dpadRight = false;
        bool confirm = false;
        bool back = false;
        bool context = false;
        bool menu = false;
        bool search = false;
        bool pagePrevious = false;
        bool pageNext = false;
        bool playPause = false;
        bool seekBackward = false;
        bool seekForward = false;
        bool previousItem = false;
        bool nextItem = false;
        qint16 leftThumbX = 0;
        qint16 leftThumbY = 0;
        qint16 rightThumbX = 0;
        qint16 rightThumbY = 0;
    };

    struct ActionEvent {
        ControllerInputAction action = ControllerInputAction::Activate;
        bool pressed = false;
        bool repeated = false;
    };

    static constexpr int LeftStickThreshold = 16'000;
    static constexpr int RightStickThreshold = 20'000;
    static constexpr qint64 InitialRepeatDelayMs = 350;
    static constexpr qint64 RepeatIntervalMs = 90;

    // Compatibility adapter used by the pre-existing tests and legacy key
    // dispatch. New code consumes updateActions() instead.
    [[nodiscard]] QVector<int> update(const Snapshot &snapshot,
                                      qint64 nowMs);
    [[nodiscard]] QVector<ActionEvent> updateActions(
        const Snapshot &snapshot,
        qint64 nowMs);
    void reset();

    [[nodiscard]] static Direction directionFor(
        const Snapshot &snapshot);
    [[nodiscard]] static Direction scrollDirectionFor(
        const Snapshot &snapshot);
    [[nodiscard]] static bool isNeutral(const Snapshot &snapshot);
    [[nodiscard]] static qint16 invertVerticalAxis(qint16 value);
    [[nodiscard]] static int qtKeyForAction(ControllerInputAction action);

private:
    static constexpr int ButtonActionCount = 12;

    void appendDirectionTransition(QVector<ActionEvent> &events,
                                   Direction previous,
                                   Direction current,
                                   qint64 nowMs,
                                   bool scroll);
    [[nodiscard]] static ControllerInputAction directionAction(
        Direction direction,
        bool scroll);

    bool m_initialized = false;
    bool m_awaitingNeutral = true;
    std::array<bool, ButtonActionCount> m_buttonPressed{};
    Direction m_direction = Direction::None;
    Direction m_scrollDirection = Direction::None;
    qint64 m_nextRepeatAtMs = 0;
    qint64 m_nextScrollRepeatAtMs = 0;
};

class ControllerNavigationSource final : public QObject
{
    Q_OBJECT

public:
    struct DeviceClassification {
        QString family;
        QString supportTier;
    };

    struct FaceButtonState {
        bool confirm = false;
        bool back = false;
    };

    explicit ControllerNavigationSource(QObject *parent = nullptr);
    ~ControllerNavigationSource() override;

    Q_DISABLE_COPY_MOVE(ControllerNavigationSource)

    [[nodiscard]] QVariantList connectedDevices() const;
    [[nodiscard]] QString backend() const;
    [[nodiscard]] QVariantMap deviceDescriptor(
        const QString &deviceId) const;
    [[nodiscard]] QString remoteDeviceIdForNativeHandle(
        quintptr nativeHandle) const;

    [[nodiscard]] static DeviceClassification classifyGamepad(
        int nativeType,
        quint16 vendorId,
        const QString &name);
    [[nodiscard]] static DeviceClassification classifyRawInputDevice(
        quint16 usagePage,
        quint16 usage,
        quint16 vendorId,
        quint16 productId,
        const QString &name);
    [[nodiscard]] static QString rawInputPhysicalPathKey(
        const QString &devicePath);
    [[nodiscard]] static bool shouldAssociateRawKeyboard(
        const DeviceClassification &ownClassification,
        bool physicalDeviceHasConfirmedRemoteCollection);
    [[nodiscard]] static bool sdlEnumerationSucceeded(
        bool resultBufferReturned);
    [[nodiscard]] static bool rawRemoteEnumerationSucceeded(
        const QString &error);
    [[nodiscard]] static bool nintendoConfirmUsesEastButton(
        const DeviceClassification &classification);
    [[nodiscard]] static FaceButtonState mapFaceButtons(
        const DeviceClassification &classification,
        bool southPressed,
        bool eastPressed);

signals:
    void actionPressed(int action, bool repeated, const QString &deviceId);
    void actionReleased(int action, const QString &deviceId);
    void devicesChanged();
    void backendChanged();

private:
    struct Impl;

    void handleApplicationStateChanged(Qt::ApplicationState state);
    void poll();
    void resetInputStates(bool emitReleases);
    void emitEvents(const QVector<ControllerNavigationState::ActionEvent> &events,
                    const QString &deviceId);

    std::unique_ptr<Impl> m_impl;
    QElapsedTimer m_elapsedTimer;
    QTimer m_pollTimer;
};
