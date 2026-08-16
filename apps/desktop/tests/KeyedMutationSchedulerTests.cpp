#include "KeyedMutationScheduler.hpp"

#include <QTest>

namespace {

struct Request
{
    QString itemId;
    int ordinal = 0;
    quint64 sessionGeneration = 0;
};

KeyedMutationScheduler<Request> scheduler()
{
    return KeyedMutationScheduler<Request>(
        2,
        [](const Request &request) { return request.itemId; });
}

} // namespace

class KeyedMutationSchedulerTests final : public QObject
{
    Q_OBJECT

private slots:
    void sameItemIsStrictFifo()
    {
        auto subject = scheduler();
        QVERIFY(subject.enqueue({QStringLiteral("A"), 1, 7}));
        QVERIFY(subject.enqueue({QStringLiteral("A"), 2, 7}));
        QVERIFY(subject.enqueue({QStringLiteral("A"), 3, 7}));

        QVector<Request> ready = subject.takeReady();
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.front().ordinal, 1);
        QCOMPARE(subject.activeCount(), 1);

        QVERIFY(subject.complete(QStringLiteral("A")));
        ready = subject.takeReady();
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.front().ordinal, 2);

        QVERIFY(subject.complete(QStringLiteral("A")));
        ready = subject.takeReady();
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.front().ordinal, 3);
    }

    void differentItemsUseBothSlotsWithoutHeadOfLineBlocking()
    {
        auto subject = scheduler();
        subject.enqueue({QStringLiteral("A"), 1, 3});
        subject.enqueue({QStringLiteral("A"), 2, 3});
        subject.enqueue({QStringLiteral("B"), 3, 3});
        subject.enqueue({QStringLiteral("C"), 4, 3});

        QVector<Request> ready = subject.takeReady();
        QCOMPARE(ready.size(), 2);
        QCOMPARE(ready[0].ordinal, 1);
        QCOMPARE(ready[1].ordinal, 3);
        QCOMPARE(subject.activeCount(), 2);
        QVERIFY(subject.active(QStringLiteral("A")));
        QVERIFY(subject.active(QStringLiteral("B")));

        // B finishing frees C before the second A request. A2 remains fenced
        // by A1 even though another execution slot is available.
        QVERIFY(subject.complete(QStringLiteral("B")));
        ready = subject.takeReady();
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.front().ordinal, 4);
        QVERIFY(!subject.active(QStringLiteral("B")));
        QVERIFY(subject.active(QStringLiteral("C")));
    }

    void sessionResetDiscardsQueuedAndMarksActiveAsStale()
    {
        auto subject = scheduler();
        subject.enqueue({QStringLiteral("A"), 1, 11});
        subject.enqueue({QStringLiteral("A"), 2, 11});
        subject.enqueue({QStringLiteral("B"), 3, 11});
        const QVector<Request> started = subject.takeReady();
        QCOMPARE(started.size(), 2);

        constexpr quint64 currentSessionGeneration = 12;
        const QVector<Request> dropped = subject.takeQueuedIf(
            [](const Request &request) {
                return request.sessionGeneration != currentSessionGeneration;
            });
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(dropped.front().ordinal, 2);
        QCOMPARE(subject.queuedCount(), 0);

        const QVector<Request> active = subject.activeRequests();
        QCOMPARE(active.size(), 2);
        for (const Request &request : active)
            QVERIFY(request.sessionGeneration != currentSessionGeneration);

        // The transport may still finish, but the owner can suppress it using
        // the captured generation. Completion only releases the keyed slot.
        QVERIFY(subject.complete(QStringLiteral("A")));
        QVERIFY(subject.takeReady().isEmpty());
        subject.enqueue({QStringLiteral("A"), 4, currentSessionGeneration});
        const QVector<Request> next = subject.takeReady();
        QCOMPARE(next.size(), 1);
        QCOMPARE(next.front().ordinal, 4);
        QCOMPARE(next.front().sessionGeneration, currentSessionGeneration);
    }
};

QTEST_APPLESS_MAIN(KeyedMutationSchedulerTests)

#include "KeyedMutationSchedulerTests.moc"
