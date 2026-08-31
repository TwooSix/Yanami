#include "UpdateHelperProcess.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <csignal>
#endif

#include <atomic>
#include <cstdio>
#include <memory>

namespace {
const QString fixtureFlag = QStringLiteral("--transport-fixture");

QStringList fixtureArguments(const QString &mode)
{
    return {fixtureFlag, mode};
}

bool processIsRunning(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process)
        return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

qint64 outputPid(const QByteArray &output)
{
    if (!output.startsWith("pid="))
        return 0;
    return output.sliced(4).trimmed().toLongLong();
}

int runFixture(const QString &mode)
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (mode == QStringLiteral("chunks")) {
        std::fputs("first\n", stdout);
        std::fflush(stdout);
        QThread::msleep(150);
        // Fill more than a pipe buffer without contaminating protocol stdout.
        const QByteArray noise(256 * 1024, 'x');
        std::fwrite(noise.constData(), 1, size_t(noise.size()), stderr);
        std::fflush(stderr);
        std::fputs("last-without-newline", stdout);
        return 7;
    }
    if (mode == QStringLiteral("success")) {
        std::fputs("done", stdout);
        return 0;
    }
    if (mode == QStringLiteral("hang")) {
        std::fprintf(stdout, "pid=%lld\n",
            static_cast<long long>(QCoreApplication::applicationPid()));
        std::fflush(stdout);
        QThread::msleep(10000);
        return 0;
    }
    if (mode == QStringLiteral("flood")) {
        const QByteArray chunk(64 * 1024, 'x');
        for (int index = 0; index < 128; ++index) {
            std::fwrite(chunk.constData(), 1, size_t(chunk.size()), stdout);
            std::fflush(stdout);
        }
        return 0;
    }
    return 2;
}
}

