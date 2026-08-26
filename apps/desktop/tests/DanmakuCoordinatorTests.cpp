#include "BackendInfrastructure.hpp"
#include "DanmakuCoordinator.hpp"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <functional>

namespace {

class RecordingStatusSink final : public StatusSink
{
public:
    void publishStatus(const QString &message, bool error) override
    {
        messages.push_back(message);
        errors.push_back(error);
    }

    QString userFacingBackendError(
        const QString &,
        const QString & = {}) const override
    {
        return QStringLiteral("backend failure");
    }

    QString userFacingDanmakuError(
        const QString &,
        const QString & = {}) const override
    {
        return QStringLiteral("danmaku failure");
    }

    QStringList messages;
    QList<bool> errors;
};

QByteArray loadedPayload(int commentCount)
{
    QJsonArray comments;
    for (int index = 0; index < commentCount; ++index) {
        comments.append(QJsonObject {
            {QStringLiteral("id"), index},
            {QStringLiteral("time"), index / 10.0},
            {QStringLiteral("mode"), index % 3},
            {QStringLiteral("color"), 0x00ffffff - index},
            {QStringLiteral("text"),
                QStringLiteral("comment-%1").arg(index)},
        });
    }
    return QJsonDocument(QJsonObject {
        {QStringLiteral("schemaVersion"), 8},
        {QStringLiteral("status"), QStringLiteral("loaded")},
        {QStringLiteral("title"), QStringLiteral("Large fixture")},
        {QStringLiteral("commentCount"), commentCount},
        {QStringLiteral("stale"), false},
        {QStringLiteral("matchedTimeOffset"), 1.25},
        {QStringLiteral("comments"), comments},
    }).toJson(QJsonDocument::Compact);
}

QByteArray searchPayload(const QString &title)
{
    return QJsonDocument(QJsonObject {
        {QStringLiteral("schemaVersion"), 8},
        {QStringLiteral("status"), QStringLiteral("search-results")},
        {QStringLiteral("animes"), QJsonArray {
            QJsonObject {
                {QStringLiteral("animeId"), 42},
                {QStringLiteral("animeTitle"), title},
            },
        }},
    }).toJson(QJsonDocument::Compact);
}

DanmakuCoordinator::BackendOperations backendOperations(
    std::function<YanamiOperationResult(
        DanmakuPort::Operation,
        const QString &,
        const QVariantMap &)> danmaku)
{
    return {
        [] { return true; },
        [] { return YanamiStatusResult {0, {}, {}}; },
        [](const QString &, const QString &) {
            return YanamiOperationResult {0, {}, {}, {}};
        },
        [] { return YanamiOperationResult {0, {}, {}, {}}; },
        std::move(danmaku),
    };
}

} // namespace

