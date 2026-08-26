#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace YanamiPlayback {

enum class CompletionBoundary {
    Natural,
    Premature,
};

// Turns libmpv's keep-open EOF property into one classified decision per load.
// eof-reached by itself is not proof that a remote stream delivered its full
// duration, so automatic advance is allowed only near a known duration.
class PlaybackCompletionGate final
{
public:
    void reset()
    {
        m_eofReached = false;
        m_handled = false;
    }

    std::optional<CompletionBoundary> observe(
        bool eofReached,
        bool fileLoaded,
        double positionSeconds,
        double durationSeconds)
    {
        const bool risingEdge = eofReached && !m_eofReached;
        m_eofReached = eofReached;
        if (!risingEdge || !fileLoaded || m_handled)
            return std::nullopt;

        m_handled = true;
        return isNearVerifiedDuration(positionSeconds, durationSeconds)
            ? CompletionBoundary::Natural
            : CompletionBoundary::Premature;
    }

    bool handled() const { return m_handled; }

private:
    static bool isNearVerifiedDuration(
        double positionSeconds,
        double durationSeconds)
    {
        if (!std::isfinite(positionSeconds)
            || !std::isfinite(durationSeconds)
            || positionSeconds < 0.0
            || durationSeconds <= 0.0) {
            return false;
        }

        // Container timestamps and the last decoded frame rarely land on the
        // exact duration. Allow 1%, bounded to 2-15 seconds, while still
        // rejecting a materially truncated remote stream.
        const double toleranceSeconds = std::clamp(
            durationSeconds * 0.01, 2.0, 15.0);
        return positionSeconds + toleranceSeconds >= durationSeconds;
    }

    bool m_eofReached = false;
    bool m_handled = false;
};

} // namespace YanamiPlayback
