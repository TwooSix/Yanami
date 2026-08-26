#include "UpscalingPerformancePolicy.hpp"

#include <algorithm>
#include <cmath>

namespace YanamiUpscaling {

void PerformanceProtection::reset(
    int availableFallbacks, int reservedHeadroomPercent)
{
    m_remainingFallbacks = std::max(0, availableFallbacks);
    m_reservedHeadroomPercent = std::clamp(
        reservedHeadroomPercent, 10, 40);
    m_warmupWindows = kInitialWarmupWindows;
    m_consecutiveOverloadWindows = 0;
    m_lastFrameBudgetMs = 0.0;
    m_lastGuardBudgetMs = 0.0;
    m_lastWindowOverloaded = false;
}

PerformanceProtection::Action PerformanceProtection::evaluate(
    const HealthWindow &window)
{
    if (!window.playing || window.buffering || window.paused
        || window.renderSamples < 10 || !std::isfinite(window.estimatedFps)
        || window.estimatedFps <= 0.0) {
        m_consecutiveOverloadWindows = 0;
        m_lastWindowOverloaded = false;
        return Action::None;
    }

    const double effectiveFps = std::max(24.0, window.estimatedFps);
    m_lastFrameBudgetMs = 1000.0 / effectiveFps;
    m_lastGuardBudgetMs = m_lastFrameBudgetMs
        * (100.0 - m_reservedHeadroomPercent) / 100.0;

    if (m_warmupWindows > 0) {
        --m_warmupWindows;
        m_consecutiveOverloadWindows = 0;
        m_lastWindowOverloaded = false;
        return Action::None;
    }

    // Output-side timing is attributable to the render pipeline. Decoder
    // drops are intentionally excluded: downgrading a shader cannot repair a
    // decoder or network bottleneck.
    const double expectedFrames = std::max(1.0, effectiveFps);
    const double outputTimingRatio = static_cast<double>(
        std::max<qint64>(0, window.outputDroppedFrames)
        + std::max<qint64>(0, window.mistimedFrames)
        + std::max<qint64>(0, window.delayedFrames)) / expectedFrames;
    m_lastWindowOverloaded =
        (!std::isfinite(window.renderP95Ms)
            || window.renderP95Ms > m_lastGuardBudgetMs)
        || outputTimingRatio > 0.01
        || !std::isfinite(window.avSyncMs)
        || std::abs(window.avSyncMs) > 50.0;

    if (!m_lastWindowOverloaded) {
        m_consecutiveOverloadWindows = 0;
        return Action::None;
    }

    ++m_consecutiveOverloadWindows;
    if (m_consecutiveOverloadWindows < kRequiredOverloadWindows)
        return Action::None;

    m_consecutiveOverloadWindows = 0;
    m_warmupWindows = kPostDowngradeWarmupWindows;
    if (m_remainingFallbacks > 0) {
        --m_remainingFallbacks;
        return Action::Downgrade;
    }
    return Action::Disable;
}

} // namespace YanamiUpscaling