class DanmakuCoordinatorTests final : public QObject
{
    Q_OBJECT

private slots:
    void largePayloadIsDecodedOffTheGuiThread()
    {
        constexpr int commentCount = 40'000;
        const QByteArray payload = loadedPayload(commentCount);
        QThread *const guiThread = QThread::currentThread();
        std::atomic<QThread *> workerThread = nullptr;

        QThreadPool queryPool;
        QThreadPool mutationPool;
        queryPool.setMaxThreadCount(1);
        mutationPool.setMaxThreadCount(1);
        RecordingStatusSink statusSink;
        DanmakuCoordinator coordinator(
            backendOperations(
                [payload, &workerThread](DanmakuPort::Operation,
                    const QString &,
                    const QVariantMap &) {
                    workerThread.store(
                        QThread::currentThread(),
                        std::memory_order_release);
                    return YanamiOperationResult {
                        0, payload, {}, {},
                    };
                }),
            queryPool,
            mutationPool,
            [] {
                return DanmakuCoordinator::SessionState {1, true};
            },
            statusSink);
        QVERIFY(coordinator.initializeCredentialStatus());

        bool completed = false;
        QThread *completionThread = nullptr;
        QVariantMap delivered;
        qint64 receiverElapsedNs = -1;
        connect(&coordinator, &DanmakuPort::operationCompleted,
            this,
            [&](const QString &, const QString &,
                DanmakuPort::Operation, const QVariantMap &result) {
                QElapsedTimer receiverTimer;
                receiverTimer.start();
                completionThread = QThread::currentThread();
                delivered = result;
                completed = true;
                receiverElapsedNs = receiverTimer.nsecsElapsed();
            });

        int heartbeatCount = 0;
        QTimer heartbeat;
        heartbeat.setInterval(0);
        connect(&heartbeat, &QTimer::timeout,
            this, [&heartbeatCount] { ++heartbeatCount; });
        heartbeat.start();
        coordinator.loadAutomatically(
            QStringLiteral("large-request"),
            QStringLiteral("item-large"));
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15'000);
        heartbeat.stop();

        QCOMPARE(workerThread.load(std::memory_order_acquire) == guiThread,
            false);
        QCOMPARE(completionThread, guiThread);
        QCOMPARE(delivered.value(QStringLiteral("schemaVersion")).isValid(),
            false);
        QCOMPARE(delivered.value(QStringLiteral("status")).toString(),
            QStringLiteral("loaded"));
        QCOMPARE(delivered.value(QStringLiteral("commentCount")).toInt(),
            commentCount);
        const QVariantList comments =
            delivered.value(QStringLiteral("comments")).toList();
        QCOMPARE(comments.size(), commentCount);
        QCOMPARE(comments.constFirst().toMap()
                .value(QStringLiteral("text")).toString(),
            QStringLiteral("comment-0"));
        QCOMPARE(comments.constLast().toMap()
                .value(QStringLiteral("text")).toString(),
            QStringLiteral("comment-39999"));
        QVERIFY(heartbeatCount > 0);
        QVERIFY(receiverElapsedNs >= 0);
        qInfo() << "danmaku large-payload test"
                << "bytes=" << payload.size()
                << "comments=" << commentCount
                << "guiHeartbeats=" << heartbeatCount
                << "receiverNs=" << receiverElapsedNs;
    }