class UpdateHelperProcessTests final : public QObject
{
    Q_OBJECT

private slots:
    void streamsOutputAndDeliversTailBeforeCompletion()
    {
        UpdateHelperProcess process;
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        QByteArray received;
        QByteArray atCompletion;
        bool sawStreamingOutput = false;
        bool callbacksOnOwningThread = true;
        connect(&process, &UpdateHelperProcess::outputReady, this,
            [&](const QByteArray &output) {
                callbacksOnOwningThread &= QThread::currentThread() == thread();
                received += output;
                if (received == QByteArrayLiteral("first\n"))
                    sawStreamingOutput = finished.isEmpty();
            });
        connect(&process, &UpdateHelperProcess::finished, this,
            [&](int, bool) {
                callbacksOnOwningThread &= QThread::currentThread() == thread();
                atCompletion = received;
            });

        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("chunks")), 3000);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(sawStreamingOutput);
        QVERIFY(callbacksOnOwningThread);
        QCOMPARE(received, QByteArrayLiteral("first\nlast-without-newline"));
        QCOMPARE(atCompletion, received);
        QCOMPARE(finished.first().at(0).toInt(), 7);
        QCOMPARE(finished.first().at(1).toBool(), false);
    }

    void failedStartCompletesExactlyOnce()
    {
        UpdateHelperProcess process;
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        process.start(QStringLiteral("/yanami-missing-helper-does-not-exist"),
            {}, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toInt(), -1);
        QCOMPARE(finished.first().at(1).toBool(), true);
        QTest::qWait(100);
        QCOMPARE(finished.size(), 1);
    }

    void ignoresDuplicateStart()
    {
        UpdateHelperProcess process;
        QSignalSpy output(&process, &UpdateHelperProcess::outputReady);
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("success")));
        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("hang")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toInt(), 0);
        QCOMPARE(finished.first().at(1).toBool(), false);
        QCOMPARE(output.size(), 1);
        QCOMPARE(output.first().at(0).toByteArray(), QByteArrayLiteral("done"));
    }

    void cancellationReapsHelper()
    {
        UpdateHelperProcess process;
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        QByteArray received;
        connect(&process, &UpdateHelperProcess::outputReady, this,
            [&](const QByteArray &output) { received += output; });
        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("hang")));
        QTRY_VERIFY_WITH_TIMEOUT(outputPid(received) > 0, 3000);
        const qint64 pid = outputPid(received);
        QVERIFY(processIsRunning(pid));

        QElapsedTimer elapsed;
        elapsed.start();
        process.cancel();
        QVERIFY2(elapsed.elapsed() < 100,
            "Cancellation must not wait on process or thread termination");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toInt(), -1);
        QCOMPARE(finished.first().at(1).toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(!processIsRunning(pid), 3000);
    }

    void deadlineTerminatesHelper()
    {
        UpdateHelperProcess process;
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        QByteArray received;
        connect(&process, &UpdateHelperProcess::outputReady, this,
            [&](const QByteArray &output) { received += output; });
        QElapsedTimer elapsed;
        elapsed.start();
        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("hang")), 200);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toInt(), -1);
        QCOMPARE(finished.first().at(1).toBool(), true);
        QVERIFY(elapsed.elapsed() < 3000);
        if (const qint64 pid = outputPid(received); pid > 0)
            QTRY_VERIFY_WITH_TIMEOUT(!processIsRunning(pid), 3000);
    }

    void slowLaunchDoesNotBlockOwningThread()
    {
        auto enteredLaunch = std::make_shared<std::atomic_bool>(false);
        auto correctThread = std::make_shared<std::atomic_bool>(false);
        QThread *const ownerThread = thread();
        UpdateHelperProcess process(
            [enteredLaunch, correctThread, ownerThread](QProcess &helper) {
                correctThread->store(QThread::currentThread() != ownerThread
                    && helper.thread() == QThread::currentThread());
                enteredLaunch->store(true);
                QThread::msleep(300);
                helper.start();
            }, nullptr);
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        int heartbeatCount = 0;
        QTimer heartbeat;
        heartbeat.setTimerType(Qt::PreciseTimer);
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++heartbeatCount; });
        heartbeat.start();

        QElapsedTimer elapsed;
        elapsed.start();
        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("success")), 3000);
        QVERIFY2(elapsed.elapsed() < 100,
            "start() must return before synchronous OS process creation");
        QTRY_VERIFY_WITH_TIMEOUT(enteredLaunch->load(), 1000);
        QTest::qWait(150);
        QVERIFY(heartbeatCount >= 5);
        QVERIFY(finished.isEmpty());
        QVERIFY(correctThread->load());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.first().at(0).toInt(), 0);
    }

    void deadlineIncludesSynchronousLaunchTime()
    {
        auto childPid = std::make_shared<std::atomic<qint64>>(0);
        UpdateHelperProcess process([childPid](QProcess &helper) {
            QThread::msleep(200);
            helper.start();
            helper.waitForStarted(1000);
            childPid->store(helper.processId());
        }, nullptr);
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("hang")), 100);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toInt(), -1);
        QCOMPARE(finished.first().at(1).toBool(), true);
        QVERIFY(childPid->load() > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!processIsRunning(childPid->load()), 3000);
    }

    void destructionDuringLaunchDoesNotWaitAndReapsHelper()
    {
        auto enteredLaunch = std::make_shared<std::atomic_bool>(false);
        auto releaseLaunch = std::make_shared<std::atomic_bool>(false);
        auto childPid = std::make_shared<std::atomic<qint64>>(0);
        auto launchReturned = std::make_shared<std::atomic_bool>(false);
        auto process = std::unique_ptr<UpdateHelperProcess>(
            new UpdateHelperProcess(
                [enteredLaunch, releaseLaunch, childPid, launchReturned](
                    QProcess &helper) {
                    enteredLaunch->store(true);
                    QElapsedTimer guard;
                    guard.start();
                    while (!releaseLaunch->load() && guard.elapsed() < 3000)
                        QThread::msleep(5);
                    helper.start();
                    helper.waitForStarted(1000);
                    childPid->store(helper.processId());
                    launchReturned->store(true);
                }, nullptr));
        process->start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("hang")));
        QTRY_VERIFY_WITH_TIMEOUT(enteredLaunch->load(), 1000);

        QElapsedTimer elapsed;
        elapsed.start();
        process.reset();
        const qint64 destructionMs = elapsed.elapsed();
        releaseLaunch->store(true);
        QVERIFY2(destructionMs < 100,
            "Destruction must not join a thread blocked inside process start");
        QTRY_VERIFY_WITH_TIMEOUT(launchReturned->load(), 5000);
        const qint64 pid = childPid->load();
        QVERIFY(pid > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!processIsRunning(pid), 3000);
    }

    void boundsRetainedProtocolOutput()
    {
        UpdateHelperProcess process;
        QSignalSpy finished(&process, &UpdateHelperProcess::finished);
        qsizetype receivedBytes = 0;
        connect(&process, &UpdateHelperProcess::outputReady, this,
            [&](const QByteArray &output) { receivedBytes += output.size(); });
        process.start(QCoreApplication::applicationFilePath(),
            fixtureArguments(QStringLiteral("flood")), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 7000);
        QVERIFY(receivedBytes <= 4 * 1024 * 1024);
        QCOMPARE(finished.first().at(0).toInt(), -1);
        QCOMPARE(finished.first().at(1).toBool(), true);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 3 && arguments.at(1) == fixtureFlag)
        return runFixture(arguments.at(2));
    UpdateHelperProcessTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "UpdateHelperProcessTests.moc"
