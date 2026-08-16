#include <QtTest>

#include "MpvVideoItem.hpp"
#include "PlaybackReporter.hpp"

#include <memory>
#include <utility>

namespace {

struct RecordedPlaybackReport
{
    PlaybackPort::Event event;
    PlaybackPort::Snapshot snapshot;
};

class FakePlaybackPort final : public PlaybackPort
{
public:
    using PlaybackPort::PlaybackPort;

    void prepare(const QString &, const QString &) override { }
    void prepareInContext(
        const QString &, const QString &, const QVariantMap &) override
    {
    }
    void cancelPreparation() override { }
    void switchTo(
        const QString &, const QString &, double, bool) override
    {
    }
    void switchToInContext(
        const QString &, const QString &, const QVariantMap &, double,
        bool) override
    {
    }

    void report(Event event, const Snapshot &snapshot) override
    {
        reports.push_back({event, snapshot});
    }

    qsizetype count(Event event) const
    {
        qsizetype result = 0;
        for (const RecordedPlaybackReport &report : reports) {
            if (report.event == event)
                ++result;
        }
        return result;
    }

    const RecordedPlaybackReport &at(qsizetype index) const
    {
        return reports.at(index);
    }

    QList<RecordedPlaybackReport> reports;
};

// MpvVideoItem observes a few libmpv properties as soon as it is constructed.
// Give those queued notifications turns in which the reporter is inactive so
// they cannot be mistaken for a signal deliberately exercised by a test.
void drainStartupNotifications()
{
    for (int turn = 0; turn < 3; ++turn) {
        QEventLoop loop;
        QTimer::singleShot(0, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

void runOneQueuedTurn()
{
    QEventLoop loop;
    QTimer::singleShot(0, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

class PlaybackReporterTests final : public QObject
{
    Q_OBJECT

private slots:
    void tracksChangedStartsOnceAndCapturesDefaultSnapshot()
    {
        FakePlaybackPort port;
        MpvVideoItem player;
        PlaybackReporter reporter(&port);
        QVERIFY(reporter.attachPlayer(&player));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("  session-a  "), {}));
        QCOMPARE(port.reports.size(), 0);

        player.tracksChanged();
        player.tracksChanged();

        QCOMPARE(port.count(PlaybackPort::Event::Started), 1);
        QCOMPARE(port.reports.size(), 1);
        const PlaybackPort::Snapshot &snapshot = port.at(0).snapshot;
        QCOMPARE(snapshot.reportSessionId, QStringLiteral("session-a"));
        QCOMPARE(snapshot.positionSeconds, 0.0);
        QCOMPARE(snapshot.paused, false);
        QCOMPARE(snapshot.muted, false);
        QCOMPARE(snapshot.volume, 100.0);
        QCOMPARE(snapshot.rate, 1.0);
        QCOMPARE(snapshot.audioStreamIndex, -1);
        QCOMPARE(snapshot.subtitleStreamIndex, -1);
        QCOMPARE(snapshot.seekable, false);

        reporter.stopSession();
    }

    void fallbackThenTracksChangedStillStartsOnlyOnce()
    {
        FakePlaybackPort port;
        MpvVideoItem player;
        PlaybackReporter reporter(
            &port, nullptr, PlaybackReporterTiming{5, 10'000});
        QVERIFY(reporter.attachPlayer(&player));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("fallback"), {}));
        QTRY_COMPARE_WITH_TIMEOUT(
            port.count(PlaybackPort::Event::Started), 1, 1000);

        player.tracksChanged();
        QCOMPARE(port.count(PlaybackPort::Event::Started), 1);

        reporter.stopSession();
    }

    void stopBeforeFallbackReportsStartedThenStopped()
    {
        FakePlaybackPort port;
        MpvVideoItem player;
        PlaybackReporter reporter(&port);
        QVERIFY(reporter.attachPlayer(&player));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("early-stop"), {}));
        player.seek(42.5);
        reporter.stopSession();

        QCOMPARE(port.reports.size(), 2);
        QCOMPARE(port.at(0).event, PlaybackPort::Event::Started);
        QCOMPARE(port.at(0).snapshot.reportSessionId,
                 QStringLiteral("early-stop"));
        QCOMPARE(port.at(0).snapshot.positionSeconds, 42.5);
        QCOMPARE(port.at(1).event, PlaybackPort::Event::Stopped);
        QCOMPARE(port.at(1).snapshot.reportSessionId,
                 QStringLiteral("early-stop"));
    }

    void fileEndedThenExplicitStopReportsStoppedOnce()
    {
        FakePlaybackPort port;
        MpvVideoItem player;
        PlaybackReporter reporter(&port);
        QVERIFY(reporter.attachPlayer(&player));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("ended"), {}));
        player.tracksChanged();
        player.fileEnded();
        reporter.stopSession();

        QCOMPARE(port.count(PlaybackPort::Event::Started), 1);
        QCOMPARE(port.count(PlaybackPort::Event::Stopped), 1);
        QCOMPARE(port.reports.size(), 2);
    }

