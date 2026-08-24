#include <QBitArray>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include "DanmakuTimelineModel.hpp"

namespace {

QVariantMap comment(
    const QString &id,
    qreal time,
    const QString &mode = QStringLiteral("scroll"),
    const QString &text = QStringLiteral("comment"),
    int color = 0xffffff)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("time"), time},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("text"), text},
        {QStringLiteral("color"), color},
    };
}

QVariantList comments(
    int count,
    qreal start,
    qreal step = 0,
    const QString &prefix = QStringLiteral("comment"))
{
    QVariantList result;
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        result.push_back(comment(
            prefix + QString::number(index),
            start + index * step,
            QStringLiteral("scroll"),
            prefix + QStringLiteral(" text ") + QString::number(index)));
    }
    return result;
}

QSet<QString> activeIds(const DanmakuTimelineModel &model)
{
    QSet<QString> result;
    for (int row = 0; row < model.rowCount(); ++row)
        result.insert(model.get(row).value(QStringLiteral("commentId")).toString());
    return result;
}

} // namespace

class DanmakuTimelineModelTests final : public QObject
{
    Q_OBJECT

private slots:
    void stableTimeAndInputOrder()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.replaceComments({
            comment(QStringLiteral("third"), 8),
            comment(QStringLiteral("first-tie"), 4),
            comment(QStringLiteral("second-tie"), 4),
            comment(QStringLiteral("first"), 1),
        }, 1);

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);
        QCOMPARE(model.timelineCount(), 4);
        QCOMPARE(model.entryAt(0).value(QStringLiteral("commentId")).toString(),
                 QStringLiteral("first"));
        QCOMPARE(model.entryAt(1).value(QStringLiteral("commentId")).toString(),
                 QStringLiteral("first-tie"));
        QCOMPARE(model.entryAt(2).value(QStringLiteral("commentId")).toString(),
                 QStringLiteral("second-tie"));
        QCOMPARE(model.entryAt(3).value(QStringLiteral("commentId")).toString(),
                 QStringLiteral("third"));
    }

    void entryKeysAreStableAndDoNotDependOnCommentIdUniqueness()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.setLaneCount(64);
        model.setDensity(100);
        model.replaceComments({
            comment(QStringLiteral("duplicate"), 3),
            comment(QStringLiteral("duplicate"), 3),
        }, 8);

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);
        const QString firstKey = model.entryAt(0)
            .value(QStringLiteral("entryKey")).toString();
        const QString secondKey = model.entryAt(1)
            .value(QStringLiteral("entryKey")).toString();
        QVERIFY(!firstKey.isEmpty());
        QVERIFY(!secondKey.isEmpty());
        QVERIFY(firstKey != secondKey);

        model.syncActive(3, true);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.get(0).value(QStringLiteral("entryKey")).toString(), firstKey);
        QCOMPARE(model.get(1).value(QStringLiteral("entryKey")).toString(), secondKey);
        model.syncActive(3.1, false);
        QCOMPARE(model.get(0).value(QStringLiteral("entryKey")).toString(), firstKey);
        QCOMPARE(model.get(1).value(QStringLiteral("entryKey")).toString(), secondKey);
    }

    void latestWinsAndQueueStaysBounded()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);

        model.replaceComments(comments(100'000, 0, 0.01, QStringLiteral("old")), 9);
        const quint64 firstGeneration = model.generation();
        model.replaceComments({comment(QStringLiteral("middle"), 5)}, 9);
        QCOMPARE(model.pendingCount(), 1);
        model.replaceComments({comment(QStringLiteral("newest"), 7)}, 9);

        QCOMPARE(model.pendingCount(), 1);
        QCOMPARE(model.generation(), firstGeneration + 2);
        QVERIFY(model.preparing());

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 30'000);
        QCOMPARE(model.pendingCount(), 0);
        QCOMPARE(model.timelineCount(), 1);
        QCOMPARE(model.entryAt(0).value(QStringLiteral("commentId")).toString(),
                 QStringLiteral("newest"));
    }

    void sessionFenceRejectsStaleWorkerResult()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.setLaneCount(64);
        model.setDensity(1000);

        model.replaceComments(comments(80'000, 10, 0, QStringLiteral("stale")), 10);
        model.replaceComments({comment(QStringLiteral("current"), 10)}, 11);

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 30'000);
        QCOMPARE(model.sessionGeneration(), quint64(11));
        QCOMPARE(model.timelineCount(), 1);
        model.syncActive(10, true);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.get(0).value(QStringLiteral("commentId")).toString(),
                 QStringLiteral("current"));
    }

    void oneHundredThousandEntriesRemainACompactActiveWindow()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(3840);
        model.setLaneCount(64);
        model.setDensity(64);
        model.replaceComments(comments(100'000, 0, 0.1), 3);

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 30'000);
        QCOMPARE(model.timelineCount(), 100'000);
        model.syncActive(5'000, true);
        QVERIFY(model.rowCount() > 0);
        QVERIFY2(model.rowCount() < 500,
                 "the QML-facing model must contain only the active time window");
    }

    void everyEligibleEntrySurvivesBeyondTheOldThreeSixtyCap()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.setLaneCount(64);
        model.setDensity(1000);

        QVariantList source;
        source.reserve(500);
        for (int index = 0; index < 500; ++index) {
            source.push_back(comment(
                QString::number(index),
                10,
                QStringLiteral("scroll"),
                index % 10 == 0 ? QStringLiteral("blocked")
                                : QStringLiteral("allowed")));
        }
        model.replaceComments(source, 4);
        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);

        model.syncActive(9.99, true);
        QCOMPARE(model.rowCount(), 0);
        model.syncActive(10, true);
        QCOMPARE(model.rowCount(), 500);

        model.setBlockedTerms(QStringLiteral("blocked"));
        QCOMPARE(model.rowCount(), 450);
        model.setDensity(400);
        QCOMPARE(model.rowCount(), 360);
        model.setShowScroll(false);
        QCOMPARE(model.rowCount(), 0);
    }

    void laneFilterIsAppliedBeforeRowsArePublished()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(100);
        model.setDensity(100);
        model.setLaneCount(1);
        model.replaceComments({
            comment(QStringLiteral("lane-zero"), 2),
            comment(QStringLiteral("lane-one"), 2),
        }, 5);

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);
        model.syncActive(2, true);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.get(0).value(QStringLiteral("commentId")).toString(),
                 QStringLiteral("lane-zero"));
    }

    void seeksNeverRetainStaleOrDuplicateRows()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.setLaneCount(64);
        model.setDensity(1000);
        QVariantList source = comments(50, 10, 0, QStringLiteral("early"));
        source += comments(50, 100, 0, QStringLiteral("late"));
        model.replaceComments(source, 6);

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);
        model.syncActive(10, true);
        QCOMPARE(model.rowCount(), 50);
        QCOMPARE(activeIds(model).size(), 50);
        for (const QString &id : activeIds(model))
            QVERIFY(id.startsWith(QStringLiteral("early")));

        model.syncActive(100, false);
        QCOMPARE(model.rowCount(), 50);
        QCOMPARE(activeIds(model).size(), 50);
        for (const QString &id : activeIds(model))
            QVERIFY(id.startsWith(QStringLiteral("late")));

        model.syncActive(10, false);
        QCOMPARE(model.rowCount(), 50);
        QCOMPARE(activeIds(model).size(), 50);
        for (const QString &id : activeIds(model))
            QVERIFY(id.startsWith(QStringLiteral("early")));
    }

    void smallClockCorrectionRewindsTheActiveCursorExactly()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.setLaneCount(64);
        model.setDensity(100);
        model.replaceComments({
            comment(QStringLiteral("already-active"), 0.9),
            comment(QStringLiteral("boundary"), 1.0),
        }, 13);
        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);

        model.syncActive(1.00, true);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(activeIds(model).size(), 2);

        // A normal clock correction can move the predicted time backwards by
        // much less than the seek threshold. The boundary entry must leave the
        // exact active set, and the timeline cursor must rewind with it.
        model.syncActive(0.95, false);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(activeIds(model), QSet<QString> {QStringLiteral("already-active")});

        model.syncActive(1.01, false);
        QCOMPARE(model.rowCount(), 2);
        const QSet<QString> restored = activeIds(model);
        QCOMPARE(restored.size(), 2);
        QVERIFY(restored.contains(QStringLiteral("already-active")));
        QVERIFY(restored.contains(QStringLiteral("boundary")));
    }

    void continuousPlaybackUsesDifferentialRows()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.setLaneCount(64);
        model.setDensity(100);
        model.replaceComments({
            comment(QStringLiteral("one"), 1),
            comment(QStringLiteral("two"), 2),
        }, 12);
        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);

        model.syncActive(1, true);
        QCOMPARE(model.rowCount(), 1);
        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        QSignalSpy insertSpy(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removeSpy(&model, &QAbstractItemModel::rowsRemoved);

        model.syncActive(2, false);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(insertSpy.count(), 1);
        QCOMPARE(removeSpy.count(), 0);

        for (int position = 3; position <= 10; ++position)
            model.syncActive(position, false);
        model.syncActive(10.1, false);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(removeSpy.count(), 1);
    }

    void onlyWidthOrCommentsRelayoutExistingLanes()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(100);
        model.setFontSize(10);
        model.setScrollDuration(9);
        model.replaceComments({
            comment(QStringLiteral("long"), 0, QStringLiteral("scroll"),
                    QString(20, QLatin1Char('W'))),
            comment(QStringLiteral("later"), 5),
        }, 7);

        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);
        QCOMPARE(model.entryAt(1).value(QStringLiteral("lane")).toInt(), 1);
        const quint64 laidOutGeneration = model.generation();
        const QString stableEntryKey = model.entryAt(1)
            .value(QStringLiteral("entryKey")).toString();

        model.setFontSize(20);
        model.setScrollDuration(3);
        QCOMPARE(model.generation(), laidOutGeneration);
        QCOMPARE(model.entryAt(1).value(QStringLiteral("lane")).toInt(), 1);

        model.setViewportWidth(1000);
        QVERIFY(model.preparing());
        QCOMPARE(model.timelineCount(), 2);
        QCOMPARE(model.entryAt(1).value(QStringLiteral("lane")).toInt(), 1);
        QCOMPARE(model.entryAt(1).value(QStringLiteral("entryKey")).toString(),
                 stableEntryKey);
        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);
        QCOMPARE(model.generation(), laidOutGeneration + 1);
        QCOMPARE(model.entryAt(1).value(QStringLiteral("lane")).toInt(), 0);
        QCOMPARE(model.entryAt(1).value(QStringLiteral("entryKey")).toString(),
                 stableEntryKey);
    }

    void explicitSessionChangeImmediatelyClearsOldRowsAndPendingWork()
    {
        DanmakuTimelineModel model;
        model.setViewportWidth(1920);
        model.setLaneCount(64);
        model.setDensity(1000);
        model.replaceComments({comment(QStringLiteral("old"), 1)}, 20);
        QTRY_VERIFY_WITH_TIMEOUT(!model.preparing(), 10'000);
        model.syncActive(1, true);
        QCOMPARE(model.rowCount(), 1);

        model.setSessionGeneration(21);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.timelineCount(), 0);
        QCOMPARE(model.pendingCount(), 0);
        QVERIFY(!model.preparing());
        QVERIFY(model.comments().isEmpty());
    }
};

QTEST_GUILESS_MAIN(DanmakuTimelineModelTests)

#include "DanmakuTimelineModelTests.moc"