    void staleSessionCompletionNeverCommitsDecodedPayload()
    {
        constexpr int commentCount = 20'000;
        const QByteArray payload = loadedPayload(commentCount);
        std::atomic<quint64> generation = 10;
        QSemaphore workerEntered;
        QSemaphore allowWorkerToReturn;

        QThreadPool queryPool;
        QThreadPool mutationPool;
        queryPool.setMaxThreadCount(1);
        mutationPool.setMaxThreadCount(1);
        RecordingStatusSink statusSink;
        DanmakuCoordinator coordinator(
            backendOperations(
                [payload, &workerEntered, &allowWorkerToReturn](
                    DanmakuPort::Operation,
                    const QString &,
                    const QVariantMap &) {
                    workerEntered.release();
                    allowWorkerToReturn.acquire();
                    return YanamiOperationResult {
                        0, payload, {}, {},
                    };
                }),
            queryPool,
            mutationPool,
            [&generation] {
                return DanmakuCoordinator::SessionState {
                    generation.load(std::memory_order_acquire), true,
                };
            },
            statusSink);
        QVERIFY(coordinator.initializeCredentialStatus());

        QSignalSpy completed(
            &coordinator, &DanmakuPort::operationCompleted);
        QSignalSpy failed(
            &coordinator, &DanmakuPort::operationFailed);
        coordinator.loadAutomatically(
            QStringLiteral("stale-request"),
            QStringLiteral("item-stale"));
        QVERIFY(workerEntered.tryAcquire(1, 5'000));
        generation.store(11, std::memory_order_release);
        allowWorkerToReturn.release();

        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 15'000);
        QCOMPARE(completed.size(), 0);
        const QList<QVariant> failure = failed.takeFirst();
        QCOMPARE(failure.at(0).toString(), QStringLiteral("stale-request"));
        QCOMPARE(failure.at(1).toString(), QStringLiteral("item-stale"));
        QCOMPARE(failure.at(3).toString(),
            QStringLiteral(
                "The action was canceled because the Emby session changed."));
        QCOMPARE(failure.at(4).toBool(), false);
    }

    void latestSearchStillWinsWhenOldDecodeFinishesLater()
    {
        QSemaphore firstWorkerEntered;
        QSemaphore releaseFirstWorker;
        std::atomic<int> calls = 0;

        QThreadPool queryPool;
        QThreadPool mutationPool;
        queryPool.setMaxThreadCount(1);
        mutationPool.setMaxThreadCount(1);
        RecordingStatusSink statusSink;
        DanmakuCoordinator coordinator(
            backendOperations(
                [&firstWorkerEntered, &releaseFirstWorker, &calls](
                    DanmakuPort::Operation,
                    const QString &,
                    const QVariantMap &payload) {
                    const int call = calls.fetch_add(
                        1, std::memory_order_acq_rel);
                    if (call == 0) {
                        firstWorkerEntered.release();
                        releaseFirstWorker.acquire();
                    }
                    return YanamiOperationResult {
                        0,
                        searchPayload(payload.value(
                            QStringLiteral("anime")).toString()),
                        {},
                        {},
                    };
                }),
            queryPool,
            mutationPool,
            [] {
                return DanmakuCoordinator::SessionState {3, true};
            },
            statusSink);
        QVERIFY(coordinator.initializeCredentialStatus());

        QSignalSpy completed(
            &coordinator, &DanmakuPort::operationCompleted);
        QSignalSpy failed(
            &coordinator, &DanmakuPort::operationFailed);
        coordinator.search(
            QStringLiteral("old-request"),
            QStringLiteral("item"),
            QStringLiteral("Old"));
        QVERIFY(firstWorkerEntered.tryAcquire(1, 5'000));
        coordinator.search(
            QStringLiteral("new-request"),
            QStringLiteral("item"),
            QStringLiteral("New"));
        QCOMPARE(failed.size(), 1);
        releaseFirstWorker.release();

        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10'000);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(calls.load(std::memory_order_acquire), 2);
        const QList<QVariant> completion = completed.takeFirst();
        QCOMPARE(completion.at(0).toString(),
            QStringLiteral("new-request"));
        const QVariantList animes = completion.at(3).toMap()
            .value(QStringLiteral("animes")).toList();
        QCOMPARE(animes.size(), 1);
        QCOMPARE(animes.constFirst().toMap()
                .value(QStringLiteral("animeTitle")).toString(),
            QStringLiteral("New"));
        const QList<QVariant> superseded = failed.takeFirst();
        QCOMPARE(superseded.at(0).toString(),
            QStringLiteral("old-request"));
        QCOMPARE(superseded.at(3).toString(),
            QStringLiteral("The request was superseded."));
        QCOMPARE(superseded.at(4).toBool(), true);
    }

    void invalidPayloadKeepsModalAndStatusContract()
    {
        QThreadPool queryPool;
        QThreadPool mutationPool;
        queryPool.setMaxThreadCount(1);
        mutationPool.setMaxThreadCount(1);
        RecordingStatusSink statusSink;
        DanmakuCoordinator coordinator(
            backendOperations(
                [](DanmakuPort::Operation,
                    const QString &,
                    const QVariantMap &) {
                    return YanamiOperationResult {
                        0, QByteArrayLiteral("{}"), {}, {},
                    };
                }),
            queryPool,
            mutationPool,
            [] {
                return DanmakuCoordinator::SessionState {7, true};
            },
            statusSink);
        QVERIFY(coordinator.initializeCredentialStatus());

        QSignalSpy failed(
            &coordinator, &DanmakuPort::operationFailed);
        coordinator.search(
            QStringLiteral("invalid-search"),
            QStringLiteral("item"),
            QStringLiteral("Query"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5'000);
        QList<QVariant> failure = failed.takeFirst();
        QCOMPARE(failure.at(3).toString(),
            QStringLiteral(
                "The danmaku service returned an invalid response."));
        QCOMPARE(failure.at(4).toBool(), false);
        QCOMPARE(statusSink.messages.size(), 1);
        QCOMPARE(statusSink.messages.constFirst(), failure.at(3).toString());
        QCOMPARE(statusSink.errors.constFirst(), true);

        statusSink.messages.clear();
        statusSink.errors.clear();
        coordinator.loadAutomatically(
            QStringLiteral("invalid-automatic"),
            QStringLiteral("item"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5'000);
        failure = failed.takeFirst();
        QCOMPARE(failure.at(3).toString(),
            QStringLiteral(
                "The danmaku service returned an invalid response."));
        QCOMPARE(failure.at(4).toBool(), true);
        QCOMPARE(statusSink.messages.size(), 0);
        QCOMPARE(statusSink.errors.size(), 0);
    }
};

QTEST_MAIN(DanmakuCoordinatorTests)

#include "DanmakuCoordinatorTests.moc"