    void destroyedPlayerStopsActiveSessionOnce()
    {
        FakePlaybackPort port;
        PlaybackReporter reporter(&port);
        auto player = std::make_unique<MpvVideoItem>();
        QVERIFY(reporter.attachPlayer(player.get()));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("destroyed"), {}));
        player->tracksChanged();
        player.reset();
        reporter.stopSession();

        QCOMPARE(port.count(PlaybackPort::Event::Started), 1);
        QCOMPARE(port.count(PlaybackPort::Event::Stopped), 1);
        QCOMPARE(port.at(1).snapshot.reportSessionId,
                 QStringLiteral("destroyed"));
    }

    void replacingPlayerStopsOldSessionAndDisconnectsIt()
    {
        FakePlaybackPort port;
        MpvVideoItem firstPlayer;
        MpvVideoItem secondPlayer;
        PlaybackReporter reporter(&port);
        QVERIFY(reporter.attachPlayer(&firstPlayer));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("first"), {}));
        firstPlayer.tracksChanged();
        QVERIFY(reporter.attachPlayer(&secondPlayer));

        QVERIFY(reporter.beginSession(QStringLiteral("second"), {}));
        secondPlayer.tracksChanged();
        firstPlayer.fileEnded();

        QCOMPARE(port.reports.size(), 3);
        QCOMPARE(port.at(0).event, PlaybackPort::Event::Started);
        QCOMPARE(port.at(0).snapshot.reportSessionId, QStringLiteral("first"));
        QCOMPARE(port.at(1).event, PlaybackPort::Event::Stopped);
        QCOMPARE(port.at(1).snapshot.reportSessionId, QStringLiteral("first"));
        QCOMPARE(port.at(2).event, PlaybackPort::Event::Started);
        QCOMPARE(port.at(2).snapshot.reportSessionId, QStringLiteral("second"));

        reporter.stopSession();
        QCOMPARE(port.count(PlaybackPort::Event::Stopped), 2);
    }

    void queuedProgressFromOldSessionCannotPolluteNewSession()
    {
        FakePlaybackPort port;
        MpvVideoItem player;
        PlaybackReporter reporter(&port);
        QVERIFY(reporter.attachPlayer(&player));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("old"), {}));
        player.tracksChanged();
        player.volumeChanged(); // Queues, but does not immediately report.
        reporter.stopSession();

        QVERIFY(reporter.beginSession(QStringLiteral("new"), {}));
        player.tracksChanged();
        // The stale zero-timer was registered before this barrier. It must be
        // consumed without being allowed to report against the replacement
        // session. One turn avoids folding unrelated future player activity
        // into this generation-boundary assertion.
        runOneQueuedTurn();

        QCOMPARE(port.count(PlaybackPort::Event::Progress), 0);
        QCOMPARE(port.reports.size(), 3);
        QCOMPARE(port.at(0).snapshot.reportSessionId, QStringLiteral("old"));
        QCOMPARE(port.at(1).snapshot.reportSessionId, QStringLiteral("old"));
        QCOMPARE(port.at(2).snapshot.reportSessionId, QStringLiteral("new"));

        reporter.stopSession();
    }

    void sessionTransitionAbandonsWithoutCrossSessionTransport()
    {
        FakePlaybackPort port;
        MpvVideoItem player;
        PlaybackReporter reporter(&port);
        QVERIFY(reporter.attachPlayer(&player));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("old-account"), {}));
        player.tracksChanged();
        player.volumeChanged();
        reporter.abandonSessionForTransition();
        reporter.stopSession();
        runOneQueuedTurn();

        QCOMPARE(port.count(PlaybackPort::Event::Started), 1);
        QCOMPARE(port.count(PlaybackPort::Event::Progress), 0);
        QCOMPARE(port.count(PlaybackPort::Event::Stopped), 0);

        QVERIFY(reporter.beginSession(QStringLiteral("new-account"), {}));
        player.tracksChanged();
        reporter.stopSession();
        QCOMPARE(port.count(PlaybackPort::Event::Started), 2);
        QCOMPARE(port.count(PlaybackPort::Event::Stopped), 1);
        QCOMPARE(port.reports.constLast().snapshot.reportSessionId,
            QStringLiteral("new-account"));
    }

    void heartbeatIntervalIsOwnedByReporter()
    {
        FakePlaybackPort port;
        MpvVideoItem player;
        PlaybackReporter reporter(
            &port, nullptr, PlaybackReporterTiming{5, 20});
        QVERIFY(reporter.attachPlayer(&player));
        drainStartupNotifications();

        QVERIFY(reporter.beginSession(QStringLiteral("heartbeat"), {}));
        player.tracksChanged();
        QTRY_VERIFY_WITH_TIMEOUT(
            port.count(PlaybackPort::Event::Progress) >= 1, 300);
        for (const RecordedPlaybackReport &report : std::as_const(port.reports)) {
            QCOMPARE(report.snapshot.reportSessionId,
                QStringLiteral("heartbeat"));
        }
        reporter.stopSession();
    }
};

QTEST_MAIN(PlaybackReporterTests)

#include "PlaybackReporterTests.moc"
