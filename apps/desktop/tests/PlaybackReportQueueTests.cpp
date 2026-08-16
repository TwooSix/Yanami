#include "PlaybackReportQueue.hpp"

#include <QtTest>

namespace {

YanamiPlaybackReportRequest request(
    PlaybackPort::Event event,
    const QString &reportSessionId,
    quint64 sessionGeneration = 1)
{
    PlaybackPort::Snapshot snapshot;
    snapshot.reportSessionId = reportSessionId;
    return {event, snapshot, sessionGeneration};
}

} // namespace

class PlaybackReportQueueTests final : public QObject
{
    Q_OBJECT

private slots:
    void coalescesOnlyWithinTheSameReportSession()
    {
        QQueue<YanamiPlaybackReportRequest> queue;
        YanamiPlayback::enqueueReport(queue,
            request(PlaybackPort::Event::Progress, QStringLiteral("A")));
        YanamiPlayback::enqueueReport(queue,
            request(PlaybackPort::Event::Progress, QStringLiteral("B")));
        YanamiPlayback::enqueueReport(queue,
            request(PlaybackPort::Event::Progress, QStringLiteral("A")));
        QCOMPARE(queue.size(), 2);
        QCOMPARE(queue.at(0).snapshot.reportSessionId, QStringLiteral("B"));
        QCOMPARE(queue.at(1).snapshot.reportSessionId, QStringLiteral("A"));

        YanamiPlayback::enqueueReport(queue,
            request(PlaybackPort::Event::Stopped, QStringLiteral("A")));
        QCOMPARE(queue.size(), 2);
        QCOMPARE(queue.at(0).snapshot.reportSessionId, QStringLiteral("B"));
        QCOMPARE(queue.at(1).event, PlaybackPort::Event::Stopped);
    }

    void bindsAfterStopToLatestSpecificSession()
    {
        const std::optional<YanamiPlaybackReportRequest> active =
            request(PlaybackPort::Event::Stopped, QStringLiteral("A"));
        QQueue<YanamiPlaybackReportRequest> queue;
        queue.enqueue(
            request(PlaybackPort::Event::Stopped, QStringLiteral("B")));
        QCOMPARE(YanamiPlayback::latestStoppedReportSessionId(active, queue),
            QStringLiteral("B"));
    }

    void rejectsOldAccountReports()
    {
        QQueue<YanamiPlaybackReportRequest> queue;
        queue.enqueue(request(
            PlaybackPort::Event::Stopped, QStringLiteral("A"), 1));
        queue.enqueue(request(
            PlaybackPort::Event::Progress, QStringLiteral("B"), 2));
        YanamiPlayback::discardForeignSessionReports(queue, 2, false);
        QCOMPARE(queue.size(), 1);
        QCOMPARE(queue.head().snapshot.reportSessionId, QStringLiteral("B"));
        QVERIFY(!YanamiPlayback::belongsToSession(
            request(PlaybackPort::Event::Stopped, QStringLiteral("A"), 1),
            2, false));
        QVERIFY(!YanamiPlayback::belongsToSession(
            request(PlaybackPort::Event::Stopped, QStringLiteral("B"), 2),
            2, true));
    }
};

QTEST_MAIN(PlaybackReportQueueTests)
#include "PlaybackReportQueueTests.moc"
