#include "PlaybackTelemetry.hpp"

#include <QTest>

#include <limits>

class PlaybackTelemetryTests final : public QObject
{
    Q_OBJECT

private slots:
    void volumeIsRoundedAndClampedForTheRustU8Contract()
    {
        QCOMPARE(YanamiPlayback::telemetryVolume(42.5), 43);
        QCOMPARE(YanamiPlayback::telemetryVolume(-0.25), 0);
        QCOMPARE(YanamiPlayback::telemetryVolume(120.0), 100);
        QCOMPARE(YanamiPlayback::telemetryVolume(
            std::numeric_limits<double>::quiet_NaN()), 100);
        QCOMPARE(YanamiPlayback::telemetryVolume(
            std::numeric_limits<double>::infinity()), 100);
        QCOMPARE(YanamiPlayback::telemetryVolume(
            -std::numeric_limits<double>::infinity()), 0);
    }

    void payloadMatchesSchemaEightTelemetryFields()
    {
        PlaybackPort::Snapshot snapshot;
        snapshot.reportSessionId = QStringLiteral("session-8");
        snapshot.positionSeconds = 1.25;
        snapshot.volume = 42.5;
        snapshot.audioStreamIndex = 3;
        snapshot.subtitleStreamIndex = -1;
        const QVariantMap payload = YanamiPlayback::telemetryPayload(
            PlaybackPort::Event::Progress, snapshot);

        QCOMPARE(payload.value(QStringLiteral("event")).toString(),
            QStringLiteral("progress"));
        QCOMPARE(payload.value(QStringLiteral("positionTicks")).toULongLong(),
            12'500'000ULL);
        QCOMPARE(payload.value(QStringLiteral("volume")).toInt(), 43);
        QCOMPARE(payload.value(QStringLiteral("audioStreamIndex")).toInt(), 3);
        QVERIFY(!payload.contains(QStringLiteral("subtitleStreamIndex")));
    }
};

QTEST_MAIN(PlaybackTelemetryTests)
#include "PlaybackTelemetryTests.moc"
