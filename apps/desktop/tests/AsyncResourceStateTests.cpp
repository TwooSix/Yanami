#include "AsyncResourceState.hpp"

#include <QSignalSpy>
#include <QTest>

class AsyncResourceStateTests final : public QObject
{
    Q_OBJECT

private slots:
    void staleWhileRevalidateKeepsCachedDataOnFailure()
    {
        AsyncResourceState state;
        const quint64 first = state.begin(QStringLiteral("images:item-1"), 3, 7, false);
        QCOMPARE(state.phase(), AsyncResourceState::Phase::Loading);
        QVERIFY(state.resolve(first, QStringLiteral("images:item-1"), 3, 7,
            QVariantMap{{QStringLiteral("title"), QStringLiteral("cached")}}));

        const quint64 refresh = state.begin(QStringLiteral("images:item-1"), 3, 7, true);
        QCOMPARE(state.phase(), AsyncResourceState::Phase::Refreshing);
        QVERIFY(state.stale());
        QVERIFY(state.reject(refresh, QStringLiteral("images:item-1"), 3, 7,
            QStringLiteral("offline")));
        QCOMPARE(state.phase(), AsyncResourceState::Phase::Ready);
        QVERIFY(state.hasData());
        QVERIFY(state.stale());
        QCOMPARE(state.errorMessage(), QStringLiteral("offline"));
    }

    void detachedAndSupersededResultsAreDiscarded()
    {
        AsyncResourceState state;
        const quint64 itemA = state.begin(QStringLiteral("images:A"), 1, 1, false);
        state.detach();
        QVERIFY(!state.resolve(itemA, QStringLiteral("images:A"), 1, 1,
            QVariantMap{{QStringLiteral("id"), QStringLiteral("A")}}));

        const quint64 itemB = state.begin(QStringLiteral("images:B"), 1,
            state.viewGeneration(), false);
        QVERIFY(!state.resolve(itemA, QStringLiteral("images:A"), 1, 1,
            QVariantMap{{QStringLiteral("id"), QStringLiteral("A")}}));
        QVERIFY(state.resolve(itemB, QStringLiteral("images:B"), 1,
            state.viewGeneration(),
            QVariantMap{{QStringLiteral("id"), QStringLiteral("B")}}));
        QCOMPARE(state.data().toMap().value(QStringLiteral("id")).toString(),
            QStringLiteral("B"));
    }

    void retryIsAnIntentNotAnImplicitRequest()
    {
        AsyncResourceState state;
        QSignalSpy retrySpy(&state, &AsyncResourceState::retryRequested);
        state.retry();
        QCOMPARE(retrySpy.count(), 1);
        QCOMPARE(state.phase(), AsyncResourceState::Phase::Idle);
    }
};

QTEST_MAIN(AsyncResourceStateTests)
#include "AsyncResourceStateTests.moc"
