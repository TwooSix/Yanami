#include "PlaybackDescriptor.hpp"

#include <QJsonDocument>
#include <QTest>

class PlaybackDescriptorTests final : public QObject
{
    Q_OBJECT

private slots:
    void schemaEightOutcomePreservesReportingAndTrackFields()
    {
        const QByteArray response = QByteArrayLiteral(
            "{\"schemaVersion\":8,"
            "\"url\":\"https://media.example/episode.mkv\","
            "\"itemId\":\"episode-1\","
            "\"title\":\"Episode 1\","
            "\"reportSessionId\":\"report-session-1\","
            "\"audioTracks\":[{\"kind\":\"audio\",\"streamIndex\":2,\"selected\":true}],"
            "\"subtitleTracks\":[{\"kind\":\"subtitle\",\"streamIndex\":5,\"selected\":false}],"
            "\"externalSubtitles\":[],\"playbackWarnings\":[]}");
        const QJsonDocument document = QJsonDocument::fromJson(response);
        QVERIFY(document.isObject());
        const QVariantMap descriptor = YanamiPlayback::descriptorFromResponse(
            document.object(),
            {{QStringLiteral("requestedItemId"), QStringLiteral("series-1")}});

        // The requested Series may resolve to an Episode; caller request
        // identity, not item equality, decides whether this result is stale.
        QCOMPARE(descriptor.value(QStringLiteral("itemId")).toString(),
            QStringLiteral("episode-1"));
        QCOMPARE(descriptor.value(QStringLiteral("reportSessionId")).toString(),
            QStringLiteral("report-session-1"));
        QCOMPARE(descriptor.value(QStringLiteral("audioTracks")).toList().size(), 1);
        QCOMPARE(descriptor.value(QStringLiteral("subtitleTracks")).toList().size(), 1);
        QCOMPARE(descriptor.value(QStringLiteral("playbackContext")).toMap()
            .value(QStringLiteral("requestedItemId")).toString(),
            QStringLiteral("series-1"));
    }
};

QTEST_MAIN(PlaybackDescriptorTests)
#include "PlaybackDescriptorTests.moc"
