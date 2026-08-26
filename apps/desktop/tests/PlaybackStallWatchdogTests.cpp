#include <QtTest>

#include "PlaybackStallWatchdog.hpp"

#include <limits>

using YanamiPlayback::PlaybackStallEvent;
using YanamiPlayback::PlaybackStallState;
using YanamiPlayback::PlaybackStallTiming;
using YanamiPlayback::PlaybackStallWatchdog;

class PlaybackStallWatchdogTests final : public QObject
{
    Q_OBJECT

private slots:
    void inactiveWatchdogDoesNothing()
    {
        PlaybackStallWatchdog watchdog;

        QCOMPARE(watchdog.poll(30'000), PlaybackStallEvent::None);
        QCOMPARE(watchdog.state(), PlaybackStallState::Inactive);
    }

    void entersStallAndTimesOutOnlyOnce()
    {
        PlaybackStallWatchdog watchdog({2'500, 20'000, 60'000, 0.05});
        watchdog.arm(0, 12.0);

        QCOMPARE(watchdog.poll(2'499), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(2'500), PlaybackStallEvent::EnteredStall);
        QCOMPARE(watchdog.poll(5'000), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(19'999), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(20'000), PlaybackStallEvent::TimedOut);
        QCOMPARE(watchdog.poll(60'000), PlaybackStallEvent::None);
        QCOMPARE(
            watchdog.observePosition(60'001, 12.0),
            PlaybackStallEvent::None);
        QCOMPARE(
            watchdog.observePosition(60'002, 12.01),
            PlaybackStallEvent::None);
        QCOMPARE(
            watchdog.observePosition(
                60'003, std::numeric_limits<double>::quiet_NaN()),
            PlaybackStallEvent::None);
    }

    void productionCadenceReachesTheConfiguredBoundaries()
    {
        PlaybackStallWatchdog watchdog;
        watchdog.arm(0, 12.0);

        for (qint64 nowMs = 250; nowMs < 2'500; nowMs += 250)
            QCOMPARE(watchdog.poll(nowMs), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(2'500), PlaybackStallEvent::EnteredStall);
        for (qint64 nowMs = 2'750; nowMs < 20'000; nowMs += 250)
            QCOMPARE(watchdog.poll(nowMs), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(20'000), PlaybackStallEvent::TimedOut);
    }

    void forwardProgressClearsStallAndRearmsTheWindow()
    {
        PlaybackStallWatchdog watchdog({2'500, 20'000, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        QCOMPARE(watchdog.poll(2'500), PlaybackStallEvent::EnteredStall);

        QCOMPARE(
            watchdog.observePosition(2'600, 10.051),
            PlaybackStallEvent::StallCleared);
        QCOMPARE(watchdog.state(), PlaybackStallState::Monitoring);
        QCOMPARE(watchdog.poll(5'099), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(5'100), PlaybackStallEvent::EnteredStall);
    }

    void realProgressCanRecoverAfterAReportedTimeout()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        QCOMPARE(watchdog.poll(500), PlaybackStallEvent::TimedOut);

        QCOMPARE(
            watchdog.observePosition(600, 10.1),
            PlaybackStallEvent::StallCleared);
        QCOMPARE(watchdog.state(), PlaybackStallState::Monitoring);
        QCOMPARE(watchdog.poll(1'100), PlaybackStallEvent::TimedOut);
    }

    void jitterBackwardAndInvalidPositionsAreNotProgress()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);

        QCOMPARE(watchdog.observePosition(50, 10.01), PlaybackStallEvent::None);
        QCOMPARE(watchdog.observePosition(75, 9.0), PlaybackStallEvent::None);
        QCOMPARE(
            watchdog.observePosition(
                99, std::numeric_limits<double>::quiet_NaN()),
            PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(100), PlaybackStallEvent::EnteredStall);
    }

    void accumulatedSubEpsilonProgressCounts()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);

        QCOMPARE(watchdog.observePosition(40, 10.02), PlaybackStallEvent::None);
        QCOMPARE(watchdog.observePosition(80, 10.04), PlaybackStallEvent::None);
        QCOMPARE(watchdog.observePosition(90, 10.06), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(189), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(190), PlaybackStallEvent::EnteredStall);
    }

    void quarterSpeedPlaybackDoesNotFalsePositive()
    {
        PlaybackStallWatchdog watchdog({2'500, 20'000, 2'000, 0.05});
        watchdog.arm(0, 0.0);

        double positionSeconds = 0.0;
        for (qint64 nowMs = 250; nowMs <= 60'000; nowMs += 250) {
            positionSeconds += 0.0625;
            QCOMPARE(
                watchdog.observePosition(nowMs, positionSeconds),
                PlaybackStallEvent::None);
        }
        QCOMPARE(watchdog.state(), PlaybackStallState::Monitoring);
    }

    void externallyReportedStartupTimeoutRecoversOnRealProgress()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(1'000, 0.0);
        watchdog.markStalled(1'000, 0.0);

        QCOMPARE(
            watchdog.observePosition(1'100, 0.1),
            PlaybackStallEvent::StallCleared);
    }

    void userPauseSuspendsAndResumeGetsFreshGrace()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        QCOMPARE(watchdog.poll(100), PlaybackStallEvent::EnteredStall);
        QCOMPARE(
            watchdog.setPaused(true, 110, 10.0),
            PlaybackStallEvent::StallCleared);

        QCOMPARE(watchdog.poll(50'000), PlaybackStallEvent::None);
        QCOMPARE(watchdog.setPaused(false, 50'000, 10.0), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(50'099), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(50'100), PlaybackStallEvent::EnteredStall);
    }

    void pauseWhileMonitoringAlsoSuspendsTheDeadline()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        QCOMPARE(watchdog.setPaused(true, 50, 10.0), PlaybackStallEvent::None);

        QCOMPARE(watchdog.poll(5'000), PlaybackStallEvent::None);
        QCOMPARE(watchdog.setPaused(false, 5'000, 10.0), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(5'099), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(5'100), PlaybackStallEvent::EnteredStall);
    }

    void seekJumpDoesNotPretendPlaybackRecovered()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        QCOMPARE(watchdog.poll(100), PlaybackStallEvent::EnteredStall);

        watchdog.beginSeek(110, 10.0);
        watchdog.observeSeekStarted(115, 10.0);
        QCOMPARE(watchdog.observePosition(120, 80.0), PlaybackStallEvent::None);
        watchdog.observePlaybackRestart(130, 80.0);
        QCOMPARE(watchdog.state(), PlaybackStallState::Stalled);
        QCOMPARE(watchdog.observePosition(140, 80.1), PlaybackStallEvent::None);
        QCOMPARE(
            watchdog.observePosition(150, 80.2),
            PlaybackStallEvent::StallCleared);
    }

    void hungSeekStillTimesOut()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        QCOMPARE(watchdog.poll(100), PlaybackStallEvent::EnteredStall);
        watchdog.beginSeek(400, 10.0);
        watchdog.observeSeekStarted(450, 10.0);

        QCOMPARE(watchdog.poll(500), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(899), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(900), PlaybackStallEvent::TimedOut);
    }

    void seekPositionJumpsDoNotRefreshTheDeadline()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        watchdog.beginSeek(10, 10.0);
        watchdog.observeSeekStarted(20, 10.0);

        QCOMPARE(watchdog.observePosition(110, 80.0), PlaybackStallEvent::EnteredStall);
        QCOMPARE(watchdog.observePosition(300, 5.0), PlaybackStallEvent::None);
        QCOMPARE(watchdog.observePosition(509, 120.0), PlaybackStallEvent::None);
        QCOMPARE(watchdog.observePosition(510, 200.0), PlaybackStallEvent::TimedOut);
    }

    void stalePlaybackRestartCannotEndANewerSeek()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        watchdog.beginSeek(10, 10.0);

        // An initial-load restart that predates MPV_EVENT_SEEK is stale.
        watchdog.observePlaybackRestart(20, 10.0);
        QVERIFY(watchdog.seeking());
        QCOMPARE(watchdog.observePosition(30, 80.0), PlaybackStallEvent::None);

        watchdog.observeSeekStarted(40, 80.0);
        watchdog.observePlaybackRestart(50, 80.0);
        QVERIFY(!watchdog.seeking());
        QCOMPARE(watchdog.observePosition(60, 80.1), PlaybackStallEvent::None);
    }

    void newerSeekDisarmsThePreviousRestart()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        watchdog.beginSeek(10, 10.0);
        watchdog.observeSeekStarted(20, 10.0);

        watchdog.beginSeek(30, 10.0);
        watchdog.observePlaybackRestart(40, 10.0);
        QVERIFY(watchdog.seeking());
        watchdog.observeSeekStarted(50, 10.0);
        watchdog.observePlaybackRestart(60, 40.0);
        QVERIFY(!watchdog.seeking());
    }

    void longPollGapAndClockRegressionGetFreshGrace()
    {
        PlaybackStallWatchdog watchdog({100, 500, 200, 0.05});
        watchdog.arm(1'000, 10.0);

        QCOMPARE(watchdog.poll(10'000), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(10'099), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(10'100), PlaybackStallEvent::EnteredStall);

        watchdog.arm(1'000, 10.0);
        QCOMPARE(watchdog.poll(900), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(999), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(1'000), PlaybackStallEvent::EnteredStall);
    }

    void resetPreventsLateTimeoutAndNewLoadStartsClean()
    {
        PlaybackStallWatchdog watchdog({100, 500, 60'000, 0.05});
        watchdog.arm(0, 10.0);
        QCOMPARE(watchdog.poll(100), PlaybackStallEvent::EnteredStall);
        watchdog.reset();

        QCOMPARE(watchdog.poll(10'000), PlaybackStallEvent::None);
        watchdog.arm(10'000, 0.0);
        QCOMPARE(watchdog.poll(10'099), PlaybackStallEvent::None);
        QCOMPARE(watchdog.poll(10'100), PlaybackStallEvent::EnteredStall);
    }
};

QTEST_APPLESS_MAIN(PlaybackStallWatchdogTests)
#include "PlaybackStallWatchdogTests.moc"
