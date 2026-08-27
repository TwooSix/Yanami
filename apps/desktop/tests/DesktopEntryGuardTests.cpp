#include "DesktopEntryGuard.hpp"

#include <QFile>
#include <QtTest>

class DesktopEntryGuardTests final : public QObject
{
    Q_OBJECT

private slots:
    void directUserLaunchRedirectsToBootstrap()
    {
        QCOMPARE(
            DesktopEntryGuard::routeForLaunch({}),
            DesktopEntryGuard::Route::RedirectToBootstrap);
    }

    void validBootstrapChildRunsDesktopWithoutRecursion()
    {
        DesktopEntryGuard::LaunchContext context;
        context.usableBootstrapHandoff = true;
        QCOMPARE(
            DesktopEntryGuard::routeForLaunch(context),
            DesktopEntryGuard::Route::RunDesktop);
    }

    void explicitDirectAndAutomationModesRemainAvailable()
    {
        DesktopEntryGuard::LaunchContext direct;
        direct.explicitDirectDesktop = true;
        QCOMPARE(
            DesktopEntryGuard::routeForLaunch(direct),
            DesktopEntryGuard::Route::RunDesktop);

        DesktopEntryGuard::LaunchContext automation;
        automation.diagnosticOrPerformanceMode = true;
        QCOMPARE(
            DesktopEntryGuard::routeForLaunch(automation),
            DesktopEntryGuard::Route::RunDesktop);
    }

    void staleHandoffArgumentsAreNotForwardedToFreshBootstrap()
    {
        const QStringList forwarded = DesktopEntryGuard::forwardedArguments({
            QStringLiteral("C:/Yanami/yanami-desktop.exe"),
            QStringLiteral("--yanami-bootstrap-ready-file=C:/Temp/desktop-ready.json"),
            QStringLiteral("--yanami-bootstrap-ready-handle=123"),
            QStringLiteral("--performance-trace"),
            QStringLiteral("C:/Temp/trace.jsonl"),
        });
        QCOMPARE(forwarded, QStringList({
            QStringLiteral("--performance-trace"),
            QStringLiteral("C:/Temp/trace.jsonl"),
        }));
    }

    void desktopMainRoutesBeforeLoggerAndBackendConstruction()
    {
        QFile source(QStringLiteral(YANAMI_MAIN_SOURCE));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray main = source.readAll();

        const qsizetype decision = main.indexOf(
            "DesktopEntryGuard::routeForLaunch");
        const qsizetype redirect = main.indexOf(
            "DesktopEntryGuard::redirectToSiblingBootstrap");
        const qsizetype logger = main.indexOf("RuntimeLogger::install()");
        const qsizetype backend = main.indexOf(
            "DesktopBackendServices backendServices");
        QVERIFY(decision >= 0);
        QVERIFY(redirect > decision);
        QVERIFY(logger > redirect);
        QVERIFY(backend > logger);
        QVERIFY(main.contains("bootstrapHandoff.usable()"));
        QVERIFY(main.contains("--runtime-smoke-test"));
        QVERIFY(main.contains("--mpv-runtime-smoke-test"));
        QVERIFY(main.contains("--performance-runtime-probe"));
        QVERIFY(main.contains("--performance-runtime-auto-exit"));
        QVERIFY(main.contains(
            "YanamiPerformance::PerformanceTrace::enabled()"));
    }
};

QTEST_APPLESS_MAIN(DesktopEntryGuardTests)

#include "DesktopEntryGuardTests.moc"
