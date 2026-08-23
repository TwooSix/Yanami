#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace YanamiPlayback {

enum class PlaybackStallState {
    Inactive,
    Monitoring,
    Suspended,
    Stalled,
    TimedOut,
};

enum class PlaybackStallEvent {
    None,
    EnteredStall,
    StallCleared,
    TimedOut,
};

struct PlaybackStallTiming
{
    std::int64_t stallAfterMs = 2'500;
    std::int64_t timeoutAfterMs = 20'000;
    std::int64_t maximumPollGapMs = 2'000;
    double minimumAdvanceSeconds = 0.05;
};

// A clock-free state machine for detecting playback that has stopped making
// real progress. The owner supplies monotonic milliseconds and performs the
// polling so this policy can be tested without Qt Quick or libmpv.
class PlaybackStallWatchdog
{
public:
    explicit PlaybackStallWatchdog(PlaybackStallTiming timing = {})
        : m_timing{
              std::max<std::int64_t>(1, timing.stallAfterMs),
              std::max(timing.timeoutAfterMs,
                       std::max<std::int64_t>(1, timing.stallAfterMs)),
              std::max<std::int64_t>(1, timing.maximumPollGapMs),
              std::max(0.000'001, timing.minimumAdvanceSeconds),
          }
    {
    }

    void reset()
    {
        m_state = PlaybackStallState::Inactive;
        m_seeking = false;
        m_seekRestartArmed = false;
        m_reanchorNextPosition = false;
        m_haveAnchor = false;
        m_anchorSeconds = 0.0;
        m_noProgressSinceMs = 0;
        m_lastClockMs = 0;
        m_haveClock = false;
    }

    void arm(std::int64_t nowMs, double positionSeconds)
    {
        m_state = PlaybackStallState::Monitoring;
        m_seeking = false;
        m_seekRestartArmed = false;
        m_reanchorNextPosition = false;
        setAnchor(positionSeconds);
        m_noProgressSinceMs = nowMs;
        m_lastClockMs = nowMs;
        m_haveClock = true;
    }

    void markStalled(std::int64_t nowMs, double positionSeconds)
    {
        if (m_state == PlaybackStallState::Inactive
            || m_state == PlaybackStallState::Suspended
            || m_state == PlaybackStallState::TimedOut) {
            return;
        }
        normalizeClock(nowMs);
        setAnchor(positionSeconds);
        m_state = PlaybackStallState::Stalled;
    }

    PlaybackStallEvent setPaused(
        bool paused, std::int64_t nowMs, double positionSeconds)
    {
        if (m_state == PlaybackStallState::Inactive)
            return PlaybackStallEvent::None;

        normalizeClock(nowMs);
        if (paused) {
            const bool wasStalled = isStalled();
            m_state = PlaybackStallState::Suspended;
            m_seeking = false;
            m_seekRestartArmed = false;
            m_reanchorNextPosition = false;
            setAnchor(positionSeconds);
            return wasStalled ? PlaybackStallEvent::StallCleared
                              : PlaybackStallEvent::None;
        }

        if (m_state != PlaybackStallState::Suspended)
            return PlaybackStallEvent::None;
        m_state = PlaybackStallState::Monitoring;
        m_seeking = false;
        m_seekRestartArmed = false;
        m_reanchorNextPosition = false;
        setAnchor(positionSeconds);
        m_noProgressSinceMs = nowMs;
        return PlaybackStallEvent::None;
    }

    void beginSeek(std::int64_t nowMs, double positionSeconds)
    {
        if (m_state == PlaybackStallState::Inactive
            || m_state == PlaybackStallState::Suspended) {
            return;
        }
        normalizeClock(nowMs);
        m_seeking = true;
        m_seekRestartArmed = false;
        m_reanchorNextPosition = false;
        setAnchor(positionSeconds);
        m_noProgressSinceMs = nowMs;
    }

    void observeSeekStarted(std::int64_t nowMs, double positionSeconds)
    {
        if (m_state == PlaybackStallState::Inactive
            || m_state == PlaybackStallState::Suspended) {
            return;
        }
        if (!m_seeking) {
            normalizeClock(nowMs);
            m_seeking = true;
            m_reanchorNextPosition = false;
            setAnchor(positionSeconds);
            m_noProgressSinceMs = nowMs;
        }
        // An MPV_EVENT_SEEK is the only event that can arm the matching
        // PLAYBACK_RESTART. This fences a late initial-load restart from a
        // user seek issued by FILE_LOADED and keeps repeated seek events from
        // extending the original request deadline.
        m_seekRestartArmed = true;
    }

    void observePlaybackRestart(
        std::int64_t nowMs, double positionSeconds)
    {
        if (m_state == PlaybackStallState::Inactive
            || m_state == PlaybackStallState::Suspended || !m_seeking
            || !m_seekRestartArmed) {
            return;
        }
        normalizeClock(nowMs);
        m_seeking = false;
        m_seekRestartArmed = false;
        // A seek jump or PLAYBACK_RESTART only establishes the new baseline.
        // The first subsequent forward time-pos update proves recovery.
        setAnchor(positionSeconds);
        m_reanchorNextPosition = true;
    }

    PlaybackStallEvent observePosition(
        std::int64_t nowMs, double positionSeconds)
    {
        if (m_state == PlaybackStallState::Inactive)
            return PlaybackStallEvent::None;

        normalizeClock(nowMs);
        if (!std::isfinite(positionSeconds))
            return pollNormalized(nowMs);

        if (m_state == PlaybackStallState::Suspended) {
            setAnchor(positionSeconds);
            return PlaybackStallEvent::None;
        }
        if (m_seeking) {
            setAnchor(positionSeconds);
            return pollNormalized(nowMs);
        }
        if (m_reanchorNextPosition) {
            m_reanchorNextPosition = false;
            setAnchor(positionSeconds);
            return pollNormalized(nowMs);
        }
        if (!m_haveAnchor) {
            setAnchor(positionSeconds);
            m_noProgressSinceMs = nowMs;
            return PlaybackStallEvent::None;
        }

        if (positionSeconds - m_anchorSeconds
            >= m_timing.minimumAdvanceSeconds) {
            const bool wasStalled = isStalled();
            m_anchorSeconds = positionSeconds;
            m_noProgressSinceMs = nowMs;
            m_state = PlaybackStallState::Monitoring;
            return wasStalled ? PlaybackStallEvent::StallCleared
                              : PlaybackStallEvent::None;
        }
        return pollNormalized(nowMs);
    }

    PlaybackStallEvent poll(std::int64_t nowMs)
    {
        if (m_state == PlaybackStallState::Inactive
            || m_state == PlaybackStallState::Suspended
            || m_state == PlaybackStallState::TimedOut) {
            if (m_state != PlaybackStallState::Inactive)
                normalizeClock(nowMs);
            return PlaybackStallEvent::None;
        }

        if (normalizeClock(nowMs))
            return PlaybackStallEvent::None;
        return pollNormalized(nowMs);
    }

    PlaybackStallState state() const { return m_state; }
    bool isStalled() const
    {
        return m_state == PlaybackStallState::Stalled
            || m_state == PlaybackStallState::TimedOut;
    }
    bool seeking() const { return m_seeking; }
    std::int64_t noProgressForMs(std::int64_t nowMs) const
    {
        if (m_state == PlaybackStallState::Inactive
            || m_state == PlaybackStallState::Suspended) {
            return 0;
        }
        return std::max<std::int64_t>(0, nowMs - m_noProgressSinceMs);
    }

private:
    void setAnchor(double positionSeconds)
    {
        if (!std::isfinite(positionSeconds)) {
            m_haveAnchor = false;
            return;
        }
        m_haveAnchor = true;
        m_anchorSeconds = positionSeconds;
    }

    // Returns true when a clock discontinuity or long event-loop suspension
    // was absorbed into a fresh grace window.
    bool normalizeClock(std::int64_t nowMs)
    {
        if (!m_haveClock) {
            m_haveClock = true;
            m_lastClockMs = nowMs;
            m_noProgressSinceMs = nowMs;
            return true;
        }
        const std::int64_t gapMs = nowMs - m_lastClockMs;
        m_lastClockMs = nowMs;
        if (gapMs < 0 || gapMs > m_timing.maximumPollGapMs) {
            m_noProgressSinceMs = nowMs;
            return true;
        }
        return false;
    }

    PlaybackStallEvent pollNormalized(std::int64_t nowMs)
    {
        if (m_state == PlaybackStallState::TimedOut)
            return PlaybackStallEvent::None;
        const std::int64_t elapsedMs = noProgressForMs(nowMs);
        if (elapsedMs >= m_timing.timeoutAfterMs) {
            m_state = PlaybackStallState::TimedOut;
            return PlaybackStallEvent::TimedOut;
        }
        if (elapsedMs >= m_timing.stallAfterMs
            && m_state == PlaybackStallState::Monitoring) {
            m_state = PlaybackStallState::Stalled;
            return PlaybackStallEvent::EnteredStall;
        }
        return PlaybackStallEvent::None;
    }

    PlaybackStallTiming m_timing;
    PlaybackStallState m_state = PlaybackStallState::Inactive;
    bool m_seeking = false;
    bool m_seekRestartArmed = false;
    bool m_reanchorNextPosition = false;
    bool m_haveAnchor = false;
    double m_anchorSeconds = 0.0;
    std::int64_t m_noProgressSinceMs = 0;
    std::int64_t m_lastClockMs = 0;
    bool m_haveClock = false;
};

} // namespace YanamiPlayback
