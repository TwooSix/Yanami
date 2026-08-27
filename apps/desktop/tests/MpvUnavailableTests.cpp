#include "MpvApi.hpp"
#include "MpvVideoItem.hpp"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class MpvUnavailableTests final : public QObject
{
    Q_OBJECT

private slots:
    void playerStaysConstructibleAndPublishesVisibleError()
    {
        QVERIFY(!MpvApi::instance().isLoaded());
        QTemporaryDir assets;
        QVERIFY(assets.isValid());

        MpvVideoItem player(nullptr, assets.path());
        QVERIFY(!player.backendAvailable());
        QVERIFY(!player.initializationError().isEmpty());

        QSignalSpy errors(&player, &MpvVideoItem::playbackError);
        player.open(QUrl(QStringLiteral("https://example.invalid/video")));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(
            errors.constFirst().constFirst().toString(),
            player.initializationError());
    }

    void playerPageSurfacesUnavailableRuntimeInStatusToast()
    {
        QFile playerPage(QStringLiteral(YANAMI_PLAYER_PAGE_SOURCE));
        QVERIFY2(playerPage.open(QIODevice::ReadOnly | QIODevice::Text),
            qPrintable(playerPage.errorString()));
        const QString source = QString::fromUtf8(playerPage.readAll());

        QVERIFY2(source.contains(QStringLiteral("!player.backendAvailable")),
            "PlayerPage must detect an unavailable on-demand mpv runtime");
        QVERIFY2(source.contains(QStringLiteral(
                     "playerStatusToast.show(\n"
                     "                    player.initializationError, \"error\", 8000)")),
            "Entering PlayerPage must show the mpv loader failure to the user");
    }
};

QTEST_MAIN(MpvUnavailableTests)

#include "MpvUnavailableTests.moc"
