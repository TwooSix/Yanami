#pragma once

#include <QtGlobal>

namespace YanamiUpscaling {

struct HealthWindow
{
    quint64 renderSamples = 0;
    double renderP95Ms = 0.0;
    double estimatedFps = 0.0;
    qint64 outputDroppedFrames = 0;
    qint64 mistimedFrames = 0;
    qint64 delayedFrames = 0;
    double avSyncMs = 0.0;
    bool playing = false;
    bool buffering = false;
    bool paused = false;
};

class PerformanceProtection final
{
public:
    enum class Action {
        None,
        Downgrade,
        Disable,
    };

    void reset(int availableFallbacks, int reservedHeadroomPercent);
    Action evaluate(const HealthWindow &window);

    int consecutiveOverloadWindows() const
    { return m_consecutiveOverloadWindows; }
    int remainingFallbacks() const { return m_remainingFallbacks; }
    double lastFrameBudgetMs() const { return m_lastFrameBudgetMs; }
    double lastGuardBudgetMs() const { return m_lastGuardBudgetMs; }
    bool lastWindowOverloaded() const { return m_lastWindowOverloaded; }

private:
    static constexpr int kRequiredOverloadWindows = 3;
    static constexpr int kInitialWarmupWindows = 3;
    static constexpr int kPostDowngradeWarmupWindows = 2;

    int m_remainingFallbacks = 0;
    int m_reservedHeadroomPercent = 20;
    int m_warmupWindows = kInitialWarmupWindows;
    int m_consecutiveOverloadWindows = 0;
    double m_lastFrameBudgetMs = 0.0;
    double m_lastGuardBudgetMs = 0.0;
    bool m_lastWindowOverloaded = false;
};

} // namespace YanamiUpscaling
