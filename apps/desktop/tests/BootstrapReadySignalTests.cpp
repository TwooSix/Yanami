#include "BootstrapReadySignal.hpp"

#include <QtTest>

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
