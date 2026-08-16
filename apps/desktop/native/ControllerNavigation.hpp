#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <QtTypes>

#include <array>

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
        qint16 leftThumbX = 0;
        qint16 leftThumbY = 0;
    };

    static constexpr int LeftStickThreshold = 16'000;
    static constexpr qint64 InitialRepeatDelayMs = 350;
    static constexpr qint64 RepeatIntervalMs = 90;

    [[nodiscard]] QVector<int> update(const Snapshot &snapshot,
                                      qint64 nowMs);
    void reset();

    [[nodiscard]] static Direction directionFor(
        const Snapshot &snapshot);

private:
    bool m_initialized = false;
    bool m_confirmPressed = false;
    bool m_backPressed = false;
    Direction m_direction = Direction::None;
    qint64 m_nextRepeatAtMs = 0;
};

class ControllerNavigationSource final : public QObject
{
    Q_OBJECT

public:
    explicit ControllerNavigationSource(QObject *parent = nullptr);

    Q_DISABLE_COPY_MOVE(ControllerNavigationSource)

signals:
    void navigationRequested(int qtKey);

private:
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void poll();
    void resetSlots();

    struct ControllerSlot {
        ControllerNavigationState navigation;
        bool connected = false;
        qint64 nextProbeAtMs = 0;
    };

    std::array<ControllerSlot, 4> m_slots;
    QElapsedTimer m_elapsedTimer;
    QTimer m_pollTimer;
};
