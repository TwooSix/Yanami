#include "AsyncOperationState.hpp"

#include <QSignalSpy>
#include <QTest>

class AsyncOperationStateTests final : public QObject
{
    Q_OBJECT

private slots:
    void commitsOnlyMatchingMutation()
    {
        AsyncOperationState state;
        QVERIFY(state.begin(QStringLiteral("mutation-1")));
        QVERIFY(state.busy());
        QVERIFY(!state.resolve(QStringLiteral("stale"), 1));
        QVERIFY(state.resolve(QStringLiteral("mutation-1"), 2));
        QCOMPARE(state.phase(), AsyncOperationState::Phase::Succeeded);
        QCOMPARE(state.result().toInt(), 2);
    }

    void newerMutationFencesOlderFailure()
    {
        AsyncOperationState state;
        QVERIFY(state.begin(QStringLiteral("mutation-1")));
        QVERIFY(state.begin(QStringLiteral("mutation-2")));
        QVERIFY(!state.reject(QStringLiteral("mutation-1"), QStringLiteral("old")));
        QVERIFY(state.reject(QStringLiteral("mutation-2"), QStringLiteral("failed")));
        QCOMPARE(state.phase(), AsyncOperationState::Phase::Failed);
        QCOMPARE(state.errorMessage(), QStringLiteral("failed"));
    }
};

QTEST_MAIN(AsyncOperationStateTests)
#include "AsyncOperationStateTests.moc"
