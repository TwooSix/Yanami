#include "MpvApi.hpp"

#include <QFileInfo>
#include <QtTest>

#include <clocale>

class MpvApiTests final : public QObject
{
    Q_OBJECT

private slots:
    void resolvesCompleteRuntimeOnlyOnDemand()
    {
        MpvApi &runtime = MpvApi::instance();
        QVERIFY(!runtime.isLoaded());
        QVERIFY(runtime.functions() == nullptr);

        QString error;
        const MpvFunctions *functions = runtime.load(&error);
        QVERIFY2(functions, qPrintable(error));
        QVERIFY(runtime.isLoaded());
        QCOMPARE(runtime.functions(), functions);
        QVERIFY(!runtime.loadedFileName().isEmpty());
        QVERIFY(QFileInfo::exists(runtime.loadedFileName()));

        // The centralized resolver is fail-closed: reaching this point means
        // every function used by MpvVideoItem was present. Exercise the first
        // and final handle calls without initializing playback.
        QVERIFY(std::setlocale(LC_NUMERIC, "C") != nullptr);
        mpv_handle *handle = functions->create();
        QVERIFY(handle != nullptr);
        functions->terminateDestroy(handle);
    }
};

QTEST_GUILESS_MAIN(MpvApiTests)

#include "MpvApiTests.moc"
