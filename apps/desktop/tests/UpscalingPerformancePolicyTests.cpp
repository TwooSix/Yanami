#include "UpscalingPerformancePolicy.hpp"

#include <QTest>

using YanamiUpscaling::HealthWindow;
using YanamiUpscaling::PerformanceProtection;

namespace {

HealthWindow healthyWindow()
{
    return {
        .renderSamples = 60,
        .renderP95Ms = 7.0,
        .estimatedFps = 60.0,
        .outputDroppedFrames = 0,
        .mistimedFrames = 0,
        .delayedFrames = 0,
        .avSyncMs = 4.0,
        .playing = true,
    };
}

void finishWarmup(PerformanceProtection &policy)
{
    const HealthWindow window = healthyWindow();
    for (int index = 0; index < 3; ++index)
        QCOMPARE(policy.evaluate(window), PerformanceProtection::Action::None);
}

} // namespace

class UpscalingPerformancePolicyTests final : public QObject
{
    Q_OBJECT

private slots:
    void requiresThreeConsecutiveOverloadWindows()
    {
        PerformanceProtection policy;
        policy.reset(2, 20);
        finishWarmup(policy);

        HealthWindow overloaded = healthyWindow();
        overloaded.renderP95Ms = 15.0;
        QCOMPARE(policy.evaluate(overloaded), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(overloaded), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(overloaded), PerformanceProtection::Action::Downgrade);
        QCOMPARE(policy.remainingFallbacks(), 1);
    }

    void aHealthyWindowBreaksTheOverloadSequence()
    {
        PerformanceProtection policy;
        policy.reset(1, 20);
        finishWarmup(policy);

        HealthWindow overloaded = healthyWindow();
        overloaded.renderP95Ms = 15.0;
        QCOMPARE(policy.evaluate(overloaded), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(overloaded), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(healthyWindow()), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(overloaded), PerformanceProtection::Action::None);
        QCOMPARE(policy.consecutiveOverloadWindows(), 1);
    }

    void ignoresPausedBufferingAndInsufficientSamples()
    {
        PerformanceProtection policy;
        policy.reset(1, 20);
        finishWarmup(policy);

        HealthWindow window = healthyWindow();
        window.renderP95Ms = 50.0;
        window.paused = true;
        QCOMPARE(policy.evaluate(window), PerformanceProtection::Action::None);
        window.paused = false;
        window.buffering = true;
        QCOMPARE(policy.evaluate(window), PerformanceProtection::Action::None);
        window.buffering = false;
        window.renderSamples = 3;
        QCOMPARE(policy.evaluate(window), PerformanceProtection::Action::None);
    }

    void outputTimingAndAvSyncCanTriggerProtection()
    {
        PerformanceProtection policy;
        policy.reset(1, 20);
        finishWarmup(policy);

        HealthWindow timing = healthyWindow();
        timing.outputDroppedFrames = 1;
        for (int index = 0; index < 2; ++index)
            QCOMPARE(policy.evaluate(timing), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(timing), PerformanceProtection::Action::Downgrade);

        // The post-downgrade warm-up consumes two valid windows.
        QCOMPARE(policy.evaluate(healthyWindow()), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(healthyWindow()), PerformanceProtection::Action::None);
        HealthWindow drift = healthyWindow();
        drift.avSyncMs = 60.0;
        for (int index = 0; index < 2; ++index)
            QCOMPARE(policy.evaluate(drift), PerformanceProtection::Action::None);
        QCOMPARE(policy.evaluate(drift), PerformanceProtection::Action::Disable);
    }

    void decoderOnlyPressureDoesNotDowngradeShaders()
    {
        PerformanceProtection policy;
        policy.reset(1, 20);
        finishWarmup(policy);

        HealthWindow window = healthyWindow();
        // Decoder drops are absent from HealthWindow by design.
        for (int index = 0; index < 5; ++index)
            QCOMPARE(policy.evaluate(window), PerformanceProtection::Action::None);
        QCOMPARE(policy.remainingFallbacks(), 1);
    }

    void headroomChangesTheGuardBudget()
    {
        PerformanceProtection policy;
        policy.reset(1, 40);
        finishWarmup(policy);
        QCOMPARE(policy.lastFrameBudgetMs(), 1000.0 / 60.0);
        QCOMPARE(policy.lastGuardBudgetMs(), 10.0);
    }
};

QTEST_GUILESS_MAIN(UpscalingPerformancePolicyTests)

#include "UpscalingPerformancePolicyTests.moc"
