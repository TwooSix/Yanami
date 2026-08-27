#include "BootstrapReadySignal.hpp"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include <string>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class BootstrapReadySignalTests final : public QObject
{
    Q_OBJECT

private slots:
    void handoffTemporaryRootsIncludeQtAndNativeRoots()
    {
        const QStringList roots = bootstrapHandoffTemporaryRoots();
        QVERIFY(roots.contains(QDir::tempPath()));
#ifdef Q_OS_WIN
        std::wstring nativeTemporaryRoot(32768, L'\0');
        const DWORD rootLength = GetTempPathW(
            static_cast<DWORD>(nativeTemporaryRoot.size()),
            nativeTemporaryRoot.data());
        QVERIFY(rootLength > 0);
        QVERIFY(rootLength < nativeTemporaryRoot.size());
        const QString nativeRoot = QString::fromWCharArray(
            nativeTemporaryRoot.data(), static_cast<qsizetype>(rootLength));
        QVERIFY(roots.contains(nativeRoot));
#endif
    }

    void handoffParentValidationAcceptsEveryFrozenRootOnly()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QDir fixtureRoot(fixture.path());
        QVERIFY(fixtureRoot.mkpath(QStringLiteral("temporary-a/YanamiBootstrap-a")));
        QVERIFY(fixtureRoot.mkpath(QStringLiteral("temporary-b/YanamiBootstrap-b")));
        QVERIFY(fixtureRoot.mkpath(QStringLiteral("outside/YanamiBootstrap-c")));

        const QString rootA = fixtureRoot.filePath(QStringLiteral("temporary-a"));
        const QString rootB = fixtureRoot.filePath(QStringLiteral("temporary-b"));
        const QString parentA = fixtureRoot.filePath(
            QStringLiteral("temporary-a/YanamiBootstrap-a"));
        const QString parentB = fixtureRoot.filePath(
            QStringLiteral("temporary-b/YanamiBootstrap-b"));
        const QString outside = fixtureRoot.filePath(
            QStringLiteral("outside/YanamiBootstrap-c"));

        QCOMPARE(
            validateBootstrapHandoffParent(parentA, {rootA, rootB}),
            BootstrapHandoffParentValidation::Allowed);
        QCOMPARE(
            validateBootstrapHandoffParent(parentB, {rootA, rootB}),
            BootstrapHandoffParentValidation::Allowed);
        QCOMPARE(
            validateBootstrapHandoffParent(rootA, {rootA, rootB}),
            BootstrapHandoffParentValidation::OutsideTemporaryRoots);
        QCOMPARE(
            validateBootstrapHandoffParent(outside, {rootA, rootB}),
            BootstrapHandoffParentValidation::OutsideTemporaryRoots);
        QCOMPARE(
            validateBootstrapHandoffParent(
                fixtureRoot.filePath(QStringLiteral("missing")),
                {rootA, rootB}),
            BootstrapHandoffParentValidation::CanonicalPathUnavailable);
    }

    void inheritedEventSignals()
    {
#ifdef Q_OS_WIN
        SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE event = CreateEventW(&security, TRUE, FALSE, nullptr);
        QVERIFY(event != nullptr);
        const QString argument = QStringLiteral(
            "--yanami-bootstrap-ready-handle=%1")
                .arg(reinterpret_cast<quintptr>(event));
        const BootstrapReadySignal signal =
            bootstrapReadySignalFromArguments({argument});
        QVERIFY(signal.supplied);
        QVERIFY(signal.valid);
        QString error;
        QVERIFY(signalBootstrapReady(signal, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(WaitForSingleObject(event, 0), DWORD(WAIT_OBJECT_0));
        CloseHandle(event);
#endif
    }

    void wrongHandleTypeFailsClosed()
    {
#ifdef Q_OS_WIN
        SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE mutex = CreateMutexW(&security, FALSE, nullptr);
        QVERIFY(mutex != nullptr);
        const QString argument = QStringLiteral(
            "--yanami-bootstrap-ready-handle=%1")
                .arg(reinterpret_cast<quintptr>(mutex));
        const BootstrapReadySignal signal =
            bootstrapReadySignalFromArguments({argument});
        QVERIFY(signal.valid);
        QString error;
        QVERIFY(!signalBootstrapReady(signal, &error));
        QCOMPARE(error, QStringLiteral("ready_event_signal_failed"));
        CloseHandle(mutex);
#endif
    }

    void malformedOrDuplicateHandleIsRejected()
    {
        BootstrapReadySignal malformed = bootstrapReadySignalFromArguments({
            QStringLiteral("--yanami-bootstrap-ready-handle=not-a-handle")});
        QVERIFY(malformed.supplied);
        QVERIFY(!malformed.valid);

        BootstrapReadySignal duplicate = bootstrapReadySignalFromArguments({
            QStringLiteral("--yanami-bootstrap-ready-handle=1"),
            QStringLiteral("--yanami-bootstrap-ready-handle=2")});
        QVERIFY(duplicate.supplied);
        QVERIFY(!duplicate.valid);
    }
};

QTEST_GUILESS_MAIN(BootstrapReadySignalTests)

#include "BootstrapReadySignalTests.moc"
