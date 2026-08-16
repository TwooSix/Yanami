#include <QtTest>

#include "PlaybackTrackMapper.hpp"

class PlaybackTrackMapperTests final : public QObject
{
    Q_OBJECT

private slots:
    void mapsInternalTrackByFfIndex()
    {
        const QVariantList mpvTracks{QVariantMap{
            {QStringLiteral("mpvTrackId"), 7},
            {QStringLiteral("ffIndex"), 3},
            {QStringLiteral("language"), QStringLiteral("jpn")},
        }};
        const QVariantList embyTracks{
            QVariantMap{{QStringLiteral("kind"), QStringLiteral("audio")},
                        {QStringLiteral("streamIndex"), 2},
                        {QStringLiteral("external"), false}},
            QVariantMap{{QStringLiteral("kind"), QStringLiteral("audio")},
                        {QStringLiteral("streamIndex"), 3},
                        {QStringLiteral("external"), false}},
        };
        QCOMPARE(
            YanamiPlayback::selectedStreamIndex(
                mpvTracks, 7, embyTracks, QStringLiteral("audio")),
            std::optional<int>(3));
    }

    void mapsExternalTrackByUrl()
    {
        const QVariantList mpvTracks{QVariantMap{
            {QStringLiteral("mpvTrackId"), 4},
            {QStringLiteral("externalUrl"),
             QStringLiteral("https://emby.test/Videos/1/Subtitles/8/Stream.srt#ignored")},
        }};
        const QVariantList embyTracks{QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("subtitle")},
            {QStringLiteral("streamIndex"), 8},
            {QStringLiteral("external"), true},
            {QStringLiteral("deliveryUrl"),
             QStringLiteral("https://emby.test/Videos/1/Subtitles/8/Stream.srt")},
        }};
        QCOMPARE(
            YanamiPlayback::selectedStreamIndex(
                mpvTracks, 4, embyTracks, QStringLiteral("subtitle")),
            std::optional<int>(8));
    }

    void usesOnlyUniqueMetadataFallback()
    {
        const QVariantList mpvTracks{QVariantMap{
            {QStringLiteral("mpvTrackId"), 9},
            {QStringLiteral("language"), QStringLiteral("JPN")},
            {QStringLiteral("codec"), QStringLiteral("AAC")},
            {QStringLiteral("title"), QStringLiteral("Stereo")},
        }};
        QVariantList embyTracks{QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("audio")},
            {QStringLiteral("streamIndex"), 1},
            {QStringLiteral("external"), false},
            {QStringLiteral("language"), QStringLiteral("jpn")},
            {QStringLiteral("codec"), QStringLiteral("aac")},
            {QStringLiteral("title"), QStringLiteral("stereo")},
        }};
        QCOMPARE(
            YanamiPlayback::selectedStreamIndex(
                mpvTracks, 9, embyTracks, QStringLiteral("audio")),
            std::optional<int>(1));

        QVariantMap duplicate = embyTracks.front().toMap();
        duplicate.insert(QStringLiteral("streamIndex"), 6);
        embyTracks.push_back(duplicate);
        QVERIFY(!YanamiPlayback::selectedStreamIndex(
                     mpvTracks, 9, embyTracks, QStringLiteral("audio"))
                     .has_value());
    }

    void refusesMpvIdAndWrongKindFallbacks()
    {
        const QVariantList mpvTracks{QVariantMap{
            {QStringLiteral("mpvTrackId"), 42},
            {QStringLiteral("language"), QStringLiteral("eng")},
        }};
        const QVariantList embyTracks{QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("subtitle")},
            {QStringLiteral("streamIndex"), 42},
            {QStringLiteral("external"), false},
            {QStringLiteral("language"), QStringLiteral("eng")},
        }};
        QVERIFY(!YanamiPlayback::selectedStreamIndex(
                     mpvTracks, 42, embyTracks, QStringLiteral("audio"))
                     .has_value());

        QVariantMap untypedTrack = embyTracks.constFirst().toMap();
        untypedTrack.remove(QStringLiteral("kind"));
        QVERIFY(!YanamiPlayback::selectedStreamIndex(
                     mpvTracks, 42, QVariantList{untypedTrack},
                     QStringLiteral("subtitle"))
                     .has_value());
    }
};

QTEST_APPLESS_MAIN(PlaybackTrackMapperTests)

#include "PlaybackTrackMapperTests.moc"
