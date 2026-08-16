#include <QtTest>

#include "DevelopmentHooks.hpp"

class DevelopmentHooksTests final : public QObject
{
    Q_OBJECT

private slots:
    void productionBuildIgnoresHostileEnvironment()
    {
        QVERIFY(qputenv("YANAMI_DEV_AUTOPLAY_FIRST", "1"));
        QVERIFY(qputenv("YANAMI_DEV_SCREENSHOT_PATH", "C:/tmp/should-not-exist.png"));
        QVERIFY(qputenv("YANAMI_DEV_COLLECTION_DELAY_MS", "30000"));
        QVERIFY(qputenv("YANAMI_DEV_DANMAKU_STYLE_STRESS_COUNT", "500"));

        QVERIFY(!DevelopmentHooks::enabled());
        QVERIFY(DevelopmentHooks::value(
                    DevelopmentHooks::Variable::AutoplayFirst).isEmpty());
        QVERIFY(DevelopmentHooks::value(DevelopmentHooks::Variable::ScreenshotPath).isEmpty());
        QVERIFY(DevelopmentHooks::bytes(
                    DevelopmentHooks::Variable::DanmakuStyleStressCount).isEmpty());
        QVERIFY(!DevelopmentHooks::isSet(
                    DevelopmentHooks::Variable::AutoplayFirst));
        bool parsed = true;
        QCOMPARE(
            DevelopmentHooks::intValue(
                DevelopmentHooks::Variable::CollectionDelayMs, &parsed),
            0);
        QVERIFY(!parsed);

        qunsetenv("YANAMI_DEV_AUTOPLAY_FIRST");
        qunsetenv("YANAMI_DEV_SCREENSHOT_PATH");
        qunsetenv("YANAMI_DEV_COLLECTION_DELAY_MS");
        qunsetenv("YANAMI_DEV_DANMAKU_STYLE_STRESS_COUNT");
    }
};

QTEST_APPLESS_MAIN(DevelopmentHooksTests)

#include "DevelopmentHooksTests.moc"
