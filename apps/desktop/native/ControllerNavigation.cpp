#include "ControllerNavigation.hpp"

#include <QGuiApplication>

#include <algorithm>
#include <cmath>

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

namespace {

constexpr int PollIntervalMs = 16;
constexpr qint64 DisconnectedProbeIntervalMs = 2'000;

int keyForDirection(ControllerNavigationState::Direction direction)
{
    switch (direction) {
    case ControllerNavigationState::Direction::Up:
        return Qt::Key_Up;
    case ControllerNavigationState::Direction::Down:
        return Qt::Key_Down;
    case ControllerNavigationState::Direction::Left:
        return Qt::Key_Left;
    case ControllerNavigationState::Direction::Right:
        return Qt::Key_Right;
    case ControllerNavigationState::Direction::None:
        return Qt::Key_unknown;
    }
    return Qt::Key_unknown;
}

} // namespace

QVector<int> ControllerNavigationState::update(
    const Snapshot &snapshot,
    qint64 nowMs)
{
    QVector<int> requestedKeys;
    if (!snapshot.connected) {
        reset();
        return requestedKeys;
    }

    const Direction direction = directionFor(snapshot);
    if (!m_initialized) {
        m_initialized = true;
        m_confirmPressed = snapshot.confirm;
        m_backPressed = snapshot.back;
        m_direction = direction;
        m_nextRepeatAtMs = nowMs + InitialRepeatDelayMs;
        return requestedKeys;
    }

    if (snapshot.confirm && !m_confirmPressed)
        requestedKeys.append(Qt::Key_Return);
    if (snapshot.back && !m_backPressed)
        requestedKeys.append(Qt::Key_Escape);

    if (direction != m_direction) {
        if (direction != Direction::None)
            requestedKeys.append(keyForDirection(direction));
        m_nextRepeatAtMs = nowMs + InitialRepeatDelayMs;
    } else if (direction != Direction::None
               && nowMs >= m_nextRepeatAtMs) {
        requestedKeys.append(keyForDirection(direction));
        m_nextRepeatAtMs = nowMs + RepeatIntervalMs;
    }

    m_confirmPressed = snapshot.confirm;
    m_backPressed = snapshot.back;
    m_direction = direction;
    return requestedKeys;
}

void ControllerNavigationState::reset()
{
    m_initialized = false;
    m_confirmPressed = false;
    m_backPressed = false;
    m_direction = Direction::None;
    m_nextRepeatAtMs = 0;
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

ControllerNavigationSource::ControllerNavigationSource(QObject *parent)
    : QObject(parent)
{
    m_elapsedTimer.start();
#ifdef Q_OS_WIN
    m_pollTimer.setTimerType(Qt::PreciseTimer);
    m_pollTimer.setInterval(PollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout,
            this, &ControllerNavigationSource::poll);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged,
            this,
            &ControllerNavigationSource::handleApplicationStateChanged);
    handleApplicationStateChanged(QGuiApplication::applicationState());
#endif
}

void ControllerNavigationSource::handleApplicationStateChanged(
    Qt::ApplicationState state)
{
#ifdef Q_OS_WIN
    resetSlots();
    if (state != Qt::ApplicationActive) {
        m_pollTimer.stop();
        return;
    }

    m_pollTimer.setInterval(PollIntervalMs);
    m_pollTimer.start();
#else
    Q_UNUSED(state)
#endif
}

void ControllerNavigationSource::poll()
{
#ifdef Q_OS_WIN
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        m_pollTimer.stop();
        resetSlots();
        return;
    }

    const qint64 nowMs = m_elapsedTimer.elapsed();
    bool anyControllerConnected = false;

    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        ControllerSlot &slot = m_slots[index];
        if (!slot.connected && nowMs < slot.nextProbeAtMs)
            continue;

        XINPUT_STATE nativeState{};
        if (XInputGetState(static_cast<DWORD>(index), &nativeState)
            != ERROR_SUCCESS) {
            slot.navigation.reset();
            slot.connected = false;
            slot.nextProbeAtMs = nowMs + DisconnectedProbeIntervalMs;
            continue;
        }

        slot.connected = true;
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
            .leftThumbX = nativeState.Gamepad.sThumbLX,
            .leftThumbY = nativeState.Gamepad.sThumbLY,
        };
        const QVector<int> requestedKeys =
            slot.navigation.update(snapshot, nowMs);
        for (const int key : requestedKeys)
            emit navigationRequested(key);
    }

    int nextInterval = PollIntervalMs;
    if (!anyControllerConnected) {
        qint64 earliestProbeAtMs =
            nowMs + DisconnectedProbeIntervalMs;
        for (const ControllerSlot &slot : m_slots) {
            earliestProbeAtMs = std::min(
                earliestProbeAtMs, slot.nextProbeAtMs);
        }
        nextInterval = static_cast<int>(std::clamp<qint64>(
            earliestProbeAtMs - nowMs,
            1,
            DisconnectedProbeIntervalMs));
    }
    if (m_pollTimer.interval() != nextInterval)
        m_pollTimer.setInterval(nextInterval);
#endif
}

void ControllerNavigationSource::resetSlots()
{
    for (ControllerSlot &slot : m_slots) {
        slot.navigation.reset();
        slot.connected = false;
        slot.nextProbeAtMs = 0;
    }
}
